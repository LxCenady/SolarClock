#!/usr/bin/env python3
"""向ESP32注入GPS数据(JSON协议), 读取JSON响应并展示"""
import json
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200

# 默认: 新加坡(IP定位)
REQ = {"cmd": "solar", "ts": 1734782400, "lat": 1.2897, "lon": 103.8501, "tz": 8}


def parse_args():
    global REQ
    if len(sys.argv) < 4:
        return
    REQ["lat"], REQ["lon"], REQ["tz"] = map(float, sys.argv[1:4])
    if len(sys.argv) > 4:
        REQ["ts"] = int(sys.argv[4])


def main():
    parse_args()
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    time.sleep(2.0)  # 打开串口触发复位, 等待启动
    ser.reset_input_buffer()

    req = json.dumps(REQ, separators=(",", ":"))
    ser.write((req + "\n").encode())
    print(f"[PC] {req}")

    t0 = time.time()
    while time.time() - t0 < 6:
        b = ser.readline()
        if not b:
            continue
        line = b.decode(errors="replace").strip()
        if line.startswith("{"):
            r = json.loads(line)
            print("[ESP32]", json.dumps(r, ensure_ascii=False))
            return 0 if "solar" in r else 1
    print("失败 - 未收到JSON响应")
    return 1


if __name__ == "__main__":
    sys.exit(main())
