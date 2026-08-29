#include "solar.h"

#include <math.h>

#define D2R  0.017453292519943295
#define R2D  57.29577951308232
/* 日出日落事件天顶角: NOAA标准 = 90 + 0.267(太阳半径) + 0.567(地平大气折射)
 * 官方精度声明: ±72°纬度内±1分钟, 之外±10分钟(浅日轨放大大气扰动) */
#define ZEN  90.833
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

/* 太阳高度角达到zen时的小时角(度); ±999 表示极昼/极夜无日出日落
 * atan2(√(1-c²),c) 与 acos(c) 数学等价, 但在 c≈±1(极区边界/浅日轨)
 * 处条件数远优于acos(其导数在此发散); 其余鲁棒性同前 */
static double hour_angle(double lat, double decl, double zen) {
    lat = fmax(-89.8, fmin(89.8, lat));
    double c = (cos(zen * D2R) - sin(lat * D2R) * sin(decl * D2R))
               / (cos(lat * D2R) * cos(decl * D2R));
    if (c > 1.0 + 1e-9)  return 999.0;
    if (c < -1.0 - 1e-9) return -999.0;
    if (c > 1.0) c = 1.0;
    else if (c < -1.0) c = -1.0;
    return atan2(sqrt(1.0 - c * c), c) * R2D;
}

/* 一次计算: 返回事件时刻(UTC分钟)与极区标记(polar: 0正常, 1极昼, -1极夜) */
static void rise_set(const SolarCfg *cfg, double jd,
                     double *decl, double *eot, double *rise, double *set,
                     int *polar) {
    sun_angles(jd, decl, eot);
    /* 太阳正午(UTC分钟) = 720 - 经度*4 - 均时差; 日出/日落对称于正午 */
    double noon = 720.0 - cfg->lon * 4.0 - *eot;
    double ha   = hour_angle(cfg->lat, *decl, ZEN);
    if (ha == 999.0)  { *polar = -1; *rise = noon; *set = noon; return; }
    if (ha == -999.0) { *polar =  1; *rise = noon; *set = noon; return; }
    *polar = 0;
    *rise = noon - ha * 4.0;
    *set  = noon + ha * 4.0;
}

void solar_compute(const SolarCfg *cfg, time_t utc, SolarResult *r) {
    double jd = jd_of(utc);
    double utc_min = fmod((double)(utc % 86400), 86400.0) / 60.0;
    if (utc_min < 0) utc_min += 1440.0;
    double decl, eot, rise, set;
    int polar;
    rise_set(cfg, jd, &decl, &eot, &rise, &set, &polar);

    if (!polar) {
        /* 精化(与参考库同款2次迭代): 在日出/日落/正午事件时刻重算赤纬与均时差
         * (赤纬日内变化 ~0.3°, 高纬/跨日事件会放大成分钟级误差) */
        double d2, e2, r2, s2;
        int p2;
        for (int k = 0; k < 2; k++) {
            rise_set(cfg, jd + (rise - utc_min) / 1440.0, &d2, &e2, &r2, &s2, &p2);
            if (!p2) rise = r2;
            rise_set(cfg, jd + (set - utc_min) / 1440.0, &d2, &e2, &r2, &s2, &p2);
            if (!p2) set = s2;
        }
        rise_set(cfg, jd + ((720.0 - cfg->lon * 4.0 - eot) - utc_min) / 1440.0,
                 &d2, &e2, &r2, &s2, &p2);
        eot = e2; /* 正午精化后的均时差 */
    }

    /* UTC分钟 + 时区 -> 本地墙钟分钟(取模1440) */
    double tz  = cfg->tz * 60.0;
    double cur = fmod((utc % 86400) / 60.0 + tz, 1440.0);
    if (cur < 0) cur += 1440.0;
    double noon = 720.0 - cfg->lon * 4.0 - eot;
    double wn  = fmod(noon + tz, 1440.0);
    double wr  = fmod(rise + tz, 1440.0);
    double ws  = fmod(set + tz, 1440.0);
    /* fmod 保留负号, 东经+负时区可能为负, 统一归一到 [0,1440) */
    if (wn < 0) wn += 1440.0;
    if (wr < 0) wr += 1440.0;
    if (ws < 0) ws += 1440.0;

    r->decl      = decl;
    r->eot       = eot;
    r->noon_min  = wn;
    r->rise_min  = polar ? -1.0 : wr;
    r->set_min   = polar ? -1.0 : ws;

    /* 当地太阳时: 墙钟 + 经度修正 + 均时差 - 时区偏移, 即本地时钟上太阳走到的刻度 */
    double sm = cur + cfg->lon * 4.0 + eot - tz;
    r->solar_min = sm;

    /* 距日落(分钟): 下一次日落距今时间; 已过今日日落则指向明日
     * 极昼无日落取 +1440 哨兵, 极夜无日出取 -1440 哨兵 */
    double ds;
    if (polar == 1) {
        ds = 1440.0;
    } else if (polar == -1) {
        ds = -1440.0;
    } else {
        ds = ws - cur;
        if (ds <= 0) ds += 1440.0;
    }
    r->to_set_min = ds;
}
