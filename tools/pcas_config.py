#!/usr/bin/env python3
"""$PCAS 配置工具 (ATGM336H 中科微私有协议)

功能:
  1. 校验和生成:  -c "$PCAS01,5"  -> 输出带校验和的完整命令
  2. 命令发送:    通过ESP32固件 tx1 命令转发到 GNSS UART1
  3. 一键初始化:   python3 tools/pcas_config.py setup  (115200/1Hz/GGA+RMC/保存)

用法:
  python3 tools/pcas_config.py cs "$PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0,0"
  python3 tools/pcas_config.py send "$PCAS01,5*1D"
  python3 tools/pcas_config.py setup [--port /dev/ttyACM0]
"""
import argparse
import json
import sys
import time


def nmea_cs(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return f"*{c:02X}"


def full(body):
    return f"${body}{nmea_cs(body)}"


# 常用命令(未带校验和, 工具自动生成)
COMMANDS = {
    "save":   "PCAS00",              # 保存配置到FLASH
    "baud96": "PCAS01,1",            # 9600 (5N默认)
    "baud115": "PCAS01,5",           # 115200 (6N默认/5N改)
    "rate1hz": "PCAS02,1000",        # 1Hz
    "rate2hz": "PCAS02,500",         # 2Hz
    "rate5hz": "PCAS02,200",         # 5Hz
    "rate10hz": "PCAS02,100",        # 10Hz
    "only_rmc": "PCAS03,0,0,0,0,1,0,0,0,0,0,0,0,0,0",  # 只留RMC(会关闭GGA, 固件搜星sats将恒为0)
    "gga_rmc":  "PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0,0",  # GGA+RMC(与固件gnss_task默认一致)
    "gps_bds": "PCAS04,3",           # GPS+BDS双模
    "cold":   "PCAS10,2",            # 冷启动
    "hot":    "PCAS10,0",            # 热启动
    "reset":  "PCAS10,3",            # 出厂复位
}

SETUP_SEQ = ["baud115", "rate1hz", "gga_rmc", "gps_bds", "save", "cold"]


def send_tx1(ser, cmd):
    req = {"cmd": "tx1", "data": cmd}
    ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    sub = ap.add_subparsers(dest="action")

    c = sub.add_parser("cs", help="生成校验和: cs 'PCAS01,5'")
    c.add_argument("body")

    s = sub.add_parser("send", help="发送命令(可带校验和): send '$PCAS01,5*1D'")
    s.add_argument("cmd")

    st = sub.add_parser("setup", help="一键初始化: 115200/1Hz/GGA+RMC/GPS+BDS/保存/冷启动")
    args = ap.parse_args()

    import serial

    if args.action == "cs":
        body = args.body
        body = body[1:] if body.startswith("$") else body
        print(full(body))
        return 0

    ser = serial.Serial(args.port, 115200, timeout=0.3)
    time.sleep(2.0)
    ser.reset_input_buffer()

    if args.action == "send":
        cmd = args.cmd
        if "*" not in cmd:
            cmd = full(cmd[1:] if cmd.startswith("$") else cmd)
        send_tx1(ser, cmd)
        time.sleep(0.3)
        print(f"已转发: {cmd}")
        ser.close()
        return 0

    if args.action == "setup":
        print("初始化序列(每条间隔200ms):")
        for name in SETUP_SEQ:
            body = COMMANDS[name]
            cmd = full(body)
            send_tx1(ser, cmd)
            print(f"  {name:8s} {cmd}")
            time.sleep(0.2)
        ser.close()
        print("\n完成. 若GNSS无输出, 检查波特率(6N默认115200, 5N需先PC直连改波特率)")
        return 0

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
