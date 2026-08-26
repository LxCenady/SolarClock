#!/usr/bin/env python3
"""全量验收: 数据集样本按JSON协议注入ESP32, 与astral参考值比对

用法: python3 tools/verify_dataset.py [--port /dev/ttyACM0]
每个样本发送 {"cmd":"solar","ts":..,"lat":..,"lon":..,"tz":..},
解析固件JSON响应, 与参考值比对 (容差 ±2 分钟)。
"""
import argparse
import json
import sys
import time

import serial


def hm_min(s):
    h, m = map(int, s.split(":"))
    return h * 60 + m


def fmt(dmin):
    dmin = round(dmin)
    return f"{dmin // 60:02d}:{dmin % 60:02d}"


def dmin(a, b):
    d = abs(a - b) % 1440
    return min(d, 1440 - d)


def wait_json(ser, timeout=6):
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.readline()
        if not b:
            continue
        line = b.decode(errors="replace").strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return None


def run_case(ser, case):
    exp = case["exp"]
    req = {"cmd": "solar", "ts": int(case["ts"]), "lat": case["lat"],
           "lon": case["lon"], "tz": case["tz"]}
    ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode())
    r = wait_json(ser)
    if not r:
        return False, "无响应"

    fails = []
    polar = exp.get("polar")
    if polar:
        if r.get("polar") != polar:
            fails.append(f"极区: got {r.get('polar')} exp {polar}")
    else:
        for k in ("rise", "noon", "set"):
            d = dmin(hm_min(r[k]), hm_min(exp[k]))
            if d > 2:
                fails.append(f"{k}: got {r[k]} exp {exp[k]} ({d}min)")
        d = abs(r["eot"] - exp["eot"])
        if d > 0.5:
            fails.append(f"均时差: got {r['eot']:.1f} exp {exp['eot']} ({d:.1f}min)")
    d = dmin(hm_min(r["solar"]), hm_min(exp["solar"]))
    if d > 2:
        fails.append(f"太阳时: got {r['solar']} exp {exp['solar']} ({d}min)")

    return (len(fails) == 0), "; ".join(fails) if fails else "OK"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--dataset", default=None)
    args = ap.parse_args()
    path = args.dataset or (__file__.rsplit("/", 1)[0] + "/dataset.json")
    cases = json.load(open(path, encoding="utf-8"))
    print(f"共 {len(cases)} 个样本, 开始注入验证...\n")

    ser = serial.Serial(args.port, 115200, timeout=0.5)
    time.sleep(2.0)  # 等待启动
    ser.reset_input_buffer()

    ok = 0
    for i, case in enumerate(cases, 1):
        pass_, msg = run_case(ser, case)
        if pass_:
            ok += 1
        else:
            print(f"[{i:02d}] FAIL {case['name']} {case['date']}: {msg}")
    ser.close()

    print(f"\n结果: {ok}/{len(cases)} 通过")
    sys.exit(0 if ok == len(cases) else 1)


if __name__ == "__main__":
    main()
