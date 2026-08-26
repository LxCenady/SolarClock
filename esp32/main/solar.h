#ifndef SOLAR_H
#define SOLAR_H

#include <stdint.h>
#include <time.h>

/* 一次GPS获取后保存的参数(对应ESP32上可存NVS) */
typedef struct {
    double lat;     /* 纬度, 北正南负, 度 */
    double lon;     /* 经度, 东正西负, 度 */
    int8_t  tz;     /* 时区, UTC+8 => 8 */
} SolarCfg;

/* 计算结果 */
typedef struct {
    int16_t rise_min;   /* 日出(本地墙钟分钟, 0-1439) */
    int16_t noon_min;   /* 太阳正午(同上) */
    int16_t set_min;    /* 日落(同上) */
    double  solar_min;  /* 当地太阳时(墙钟分钟, 0-1440, 可能>1439) */
    double  to_set_min; /* 距日落分钟数(已过则为负) */
    double  decl;       /* 太阳赤纬, 度 */
    double  eot;        /* 均时差, 分钟 */
} SolarResult;

/* 一次性计算: 传入任意UTC时间戳, 全程无网络, 纯本地计算 */
void solar_compute(const SolarCfg *cfg, time_t utc, SolarResult *r);

#endif
