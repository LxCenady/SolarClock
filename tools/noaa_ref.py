"""NOAA官方日出日落计算 - 参考实现

公式来源: NOAA Global Monitoring Laboratory 日出日落计算器
(https://gml.noaa.gov/grad/solcalc/calcdetails.html), 基于 Meeus 方程。
本实现严格按 NOAA 发布算法, 不含事件时刻精化;
solar.c 的同款算法在此基础上多了事件时刻赤纬/均时差精化(精度更高)。

验证基准: 与 astral 完全独立, 对齐 NOAA 官方模型。
"""
import math
from datetime import datetime, timedelta, timezone

D2R = math.pi / 180.0
R2D = 180.0 / math.pi
ZEN = 90.833  # NOAA官方: 90 + 0.267(太阳半径) + 0.567(地平折射)


def _jd(ts):
    return ts / 86400.0 + 2440587.5


def sun_angles(jd):
    """太阳赤纬(度)与均时差(分钟), NOAA 官方方程"""
    T = (jd - 2451545.0) / 36525.0
    L0 = (280.46646 + T * (36000.76983 + T * 0.0003032)) % 360.0
    M = 357.52911 + T * (35999.05029 - 0.0001537 * T)
    M2 = M * D2R
    e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T)
    C = (math.sin(M2) * (1.914602 - T * (0.004817 + 0.000014 * T))
         + math.sin(2 * M2) * (0.019993 - 0.000101 * T)
         + math.sin(3 * M2) * 0.000289)
    om = (125.04 - 1934.136 * T) * D2R
    apL = L0 + C - 0.00569 - 0.00478 * math.sin(om)
    eps = (23.43929111 - T * (0.013004167 + T * (1.639e-7 + T * 5.036e-7))
           + 0.00256 * math.cos(om))
    decl = math.asin(math.sin(eps * D2R) * math.sin(apL * D2R)) * R2D
    y = math.tan(eps * D2R * 0.5)
    y *= y
    eot = (y * math.sin(2 * L0 * D2R) - 2 * e * math.sin(M2)
           + 4 * e * y * math.sin(M2) * math.cos(2 * L0 * D2R)
           - 0.5 * y * y * math.sin(4 * L0 * D2R)
           - 1.25 * e * e * math.sin(2 * M2)) * R2D * 4.0
    return decl, eot


def _local(minute, tz):
    """UTC分钟 -> 本地墙钟HH:MM"""
    m = (int(minute + 0.5) + int(tz * 60)) % 1440
    return f"{m // 60:02d}:{m % 60:02d}"


def solar(ts, lat, lon, tz):
    """NOAA官方算法: 返回与 astral 参考同构的结果dict

    rise/set/noon: 本地HH:MM; solar: 真太阳时表盘; eot: 分钟; polar: None|"day"|"night"
    """
    decl, eot = sun_angles(_jd(ts))
    noon_utc = 720.0 - lon * 4.0 - eot

    res = {"rise": None, "noon": _local(noon_utc, tz), "set": None,
           "polar": None, "eot": round(eot, 2), "solar": None}

    c = (math.cos(ZEN * D2R) - math.sin(lat * D2R) * math.sin(decl * D2R)) \
        / (math.cos(lat * D2R) * math.cos(decl * D2R))
    if c > 1.0:
        res["polar"] = "night"
    elif c < -1.0:
        res["polar"] = "day"
    else:
        if c > 1.0:
            c = 1.0
        elif c < -1.0:
            c = -1.0
        ha = math.acos(c) * R2D
        res["rise"] = _local(noon_utc - ha * 4.0, tz)
        res["set"] = _local(noon_utc + ha * 4.0, tz)

    dial = (720.0 + ts / 60.0 - noon_utc) % 1440.0
    res["solar"] = f"{int(dial // 60):02d}:{int(dial % 60):02d}"
    return res
