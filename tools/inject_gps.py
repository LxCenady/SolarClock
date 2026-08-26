#!/usr/bin/env python3
"""注入GPS数据到ESP32串口并读取其输出验证算法"""
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200
GPS_LINE = "GPS 1.2897 103.8501 8\n"  # 新加坡(IP定位)

if len(sys.argv) > 1:
    GPS_LINE = "GPS " + " ".join(sys.argv[1:4]) + "\n"

ser = serial.Serial(PORT, BAUD, timeout=0.5)
# 打开串口时 DTR 上升沿会触发板子复位(自动复位电路), 这是预期行为
time.sleep(2.0)  # 等待启动完成
ser.reset_input_buffer()

# 等待启动完成再注入, 防止数据在启动期间丢失
booted = False
t0 = time.time()
while time.time() - t0 < 6:
    line = ser.readline().decode(errors="replace").rstrip()
    if line:
        print(f"[ESP32] {line}")
        if "SolarTime 启动" in line:
            booted = True
            break
if not booted:
    time.sleep(2)

ser.write(GPS_LINE.encode())
print(f"[PC] 发送: {GPS_LINE.strip()}")

deadline = time.time() + 8
got = []
while time.time() < deadline:
    line = ser.readline().decode(errors="replace").rstrip()
    if line:
        got.append(line)
        print(f"[ESP32] {line}")

keywords = ["GPS已注入", "日出", "太阳时"]
found = any(any(k in l for k in keywords) for l in got)
print("\n验证:", "通过 - 注入生效并输出算法结果" if found else "失败 - 未收到响应")
sys.exit(0 if found else 1)
