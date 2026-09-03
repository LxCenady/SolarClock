#!/usr/bin/env python3
"""solarclock: SolarTime TUI 入口

用法:
  solarclock               连接ESP32, 默认GNSS监听模式(等硬件定位)
  solarclock --use-cache   使用 ~/.solarclock.json 缓存坐标注入
  solarclock -c            交互配置: 坐标/时区/当地时间, 记忆供下次使用
  solarclock -r            随机生成陆地经纬度+时区并注入
  solarclock -L 31.2 -O 121.5 -T 8 [--ts 1734782400]   指定坐标/时间戳(测试用)

按键: q 退出
状态机: DISCONNECTED -> INIT -> LIVE -> QUIT
"""
import argparse
import datetime
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
from render import clear_screen, render, render_search

CACHE = os.path.expanduser("~/.solarclock.json")
PORT = "/dev/ttyACM0"

# ---- 状态机 ----
ST_DISC, ST_INIT, ST_LIVE, ST_QUIT = 0, 1, 2, 3


def _ip_geolocate():
    """PC本地网络定位(IP), 返回 (lat, lon, tz偏移小时)"""
    import urllib.request
    for url in ("https://ipinfo.io/json", "http://ip-api.com/json/?fields=lat,lon,timezone"):
        try:
            with urllib.request.urlopen(url, timeout=8) as f:
                d = json.load(f)
            if "loc" in d:
                lat, lon = map(float, d["loc"].split(","))
                tzname = d.get("timezone", "UTC")
            elif "lat" in d:
                lat, lon = float(d["lat"]), float(d["lon"])
                tzname = d.get("timezone", "UTC")
            else:
                continue
            dt = datetime.datetime.now(datetime.timezone.utc)
            off = dt.astimezone(ZoneInfo(tzname)).utcoffset()
            return round(lat, 6), round(lon, 6), off.total_seconds() / 3600.0
        except Exception:
            continue
    return None


def _rand_point():
    """随机陆地经纬度, 按该点反查时区偏移(含夏令时)"""
    tf = TimezoneFinder()
    while True:
        lat = random.uniform(-85, 85)
        lon = random.uniform(-180, 180)
        tzname = tf.timezone_at(lat=lat, lng=lon)
        if tzname:
            dt = datetime.datetime.now(datetime.timezone.utc)
            off = dt.astimezone(ZoneInfo(tzname)).utcoffset()
            return round(lat, 6), round(lon, 6), off.total_seconds() / 3600.0


def _config_mode():
    """-c: 交互询问坐标/时区/当地时间, 计算时间偏移并记忆"""
    print("=== SolarClock 配置模式 ===")
    try:
        lat = float(input("纬度 lat (北正南负, 如 31.2304): "))
        lon = float(input("经度 lon (东正西负, 如 121.4737): "))
        tz = float(input("时区 tz (UTC+8 => 8, 支持小数): "))
        s = input("当地时间 HH:MM (该地墙钟当前时间): ").strip()
        h, m = map(int, s.split(":"))
    except (ValueError, EOFError) as e:
        print(f"[error] 输入无效: {e}")
        return False

    # 期望的UTC时刻 = 当地墙钟 - 时区; delta = 期望 - PC当前UTC
    wall_min = h * 60 + m
    utc_now = datetime.datetime.now(datetime.timezone.utc)
    utc_now_min = utc_now.hour * 60 + utc_now.minute
    want_min = wall_min - tz * 60
    delta_h = (want_min - utc_now_min) / 60.0
    delta_h = (delta_h + 12) % 24 - 12  # 对齐到±12h内

    _save_cache(lat, lon, tz, delta_h)
    print(f"[ok] 已记忆: lat={lat} lon={lon} tz={tz} 时间偏移={delta_h:+.1f}h")
    print(f"[ok] 下次运行 solarclock 将显示 {s} 附近的当地时间")
    return True


def _load_cache():
    try:
        return json.load(open(CACHE))
    except Exception:
        return None


def _save_cache(lat, lon, tz, delta_h=0.0):
    try:
        json.dump({"lat": lat, "lon": lon, "tz": tz, "delta_h": delta_h},
                  open(CACHE, "w"))
    except Exception:
        pass


def _show_hint():
    print("SolarClock TUI  |  q: quit  (等待ESP32心跳...)")
    print("若长时间无心跳: 检查连接与 init ACK 输出")


def main():
    ap = argparse.ArgumentParser(description="SolarTime TUI")
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--use-cache", action="store_true",
                    help="使用 ~/.solarclock.json 缓存坐标/时间偏移注入")
    ap.add_argument("-r", action="store_true", help="随机生成经纬度+时区")
    ap.add_argument("--local", action="store_true",
                    help="PC本地网络定位(IP)获取GPS, 打包JSON提交ESP32")
    ap.add_argument("-c", action="store_true", help="交互配置坐标/时区/当地时间并记忆")
    ap.add_argument("-L", "--lat", type=float)
    ap.add_argument("-O", "--lon", type=float)
    ap.add_argument("-T", "--tz", type=float)
    ap.add_argument("--ts", type=int, default=None, help="注入的UTC时间戳(默认当前)")
    args = ap.parse_args()

    if args.c:
        return 0 if _config_mode() else 1

    delta_h = 0.0
    gnss_mode = False
    # ---- 坐标来源: --use-cache > -r > --local > -L/-O/-T > (默认GNSS监听模式) ----
    if args.use_cache:
        cache = _load_cache()
        if cache is None:
            print("[error] 没有缓存配置, 请先运行 solarclock -c 或使用 -r/-L 指定")
            return 1
        lat, lon, tz = cache["lat"], cache["lon"], cache["tz"]
        delta_h = cache.get("delta_h", 0.0)
        print(f"[cache] lat={lat} lon={lon} tz={tz} delta_h={delta_h:+.1f}h")
    elif args.r:
        lat, lon, tz = _rand_point()
        print(f"[random] lat={lat} lon={lon} tz={tz}")
    elif args.local:
        g = _ip_geolocate()
        if g is None:
            print("[error] 本地网络定位失败(需联网), 请用 -c/-r/-L 指定坐标")
            return 1
        lat, lon, tz = g
        print(f"[local] IP定位 lat={lat} lon={lon} tz={tz}")
    elif args.lat is not None and args.lon is not None and args.tz is not None:
        lat, lon, tz = args.lat, args.lon, args.tz
    else:
        # 默认: GNSS 监听模式, 不注入init, 等硬件定位后心跳
        gnss_mode = True
        lat = lon = tz = None
        print("[gnss] 监听ESP32的GNSS定位, 搜星中...")

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
                time.sleep(2.0)  # 打开串口会触发ESP32复位, 必须等启动完成
                try:
                    link.ser.reset_input_buffer()
                except Exception:
                    pass
                state = ST_LIVE if gnss_mode else ST_INIT
                if gnss_mode:
                    sys.stdout.write(clear_screen() + "\x1b[?25l")
                    sys.stdout.flush()

            elif state == ST_INIT:
                ts = args.ts if args.ts is not None \
                    else int(time.time()) + int(delta_h * 3600)
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
                _save_cache(lat, lon, tz, delta_h)
                sys.stdout.write(clear_screen() + "\x1b[?25l")
                sys.stdout.flush()
                state = ST_LIVE

            elif state == ST_LIVE:
                ev = link.read_event(timeout=1.0)
                if ev is None:
                    missing += 1
                    if missing > 5 and not gnss_mode:
                        print("\x1b[0m[error] 心跳丢失, 退出")
                        state = ST_QUIT
                    continue
                missing = 0
                if ev.get("gnss") == "search":
                    sys.stdout.write(render_search(ev))
                elif "dp" in ev:
                    sys.stdout.write(render(ev))
                else:
                    continue
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
    fd = sys.stdin.fileno()
    try:
        old = termios.tcgetattr(fd)
    except termios.error:
        sys.exit(main())  # 非tty(管道/重定向输入), 直接运行
    try:
        tty.setcbreak(fd)
        sys.exit(main())
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
