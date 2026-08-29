#!/usr/bin/env python3
"""GNSS 全链路模拟验证 (硬件未到货)

通过固件 nmea 注入命令, 模拟 ATGM336H 的 $GNRMC 数据流,
验证: 解析 -> 防抖 -> fix处理 -> 心跳模式 的完整链路。

场景:
  1. 无效定位(V) -> 不应触发fix
  2. 首次有效fix(上海) -> ACK + 心跳模式, 心跳含坐标
  3. 坐标漂移(北京) -> 新ACK, 心跳坐标更新
  4. 校验和错误 -> 忽略
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import serial


def cs(line):
    body = line[1:].split("*")[0]
    c = 0
    for ch in body:
        c ^= ord(ch)
    return line.split("*")[0] + f"*{c:02X}"


def rmc(ts, lat, lon, status="A"):
    """构造$GNRMC行(时间戳->UTC 时分秒+日期)"""
    import datetime
    dt = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)
    t = f"{dt.hour:02d}{dt.minute:02d}{dt.second:02d}.000"
    d = f"{dt.day:02d}{dt.month:02d}{dt.year % 100:02d}"
    lat_s = f"{int(abs(lat)):02d}{(abs(lat) - int(abs(lat))) * 60:07.4f}"
    lon_s = f"{int(abs(lon)):03d}{(abs(lon) - int(abs(lon))) * 60:07.4f}"
    line = f"$GNRMC,{t},{status},{lat_s},{'N' if lat >= 0 else 'S'}," \
           f"{lon_s},{'E' if lon >= 0 else 'W'},0.0,0.0,{d},,,A"
    return cs(line)


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


def send_nmea(ser, nmea_line):
    req = {"cmd": "nmea", "line": nmea_line}
    ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode())


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(2.0)
    ser.reset_input_buffer()
    ser.write(b'{"cmd":"stop"}\n')
    time.sleep(0.3)
    ser.reset_input_buffer()
    print("GNSS链路模拟开始\n")

    ok = 0
    total = 4

    # 1. V状态无效定位
    now = int(time.time())
    send_nmea(ser, rmc(now, 31.2304, 121.4737, status="V"))
    r = wait_json(ser, timeout=1.5)
    if r is None:
        ok += 1
        print("[PASS] V状态不触发fix")
    else:
        print(f"[FAIL] V状态却收到响应: {r}")

    # 2. 有效fix(上海) -> ACK + 心跳
    send_nmea(ser, rmc(now, 31.2304, 121.4737))
    ack = wait_json(ser)
    hb = None
    t0 = time.time()
    while time.time() - t0 < 3:
        h = wait_json(ser, timeout=1.0)
        if h and "dp" in h:
            hb = h
            break
    if ack and "rise" in ack and hb:
        ok += 1
        print(f"[PASS] fix ACK rise={ack['rise']} set={ack['set']}")
        print(f"       心跳 lat={hb['la']} lon={hb['lo']} (期望31.2304,121.4737)")
    else:
        print(f"[FAIL] fix链路: ack={ack} hb={hb}")

    # 3. 坐标漂移(北京)
    send_nmea(ser, rmc(now + 1, 39.9042, 116.4074))
    ack2 = wait_json(ser)
    hb2 = None
    t0 = time.time()
    while time.time() - t0 < 2:
        h = wait_json(ser, timeout=1.0)
        if h and "dp" in h:
            hb2 = h
            break
    if ack2 and hb2 and abs(hb2["la"] - 39.9042) < 0.01 and abs(hb2["lo"] - 116.4074) < 0.01:
        ok += 1
        print(f"[PASS] 漂移到北京: 心跳 lat={hb2['la']} lon={hb2['lo']}")
    else:
        print(f"[FAIL] 漂移: ack={ack2} hb={hb2}")

    # 4. 校验和错误 -> 忽略(无新的ACK式响应, 心跳照常)
    bad = "$GNRMC,120000.000,A,2236.9453,N,11408.4790,E,0.53,292.44,141216,,,A*00"
    send_nmea(ser, bad)
    r = wait_json(ser, timeout=1.5)
    if r is None or "rise" not in r:
        ok += 1
        print("[PASS] 校验和错误被忽略(无ACK式响应)")
    else:
        print(f"[FAIL] 坏校验和却触发了fix: {r}")

    ser.write(b'{"cmd":"stop"}\n')
    ser.close()
    print(f"\n结果: {ok}/{total} 通过")
    sys.exit(0 if ok == total else 1)


if __name__ == "__main__":
    main()
