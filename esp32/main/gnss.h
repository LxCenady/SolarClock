#ifndef GNSS_H
#define GNSS_H

#include <stdint.h>
#include <time.h>

/* GNSS 定位结果(与串口JSON init 契约同构) */
typedef struct {
    double lat;     /* 十进制度, 北正 */
    double lon;     /* 十进制度, 东正 */
    time_t ts;      /* UTC unix秒(由RMC的hhmmss+ddmmyy合成) */
    int valid;      /* 1=定位有效 */
} GnssFix;

/* 解析一行NMEA($GNRMC/$GPRMC)
 * 返回 0=有效fix, 负数=无效(原因): -1非RMC -2校验和错 -3字段不足 -4状态非A -5时间/日期缺 */
int gnss_parse_rmc(const char *line, GnssFix *f);

/* 十进制时间->unix秒(UTC, 与timezone/库无关的确定性实现) */
time_t gnss_mkts(int year, int mon, int day, int hh, int mm, int ss);

#endif
