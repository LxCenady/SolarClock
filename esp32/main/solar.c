#include "solar.h"

#include <math.h>

#define D2R  0.017453292519943295
#define R2D  57.29577951308232
#define ZEN  90.833 /* 日出日落日心天顶角: 90 + 0.833(大气折射+太阳半径) */
#define JD0  2451545.0
#define EPK  2440587.5 /* Unix纪元对应的儒略日 */

/* 儒略日(UT) */
static double jd_of(time_t t) { return (double)t / 86400.0 + EPK; }

/* 太阳赤纬(度)与均时差(分钟), 基于NOAA简化算法 */
static void sun_angles(double jd, double *decl, double *eot) {
    double T  = (jd - JD0) / 36525.0;
    double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
    double M  = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    double M2 = M * D2R;
    double e  = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    double C  = sin(M2) * (1.914602 - T * (0.004817 + 0.000014 * T))
              + sin(2 * M2) * (0.019993 - 0.000101 * T)
              + sin(3 * M2) * 0.000289;
    double om  = (125.04 - 1934.136 * T) * D2R;
    double apL = L0 + C - 0.00569 - 0.00478 * sin(om);
    double eps = 23.43929111 - T * (0.013004167 + T * (1.639e-7 + T * 5.036e-7))
               + 0.00256 * cos(om);
    *decl = asin(sin(eps * D2R) * sin(apL * D2R)) * R2D;
    double y = tan(eps * D2R * 0.5);
    y *= y;
    *eot = (y * sin(2 * L0 * D2R) - 2 * e * sin(M2)
            + 4 * e * y * sin(M2) * cos(2 * L0 * D2R)
            - 0.5 * y * y * sin(4 * L0 * D2R)
            - 1.25 * e * e * sin(2 * M2)) * R2D * 4.0;
}

/* 太阳高度角达到zen时的小时角(度); ±999 表示极昼/极夜无日出日落 */
static double hour_angle(double lat, double decl, double zen) {
    double c = (cos(zen * D2R) - sin(lat * D2R) * sin(decl * D2R))
               / (cos(lat * D2R) * cos(decl * D2R));
    if (c > 1.0)  return 999.0;
    if (c < -1.0) return -999.0;
    return acos(c) * R2D;
}

void solar_compute(const SolarCfg *cfg, time_t utc, SolarResult *r) {
    double decl, eot;
    sun_angles(jd_of(utc), &decl, &eot);

    /* 太阳正午(UTC分钟) = 720 - 经度*4 - 均时差; 日出/日落对称于正午 */
    double noon = 720.0 - cfg->lon * 4.0 - eot;
    double ha   = hour_angle(cfg->lat, decl, ZEN);

    /* 极昼/极夜: 无日出日落, rise/set 置 -1 标记 */
    int polar = (ha == 999.0 || ha == -999.0);
    double rise = noon - ha * 4.0, set = noon + ha * 4.0;

    /* UTC分钟 + 时区 -> 本地墙钟分钟(取模1440) */
    double tz  = cfg->tz * 60.0;
    double cur = fmod((utc % 86400) / 60.0 + tz, 1440.0);
    if (cur < 0) cur += 1440.0;
    double wn  = fmod(noon + tz, 1440.0);
    double wr  = fmod(rise + tz, 1440.0);
    double ws  = fmod(set + tz, 1440.0);

    r->decl      = decl;
    r->eot       = eot;
    r->noon_min  = (int16_t)wn;
    r->rise_min  = polar ? -1 : (int16_t)wr;
    r->set_min   = polar ? -1 : (int16_t)ws;

    /* 当地太阳时: 墙钟 + 经度修正 + 均时差 - 时区偏移, 即本地时钟上太阳走到的刻度 */
    double sm = cur + cfg->lon * 4.0 + eot - tz;
    r->solar_min = sm;

    /* 距日落(分钟), 取最近一次日落, 可跨午夜; 极昼取 +1440 哨兵 */
    double ds = polar ? 1440.0 : ws - cur;
    if (!polar) {
        if (ds < -720.0) ds += 1440.0;
        if (ds > 720.0)  ds -= 1440.0;
    }
    r->to_set_min = ds;
}
