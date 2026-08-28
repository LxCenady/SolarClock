#!/usr/bin/env python3
"""solarclock: SolarTime TUI 入口

用法:
  solarclock               连接ESP32, 用本地缓存坐标(无则ESP32默认)
  solarclock -r            随机生成陆地经纬度+时区并注入
  solarclock -L 31.2 -O 121.5 -T 8 [--ts 1734782400]   指定坐标/时间戳(测试用)

按键: q 退出
状态机: DISCONNECTED -> INIT -> LIVE -> QUIT
"""
import argparse
import json
import os
import random
import select
import sys
import termios
import time
import tty

from timezonefinder import TimezoneFinder
from zoneinfo import ZoneInfo

from link import SolarLink
from render import clear_screen, render

CACHE = os.path.expanduser("~/.solarclock.json")
PORT = "/dev/ttyACM0"

# ---- 状态机 ----
ST_DISC, ST_INIT, ST_LIVE, ST_QUIT = 0, 1, 2, 3


def _rand_point():
    """随机陆地经纬度, 按该点反查时区偏移(含夏令时)"""
    tf = TimezoneFinder()
    while True:
        lat = random.uniform(-85, 85)
        lon = random.uniform(-180, 180)
        tzname = tf.timezone_at(lat=lat, lng=lon)
        if tzname:
            import datetime
            dt = datetime.datetime.now(datetime.timezone.utc)
            off = dt.astimezone(ZoneInfo(tzname)).utcoffset()
            return round(lat, 6), round(lon, 6), off.total_seconds() / 3600.0


def _load_cache():
    try:
        return json.load(open(CACHE))
    except Exception:
        return None


def _save_cache(lat, lon, tz):
    try:
        json.dump({"lat": lat, "lon": lon, "tz": tz}, open(CACHE, "w"))
    except Exception:
        pass


def _show_hint():
    print("SolarClock TUI  |  q: quit  (等待ESP32心跳...)")
    print("若长时间无心跳: 检查连接与 init ACK 输出")


def main():
    ap = argparse.ArgumentParser(description="SolarTime TUI")
    ap.add_argument("--port", default=PORT)
    ap.add_argument("-r", action="store_true", help="随机生成经纬度+时区")
    ap.add_argument("-L", "--lat", type=float)
    ap.add_argument("-O", "--lon", type=float)
    ap.add_argument("-T", "--tz", type=float)
    ap.add_argument("--ts", type=int, default=None, help="注入的UTC时间戳(默认当前)")
    args = ap.parse_args()

    # ---- 坐标来源: -r > -L/-O/-T > 本地缓存 > ESP32默认 ----
    if args.r:
        lat, lon, tz = _rand_point()
        print(f"[random] lat={lat} lon={lon} tz={tz}")
    elif args.lat is not None and args.lon is not None and args.tz is not None:
        lat, lon, tz = args.lat, args.lon, args.tz
    else:
        cache = _load_cache()
        if cache:
            lat, lon, tz = cache["lat"], cache["lon"], cache["tz"]
            print(f"[cache] lat={lat} lon={lon} tz={tz}")
        else:
            lat, lon, tz = None, None, None
            print("[default] 使用ESP32内置坐标")

    state = ST_DISC
    link = None
    missing = 0

    try:
        while state != ST_QUIT:
            if state == ST_DISC:
                try:
                    link = SolarLink(args.port)
                except Exception as e:
                    print(f"[error] 无法打开 {args.port}: {e}")
                    return 1
                state = ST_INIT

            elif state == ST_INIT:
                ts = args.ts if args.ts is not None else int(time.time())
                if lat is None:
                    cfg = link.get_cfg()
                    if cfg is None:
                        print("[error] 无法获取ESP32默认坐标, 请用 -r 或 -L/-O/-T")
                        state = ST_QUIT
                        break
                    lat, lon, tz = cfg["lat"], cfg["lon"], cfg["tz"]
                    print(f"[default] 使用ESP32内置: lat={lat} lon={lon} tz={tz}")
                ack = link.init(ts, lat, lon, tz)
                if ack is None:
                    print("[error] init 无响应")
                    state = ST_QUIT
                    break
                print(f"[init] OK rise={ack['rise']} set={ack['set']} "
                      f"solar={ack['solar']} decl={ack['decl']} eot={ack['eot']}")
                _save_cache(lat, lon, tz)
                sys.stdout.write(clear_screen() + "\x1b[?25l")
                sys.stdout.flush()
                state = ST_LIVE

            elif state == ST_LIVE:
                hb = link.heartbeat(timeout=1.0)
                if hb is None:
                    missing += 1
                    if missing > 5:
                        print("\x1b[0m[error] 心跳丢失, 退出")
                        state = ST_QUIT
                    continue
                missing = 0
                sys.stdout.write(render(hb))
                sys.stdout.flush()

            # 键盘
            if sys.stdin.isatty():
                r, _, _ = select.select([sys.stdin], [], [], 0)
                if r:
                    c = sys.stdin.read(1)
                    if c in ("q", "Q"):
                        state = ST_QUIT
    except KeyboardInterrupt:
        pass
    finally:
        if link:
            link.stop()
            link.close()
        sys.stdout.write("\x1b[0m\x1b[?25h\n")
    return 0


if __name__ == "__main__":
    # 原始模式按键(无需回车)
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        sys.exit(main())
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
