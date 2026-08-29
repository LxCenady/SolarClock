#!/usr/bin/env python3
"""生成 SolarTime 验证数据集: 多地区 x 随机时刻

参考值由 NOAA 官方算法计算 (tools/noaa_ref.py), 与 astral 完全独立。

输出 tools/dataset.json, 每个样本含:
  ts      : UTC 时间戳
  exp.rise/noon/set : 本地墙钟 HH:MM (参考日出/正午/日落)
  exp.solar         : 当地真太阳时(表盘 HH:MM, 参考值)
  exp.polar         : null | "day"(极昼) | "night"(极夜)
"""
import json
import random
from datetime import datetime, timezone

from noaa_ref import solar

random.seed(42)

# 20 个地区: 覆盖热带/中纬/高纬/南北半球/正负时区/半时区/UTC+14 极端
# (中文名, ASCII名, 纬度, 经度, 时区)
LOCATIONS = [
    ("新加坡", "Singapore",     1.2897,  103.8501,   8.0),
    ("上海",  "Shanghai",     31.2304,  121.4737,   8.0),
    ("北京",  "Beijing",      39.9042,  116.4074,   8.0),
    ("东京",  "Tokyo",        35.6762,  139.6503,   9.0),
    ("悉尼",  "Sydney",      -33.8688,  151.2093,  10.0),
    ("奥克兰", "Auckland",    -36.8509,  174.7645,  12.0),
    ("惠灵顿", "Wellington",  -41.2865,  174.7762,  12.0),
    ("伦敦",  "London",       51.5074,   -0.1278,   0.0),
    ("巴黎",  "Paris",        48.8566,    2.3522,   1.0),
    ("开罗",  "Cairo",        30.0444,   31.2357,   2.0),
    ("孟买",  "Mumbai",       19.0760,   72.8777,   5.5),
    ("德黑兰", "Tehran",      35.6892,   51.3890,   3.5),
    ("墨西哥城", "MexicoCity", 19.4326,  -99.1332,  -6.0),
    ("纽约",  "NewYork",      40.7128,  -74.0060,  -5.0),
    ("洛杉矶", "LosAngeles",  34.0522, -118.2437,  -8.0),
    ("檀香山", "Honolulu",    21.3069, -157.8583, -10.0),
    ("圣约翰斯", "StJohns",   47.5615,  -52.7126,  -3.5),
    ("基里蒂马蒂", "Kiritimati", 1.8721, -157.4278, 14.0),
    ("特罗姆瑟", "Tromso",    69.6492,   18.9553,   1.0),
    ("黄刀镇", "Yellowknife", 62.4540, -114.3718,  -7.0),
]

RANGE = (datetime(2024, 1, 1, tzinfo=timezone.utc).timestamp(),
         datetime(2025, 12, 31, 23, 59, tzinfo=timezone.utc).timestamp())

# 极昼/极夜必测时刻
FORCED = {
    "特罗姆瑟": [datetime(2024, 6, 21, 12, 0, tzinfo=timezone.utc),
                 datetime(2024, 12, 21, 12, 0, tzinfo=timezone.utc)],
    "黄刀镇":  [datetime(2024, 12, 21, 12, 0, tzinfo=timezone.utc)],
}


def sample(name, name_en, lat, lon, tz, ts):
    d = datetime.fromtimestamp(ts, tz=timezone.utc)
    r = solar(ts, lat, lon, tz)  # NOAA 官方参考

    exp = {"polar": r["polar"]}
    exp["noon"] = r["noon"]
    exp["solar"] = r["solar"]
    exp["eot"] = r["eot"]
    if r["rise"] is not None:
        exp["rise"] = r["rise"]
    if r["set"] is not None:
        exp["set"] = r["set"]

    return {
        "name": name, "name_en": name_en, "lat": lat, "lon": lon, "tz": tz,
        "ts": ts, "date": d.isoformat().replace("+00:00", "Z"),
        "exp": exp,
    }


def main():
    out = []
    forced = {k: [t.timestamp() for t in v] for k, v in FORCED.items()}
    for name, name_en, lat, lon, tz in LOCATIONS:
        tss = forced.get(name, [])
        while len(tss) < 2:
            tss.append(random.uniform(*RANGE))
        for ts in tss:
            out.append(sample(name, name_en, lat, lon, tz, ts))

    path = __file__.rsplit("/", 1)[0] + "/dataset.json"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    n = len(out)
    polar = sum(1 for o in out if o["exp"]["polar"])
    frac = sum(1 for o in out if o["tz"] != int(o["tz"]))
    print(f"生成 {path}: {n} 样本, 极昼/极夜 {polar}, 半时区 {frac}")


if __name__ == "__main__":
    main()
