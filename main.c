#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "solar.h"

/* 无配置文件时的默认参数(ESP32上可预编译烧写) */
#ifndef DEF_LAT
#define DEF_LAT 31.2304 /* 上海 */
#endif
#ifndef DEF_LON
#define DEF_LON 121.4737
#endif
#ifndef DEF_TZ
#define DEF_TZ 8
#endif

#define CFG_FILE "solar.cfg"

static int load_cfg(SolarCfg *c) {
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return -1;
    int n = fscanf(f, "%lf %lf %lf", &c->lat, &c->lon, &c->tz);
    fclose(f);
    return n == 3 ? 0 : -1;
}

static int save_cfg(const SolarCfg *c) {
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return -1;
    fprintf(f, "%.6f %.6f %.2f\n", c->lat, c->lon, c->tz);
    fclose(f);
    return 0;
}

/* 分钟 -> "HH:MM" */
static const char *hm(int min) {
    static char b[3][8];
    static int i;
    i = (i + 1) % 3;
    if (min < 0) min += 1440;
    min %= 1440;
    sprintf(b[i], "%02d:%02d", min / 60, min % 60);
    return b[i];
}

static void show(const SolarCfg *c, time_t t) {
    SolarResult r;
    solar_compute(c, t, &r);

    time_t lt = t + c->tz * 3600;
    struct tm *tm = gmtime(&lt);
    char now[32];
    strftime(now, sizeof now, "%F %H:%M:%S", tm);

    int sm = ((int)r.solar_min % 1440 + 1440) % 1440;
    int polar = r.set_min < 0;
    char sr[8], sn[8], ss[8], st[32];
    strcpy(sr, polar ? "---" : hm(r.rise_min));
    strcpy(sn, hm(r.noon_min));
    strcpy(ss, polar ? "---" : hm(r.set_min));
    strcpy(st, polar ? (r.to_set_min == 1440.0 ? "极昼, 今日无日落"
                                               : "极夜, 今日无日出")
                     : hm((int)r.to_set_min));

    printf("SolarTime - 本地: %s (UTC%+g)\n"
           "GPS: %.4f°N, %.4f°E\n"
           "太阳时: %02d:%02d  |  赤纬: %.2f°  均时差: %+.1fmin\n"
           "日出: %s   正午: %s   日落: %s\n"
           "距日落: %s\n",
           now, (double)c->tz, c->lat, c->lon,
           sm / 60, sm % 60, r.decl, r.eot, sr, sn, ss, st);
}

int main(int argc, char **argv) {
    SolarCfg c = {DEF_LAT, DEF_LON, DEF_TZ};

    if (argc == 5 && !strcmp(argv[1], "set")) {
        /* 一次性获取GPS并保存, 之后离线运行 */
        c.lat = atof(argv[2]);
        c.lon = atof(argv[3]);
        c.tz  = atof(argv[4]);
        if (save_cfg(&c)) { perror("save"); return 1; }
        printf("GPS已保存 -> %s\n", CFG_FILE);
        return 0;
    }

    if (load_cfg(&c) == 0)
        printf("已从 %s 读取GPS\n", CFG_FILE);

    show(&c, time(NULL));
    return 0;
}
