#!/usr/bin/env python3
"""边界测试: 时区分界线/极区边界/午夜日落/极点等易错场景, 与astral热比对

覆盖:
  - 东经+负时区 (阿留申, 曾触发 fmod 负值误判极区的回归bug)
  - 西经+正时区 (基里巴斯 +14, 日期变更线)
  - 极昼/极夜 (特罗姆瑟)
  - 极圈边界日 (66.56°N 夏至/冬至, c接近±1)
  - 午夜日落 (高纬夏季日落过零点)
  - 极点钳制 (89.9°N/S, 防除零NaN)
  - 赤道, 半时区, 午夜日出
"""
import argparse
import json
import sys
import time

import serial

from hot_test import reference, wait_json, verify, tol_min
import hot_test

# (名字, 纬度, 经度, 时区, UTC时间戳, 备注)
CASES = [
    ("aleutian_eastlon_negTZ", 51.873,  179.876, -11.0, 1787914548, "东经179.9+时区-11(回归bug)"),
    ("kiritimati_westlon_posTZ", 1.8721, -157.4278, 14.0, 1734782400, "西经+UTC14日期线"),
    ("date_line_west", -16.5, -179.9, -12.0, 1787922000, "西经179.9+时区-12"),
    ("kuril_eastlon_posTZ", 45.0,  150.0,  10.0, 1787922000, "东经150+时区+10"),
    ("polar_day",  69.6492,  18.9553, 1.0, 1718971200, "特罗姆瑟夏至极昼"),
    ("polar_night", 69.6492,  18.9553, 1.0, 1734782400, "特罗姆瑟冬至极夜"),
    ("arctic_circle_jun", 66.56,  25.0, 2.0, 1718971200, "极圈66.56N夏至(c~-1边界)"),
    ("arctic_circle_dec", 66.56,  25.0, 2.0, 1734739200, "极圈66.56N冬至(c~+1边界)"),
    ("midnight_sunset", 62.0, -20.0, 0.0, 1787922000, "高纬夏末日落近午夜"),
    ("north_pole_clamp", 89.9,   0.0, 0.0, 1787922000, "北极钳制防除零"),
    ("south_pole_clamp", -89.9,  0.0, 0.0, 1787922000, "南极钳制防除零"),
    ("equator", 0.1,  100.0, 7.0, 1787922000, "赤道日出日落≈06:00/18:00"),
    ("half_hour_tz", 19.076,  72.8777, 5.5, 1734782400, "孟买半时区"),
    ("midnight_rise", -55.0, -70.0, -3.0, 1787865600, "高纬南半球午夜前日出"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.5)
    time.sleep(2.0)
    ser.reset_input_buffer()
    ser.write(b'{"cmd":"stop"}\n')  # 清场: 退出残留心跳模式
    time.sleep(0.3)
    ser.reset_input_buffer()
    print(f"边界测试: {len(CASES)} 例\n")

    ok = 0
    for name, lat, lon, tz, ts, note in CASES:
        req = {"cmd": "solar", "ts": int(ts), "lat": lat, "lon": lon, "tz": tz}
        ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode())
        r = wait_json(ser)
        if not r:
            print(f"[{name}] FAIL 无响应 ({note})")
            continue
        exp = reference(lat, lon, tz, ts)
        fails, warns = verify(lat, lon, tz, ts, r, exp, "/tmp/solarstream.jsonl")
        for w in warns:
            print(f"[{name}] WARN {w}")
        if not fails:
            ok += 1
            print(f"[{name}] PASS polar={r['polar']} rise={r['rise']} noon={r['noon']} "
                  f"set={r['set']} solar={r['solar']} ({note})")
        else:
            print(f"[{name}] FAIL: {'; '.join(fails)} ({note})")
    ser.close()
    print(f"\n边界测试结果: {ok}/{len(CASES)} 通过")
    sys.exit(0 if ok == len(CASES) else 1)


if __name__ == "__main__":
    main()
