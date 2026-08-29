/* GNSS 数据链路: ATGM336H 占位驱动(纯解析逻辑, 无硬件依赖, 可PC单测)
 * 物理接入见 GNSS_RTC_PROTOCOL.md: UART1 115200 8N1, 只消费 $GNRMC */
#include "gnss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NMEA校验和: $ 与 * 之间字符异或, 与 * 后2位hex比较 */
static int nmea_checksum(const char *line) {
    const char *p = strchr(line, '*');
    if (!p || strlen(p + 1) < 2) return 0;
    unsigned char cs = 0;
    for (const char *q = line + 1; q < p; q++) cs ^= (unsigned char)*q;
    return cs == (unsigned char)strtoul(p + 1, NULL, 16);
}

/* 手工按逗号拆字段(保留空字段, strtok会跳过连续分隔符不可用) */
static int nmea_split(const char *line, char out[][16], int max) {
    int n = 0;
    const char *s = line;
    while (n < max) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len >= 16) len = 15;
        memcpy(out[n], s, len);
        out[n][len] = 0;
        n++;
        if (!e) break;
        s = e + 1;
    }
    return n;
}

/* ddmm.mmmm / dddmm.mmmm -> 十进制度 */
static double ddmm2dec(double v) {
    int d = (int)(v / 100.0);
    return d + (v - d * 100.0) / 60.0;
}

/* 公历->unix秒(UTC), 无库依赖 */
time_t gnss_mkts(int year, int mon, int day, int hh, int mm, int ss) {
    if (mon <= 2) { year--; mon += 12; }
    int era = (year >= 0 ? year : year - 399) / 400;
    int yoe = year - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)(era * 146097 + doe - 719468) * 86400LL
           + hh * 3600LL + mm * 60LL + ss;
}

int gnss_parse_rmc(const char *line, GnssFix *f) {
    if (strncmp(line, "$GNRMC", 6) && strncmp(line, "$GPRMC", 6)) return -1;
    if (!nmea_checksum(line)) return -2;

    char fld[20][16];
    int n = nmea_split(line, fld, 20);
    /* $GNRMC,time,status,lat,N,lon,E,spd,cog,date,... */
    if (n < 11) return -3;
    if (fld[2][0] != 'A') return -4;      /* status: A=有效 */
    if (strlen(fld[1]) < 6 || strlen(fld[9]) < 6) return -5;

    double t = atof(fld[1]);
    int hh = (int)(t / 10000.0), mm = (int)(t / 100.0) % 100, ss = (int)t % 100;

    double lat = ddmm2dec(atof(fld[3]));
    if (fld[4][0] == 'S') lat = -lat;
    double lon = ddmm2dec(atof(fld[5]));
    if (fld[6][0] == 'W') lon = -lon;

    int dd = atoi(fld[9]) / 10000, mo = atoi(fld[9]) / 100 % 100, yy = atoi(fld[9]) % 100;
    int year = (yy >= 70) ? 1900 + yy : 2000 + yy;  /* 两位年世纪推断 */

    f->lat = lat;
    f->lon = lon;
    f->ts = gnss_mkts(year, mo, dd, hh, mm, ss);
    f->valid = 1;
    return 0;
}
