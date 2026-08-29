#!/usr/bin/env python3
"""GNSS $GNRMC 解析器单元测试(交叉验证 C 实现 vs 独立Python解析)"""
import datetime
import json
import subprocess
import sys
import os

C_SRC = os.path.join(os.path.dirname(__file__), "..", "esp32", "main", "gnss.c")
C_HEAD = os.path.join(os.path.dirname(__file__), "..", "esp32", "main", "gnss.h")

CASES = [
    # (行, 期望lat, 期望lon, 期望UTC ts, 期望返回码)
    ("$GNRMC,084852.000,A,2236.9453,N,11408.4790,E,0.53,292.44,141216,,,A*75",
     22.615755, 114.141317, 1481705332, 0),
    ("$GNRMC,120000.000,V,0000.0000,N,00000.0000,E,0.0,0.0,010126,,,N*6F",
     0, 0, -1, -4),  # V 无效
    ("$GNRMC,093315.000,A,3108.6869,N,12147.0142,E,0.10,0.00,260826,,,A*79",
     31.144781667, 121.783570, 1787736795, 0),  # 上海 2026-08-26
    ("$BADMSG,1,2,3*00",
     0, 0, -1, -1),  # 非RMC
    ("$GNRMC,120000.000,A,2236.9453,N,11408.4790,E,0.53,292.44,141216,,,A*00",
     0, 0, -1, -2),  # 校验和错
    ("$GNRMC,0848,A,2236.9453,N,11408.4790,E,0.53,292.44,141216,,,A*00",
     0, 0, -1, -2),  # 校验和错(截断)
]


def py_parse(line):
    """独立Python实现(非C移植), 用于交叉验证"""
    if not (line.startswith("$GNRMC") or line.startswith("$GPRMC")):
        return -1, None
    body, _, cs = line[1:].partition("*")
    calc = 0
    for c in body:
        calc ^= ord(c)
    try:
        if calc != int(cs[:2], 16):
            return -2, None
    except ValueError:
        return -2, None
    f = body.split(",")
    if len(f) < 10:
        return -3, None
    if f[2] != "A":
        return -4, None
    if len(f[1]) < 6 or len(f[9]) < 6:
        return -5, None
    t = float(f[1])
    hh, mm, ss = int(t // 10000), int(t // 100) % 100, int(t) % 100
    lat = int(float(f[3]) / 100) + (float(f[3]) - int(float(f[3]) / 100) * 100) / 60
    lon = int(float(f[5]) / 100) + (float(f[5]) - int(float(f[5]) / 100) * 100) / 60
    if f[4] == "S":
        lat = -lat
    if f[6] == "W":
        lon = -lon
    yy = int(f[9]) % 100
    year = 1900 + yy if yy >= 70 else 2000 + yy
    ts = int(datetime.datetime(year, int(f[9]) // 100 % 100, int(f[9]) // 10000,
                               hh, mm, ss, tzinfo=datetime.timezone.utc).timestamp())
    return 0, {"lat": round(lat, 9), "lon": round(lon, 9), "ts": ts}


def build_c():
    harness = r"""
#include <stdio.h>
#include "gnss.h"
int main(int argc, char **argv) {
    GnssFix f = {0};
    int rc = gnss_parse_rmc(argv[1], &f);
    printf("{\"rc\":%d,\"lat\":%.9f,\"lon\":%.9f,\"ts\":%lld}\n",
           rc, f.lat, f.lon, (long long)f.ts);
    return 0;
}
"""
    with open("/tmp/opencode/gnss_test.c", "w") as fp:
        fp.write(harness)
    r = subprocess.run(["gcc", "-O2", "-I", os.path.dirname(C_HEAD),
                        "-o", "/tmp/opencode/gnss_test",
                        "/tmp/opencode/gnss_test.c", C_SRC],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def main():
    ok, err = build_c()
    if not ok:
        print("C编译失败:\n", err)
        return 1
    passed = 0
    for line, elat, elon, ets, erc in CASES:
        c = subprocess.run(["/tmp/opencode/gnss_test", line],
                           capture_output=True, text=True)
        cj = json.loads(c.stdout)
        prc, py = py_parse(line)
        ok = True
        if cj["rc"] != erc:
            ok = False
        if erc == 0:
            if abs(cj["lat"] - elat) > 1e-6 or abs(cj["lon"] - elon) > 1e-6:
                ok = False
            if cj["ts"] != ets and ets != 0:
                ok = False
            if prc != 0 or abs(cj["lat"] - py["lat"]) > 1e-9 \
               or abs(cj["lon"] - py["lon"]) > 1e-9 or cj["ts"] != py["ts"]:
                ok = False
        print(f"[{'PASS' if ok else 'FAIL'}] C={cj} py={py if erc == 0 else prc}")
        passed += ok
    print(f"\n结果: {passed}/{len(CASES)} 通过")
    return 0 if passed == len(CASES) else 1


if __name__ == "__main__":
    sys.exit(main())
