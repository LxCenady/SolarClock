#ifndef CORE_H
#define CORE_H

#include <stdint.h>
#include <time.h>

#include "solar.h"

/* ============ SolarCore: 计算内核 + 同步 ============
 * 职责: 时间源(DS3231/GNSS帧/注入), RTC可信判定,
 *       GNSS同步状态机(窗口/6h间隔/必须同步), NVS持久化,
 *       GNSS数据链路(UART1), NOAA计算
 * 产出: 心跳数据快照 CloudHb (由SolarLink序列化成JSON) */

typedef struct {
    char     t[32];      /* 墙钟 HH:MM:SS (数组宽防编译器截断误报) */
    char     d[16];      /* MM-DD */
    char     s[16];      /* 太阳时 HH:MM */
    char     r[16];      /* 日出 HH:MM (极区"--") */
    char     st[16];     /* 日落 HH:MM */
    int      ne;         /* 0白天/1夜晚/2极昼/3极夜 */
    int      tne;        /* 距下一事件分钟 */
    int      dp;         /* 日光进度% */
    int      ev;         /* 0无/1日出/2日落事件 */
    int      p;          /* 0正常/1极昼/-1极夜 */
    double   la, lo;     /* 坐标 */
    long     syn;        /* -1搜星中 -2从未同步 >=0距上次同步秒 */
} CloudHb;

/* ---- 接口 ---- */

/* 初始化: nvs/uart0驱动/RTC探测/读NVS缓存/同步状态机初始决策 */
void core_init(void);

/* 同步状态机调度(每秒节拍, 由主循环调用) */
void core_tick(void);

/* 计算心跳快照(100ms), 返回0=心跳可用 */
int core_compute_hb(CloudHb *hb);

/* 当前时间(时间源: RTC > g_t0+uptime) */
time_t core_now(void);

/* 当前配置(锁内副本) */
SolarCfg core_get_cfg(void);

/* 是否心跳模式 */
int core_is_hb(void);

/* 输出互斥锁(Link层JSON与Core共享状态的同步用) */
void core_lock(void);
void core_unlock(void);

/* ---- 供SolarLink命令层调用 ---- */
/* init命令: 更新配置/时间基准(可选ts)/写RTC, 进入心跳模式 */
void core_apply_init(double lat, double lon, double tz, int has_ts, double ts);
/* solar命令: 仅一次性计算, 不落状态 */
int  core_apply_solar(double lat, double lon, double tz, int has_ts, double ts,
                      SolarResult *r, time_t *when);
/* nmea注入: 走真实GNSS fix链路 */
int  core_apply_nmea(const char *nmea_line);
/* stop: 退出心跳 */
void core_apply_stop(void);
/* 取配置JSON响应字段(供get命令) */
void core_get_cfg_json(char *buf, int n);

#endif
