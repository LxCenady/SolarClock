#ifndef SOLAR_H
#define SOLAR_H

#include <stdint.h>
#include <time.h>

/* 一次GPS获取后保存的参数(对应ESP32上可存NVS) */
typedef struct {
    double lat;     /* 纬度, 北正南负, 度 */
    double lon;     /* 经度, 东正西负, 度 */
    double tz;      /* 时区, UTC+8 => 8, 支持小数(如孟买5.5) */
} SolarCfg;

/* 计算结果 */
typedef struct {
    double rise_min;    /* 日出(本地墙钟分钟, 含小数亚分钟精度, 0-1440) */
    double noon_min;    /* 太阳正午(同上) */
    double set_min;     /* 日落(同上) */
    double solar_min;   /* 当地太阳时(墙钟分钟, 0-1440, 可能>1439) */
    double to_set_min;  /* 距日落分钟数(已过则为负) */
    double decl;        /* 太阳赤纬, 度 */
    double eot;         /* 均时差, 分钟 */
} SolarResult;

/* 极区标记: rise_min/set_min < 0 表示极昼或极夜(由 to_set_min 哨兵区分:
 * +1440 极昼无日落, -1440 极夜无日出) */

/* 一次性计算: 传入任意UTC时间戳, 全程无网络, 纯本地计算 */
void solar_compute(const SolarCfg *cfg, time_t utc, SolarResult *r);

#endif
