#!/usr/bin/env python3
"""RTC 写帧构造单元测试: [寄存器指针][数据...] 字节级断言
防 DS3231 帧错位类 bug (审计抓出过: 缺指针+秒无数据)"""
import json
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "esp32", "main")
HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "rtc.h"
#include "rtc_core.h"
int main(void) {
    while (1) {
        RtcTime t;
        int year, mon, mday, hour, min, sec;
        if (scanf("%d %d %d %d %d %d", &year, &mon, &mday, &hour, &min, &sec) != 6) break;
        t.year = year; t.mon = mon; t.mday = mday;
        t.hour = hour; t.min = min; t.sec = sec;
        uint8_t first[7], secf[2];
        rtc_core_build(&t, first, secf);
        printf("{\"first\":[%d,%d,%d,%d,%d,%d,%d],\"sec\":[%d,%d]}\n",
               first[0], first[1], first[2], first[3], first[4], first[5], first[6],
               secf[0], secf[1]);
    }
    return 0;
}
"""

# (年,月,日,时,分,秒, 期望first帧[7], 期望sec帧[2])
CASES = [
    # 常规: 2026-08-29 13:05:07
    (2026, 8, 29, 13, 5, 7,
     [0x01, 0x05, 0x13, 0x01, 0x29, 0x08, 0x26], [0x00, 0x07]),
    # 23时/59分/59秒 (BCD边界)
    (2026, 8, 29, 23, 59, 59,
     [0x01, 0x59, 0x23, 0x01, 0x29, 0x08, 0x26], [0x00, 0x59]),
    # 世纪位: 2100年 → 月寄存器BIT7置位
    (2100, 1, 1, 0, 0, 0,
     [0x01, 0x00, 0x00, 0x01, 0x01, 0x81, 0x00], [0x00, 0x00]),
    # 2099年 → 世纪位不置
    (2099, 12, 31, 23, 59, 58,
     [0x01, 0x59, 0x23, 0x01, 0x31, 0x12, 0x99], [0x00, 0x58]),
    # 2026-01-31 00:00:00
    (2026, 1, 31, 0, 0, 0,
     [0x01, 0x00, 0x00, 0x01, 0x31, 0x01, 0x26], [0x00, 0x00]),
]


def build():
    os.makedirs("/tmp/opencode", exist_ok=True)
    with open("/tmp/opencode/rtc_test.c", "w") as fp:
        fp.write(HARNESS)
    r = subprocess.run(["gcc", "-O2", "-I", ROOT, "-o", "/tmp/opencode/rtc_test",
                        "/tmp/opencode/rtc_test.c",
                        os.path.join(ROOT, "rtc_core.c")],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def main():
    ok, err = build()
    if not ok:
        print("C编译失败:\n", err)
        return 1
    inp = "\n".join(f"{c[0]} {c[1]} {c[2]} {c[3]} {c[4]} {c[5]}" for c in CASES) + "\n"
    out = subprocess.run(["/tmp/opencode/rtc_test"], input=inp,
                         capture_output=True, text=True).stdout.strip().splitlines()

    passed = 0
    for case, line in zip(CASES, out):
        _, _, _, _, _, _, exp_first, exp_sec = case
        got = json.loads(line)
        ok = got["first"] == exp_first and got["sec"] == exp_sec
        print(f"[{'PASS' if ok else 'FAIL'}] {case[:6]} first={got['first']} sec={got['sec']}")
        passed += ok
    print(f"\n结果: {passed}/{len(CASES)} 通过")
    return 0 if passed == len(CASES) else 1


if __name__ == "__main__":
    sys.exit(main())
