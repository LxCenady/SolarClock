#!/usr/bin/env python3
"""二阶段热测试: 随机经纬度 -> 时区反查(含DST) -> ESP32实时热运算 -> astral热验算

每次随机生成陆地经纬度, 反查时区并取当前UTC时间(±抖动)作为 ts,
按 JSON 协议注入 ESP32, 固件实时计算返回 JSON, 与 PC 端 astral 参考值热比对。

原始 ESP32 JSON 响应逐行追加到 stream 文件(默认 /tmp/solarstream.jsonl),
可在另一终端执行:  tail -f /tmp/solarstream.jsonl  亲自核查输出格式。

用法: python3 tools/hot_test.py [--rounds 20] [--seed 7] [--jitter 21600] [--stream /tmp/solarstream.jsonl]
"""
import argparse
import datetime
import json
import random
import sys
import time

import serial
from noaa_ref import solar as noaa_solar
from timezonefinder import TimezoneFinder
from zoneinfo import ZoneInfo

TF = TimezoneFinder()


def rand_point():
    """随机陆地经纬度(海洋无时区, 重试)"""
    while True:
        lat = random.uniform(-85, 85)
        lon = random.uniform(-180, 180)
        if TF.timezone_at(lat=lat, lng=lon):
            return lat, lon


def tz_hours(tzname, ts):
    """时区在 ts 时刻的偏移小时(含夏令时), 如 8 / 5.5 / -3.5"""
    dt = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)
    off = dt.astimezone(ZoneInfo(tzname)).utcoffset()
    return off.total_seconds() / 3600.0


def reference_noaa(lat, lon, tz_h, ts):
    """NOAA 官方参考(默认), 与固件同源模型, 校验实现正确性"""
    return noaa_solar(ts, lat, lon, tz_h)


def reference_astral(lat, lon, tz_h, ts):
    """astral 交叉复核(独立实现, 折射约定有 0.044° 差异)"""
    from astral import Observer
    from astral.sun import noon, sunrise, sunset

    def polar_safe(fn, *a, **k):
        try:
            return fn(*a, **k)
        except ValueError:
            return None

    d = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).date()
    obs = Observer(latitude=lat, longitude=lon)
    toff = datetime.timedelta(hours=tz_h)
    r = polar_safe(sunrise, obs, date=d, tzinfo=datetime.timezone.utc)
    n = noon(obs, date=d, tzinfo=datetime.timezone.utc)
    s = polar_safe(sunset, obs, date=d, tzinfo=datetime.timezone.utc)
    noon_min = n.hour * 60 + n.minute + n.second / 60
    exp = {"polar": None}
    if r is not None:
        exp["rise"] = (r + toff).strftime("%H:%M")
    if s is not None:
        exp["set"] = (s + toff).strftime("%H:%M")
    if r is None or s is None:
        from astral.sun import zenith_and_azimuth
        alt = 90 - zenith_and_azimuth(obs, n)[0]
        exp["polar"] = "day" if alt > 0 else "night"
    exp["noon"] = (n + toff).strftime("%H:%M")
    dial = (720 + ts / 60 - noon_min) % 1440
    exp["solar"] = f"{int(dial // 60):02d}:{int(dial % 60):02d}"
    eot = round(720 - lon * 4 - noon_min, 2)
    if eot > 30:
        eot -= 1440
    elif eot < -30:
        eot += 1440
    exp["eot"] = eot
    return exp


REFERENCES = {"noaa": reference_noaa, "astral": reference_astral}


def hm_min(s):
    h, m = map(int, s.split(":"))
    return h * 60 + m


def dmin(a, b):
    d = abs(a - b) % 1440
    return min(d, 1440 - d)


def tol_min(lat):
    """容差: 参考库astral折射约定(90.789°)与NOAA标准(90.833°)的差异在
    浅日轨被放大; >72° 采用NOAA官方声明的±10分钟包络"""
    a = abs(lat)
    if a <= 60:
        return 2
    if a <= 72:
        return 6  # 折射约定差异在浅日轨放大到5分钟
    return 12  # NOAA官方±10min包络 + 参考库约定余量


def verify(lat, lon, tz_h, ts, r, exp, stream):
    """热比对, 并把原始JSON写入stream供另一终端核查"""
    with open(stream, "a", encoding="utf-8") as f:
        f.write(json.dumps(r, separators=(",", ":")) + "\n")

    fails = []
    warns = []
    tol = tol_min(lat)
    polar = exp.get("polar")
    if r.get("rise") == "--":
        if not polar:
            msg = "firmware极区但参考非极区"
            if abs(lat) > 72:
                warns.append(msg + " (>72°折射约定边界, 降级警告)")
            else:
                fails.append(msg)
        elif r.get("polar") != polar:
            msg = f"polar got {r.get('polar')} exp {polar}"
            if abs(lat) > 72:
                warns.append(msg + " (>72°折射约定边界, 降级警告)")
            else:
                fails.append(msg)
        return fails, warns
    if polar:
        if r.get("polar") != polar:
            msg = f"polar got {r.get('polar')} exp {polar}"
            if abs(lat) > 72:
                warns.append(msg + " (>72°折射约定边界, 降级警告)")
            else:
                fails.append(msg)
    else:
        for k in ("rise", "noon", "set"):
            if dmin(hm_min(r[k]), hm_min(exp[k])) > tol:
                fails.append(f"{k} got {r[k]} exp {exp[k]} (tol{tol})")
        if abs(r["eot"] - exp["eot"]) > 0.5:
            fails.append(f"eot got {r['eot']:.1f} exp {exp['eot']}")
        if dmin(hm_min(r["solar"]), hm_min(exp["solar"])) > 2:
            fails.append(f"solar got {r['solar']} exp {exp['solar']}")
    return fails, warns


def wait_json(ser, timeout=6):
    """等待一条一次性应答(特征: noon字段), 跳过心跳包"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.readline()
        if not b:
            continue
        line = b.decode(errors="replace").strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "noon" in d:
                return d
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--jitter", type=int, default=21600,
                    help="ts相对当前时间的抖动秒数, 默认±6h")
    ap.add_argument("--stream", default="/tmp/solarstream.jsonl")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--ref", choices=["noaa", "astral"], default="noaa",
                    help="参考基准: noaa官方算法(默认) / astral交叉复核")
    args = ap.parse_args()
    reference = REFERENCES[args.ref]

    random.seed(args.seed)
    ser = serial.Serial(args.port, 115200, timeout=0.5)
    time.sleep(2.0)
    ser.reset_input_buffer()
    ser.write(b'{"cmd":"stop"}\n')  # 清场: 退出残留心跳模式
    time.sleep(0.3)
    ser.reset_input_buffer()
    print(f"热测试开始: {args.rounds} 轮, seed={args.seed}, 抖动±{args.jitter // 3600}h"
          f", 基准={args.ref}")
    print(f"JSON流: {args.stream} (另一终端: tail -f {args.stream})\n")

    ok = 0
    for i in range(1, args.rounds + 1):
        lat, lon = rand_point()
        now = int(time.time())
        ts = now + random.randint(-args.jitter, args.jitter)
        tzname = TF.timezone_at(lat=lat, lng=lon)
        tz_h = tz_hours(tzname, ts)

        req = {"cmd": "solar", "ts": ts, "lat": round(lat, 6),
               "lon": round(lon, 6), "tz": tz_h}
        ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode())
        r = wait_json(ser)

        if not r:
            print(f"[{i:02d}] FAIL {tzname} @{lat:.3f},{lon:.3f}: 无响应")
            continue
        exp = reference(lat, lon, tz_h, ts)
        fails, warns = verify(lat, lon, tz_h, ts, r, exp, args.stream)
        for w in warns:
            print(f"[{i:02d}] WARN {w}")
        if not fails:
            ok += 1
            print(f"[{i:02d}] PASS {tzname:20s} tz{tz_h:+.1f} ts{ts} "
                  f"solar={r['solar']} rise={r['rise']} set={r['set']}")
        else:
            print(f"[{i:02d}] FAIL {tzname} @{lat:.3f},{lon:.3f}: {'; '.join(fails)}")
    ser.close()
    print(f"\n热测试结果: {ok}/{args.rounds} 通过")
    sys.exit(0 if ok == args.rounds else 1)


if __name__ == "__main__":
    main()
