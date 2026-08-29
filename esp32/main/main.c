#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar.h"

/* ================= 串口协议 (纯ASCII JSON) =================
 * 请求(一行):
 *   {"cmd":"solar","ts":..,"lat":..,"lon":..,"tz":..} 一次性计算, 回一条完整JSON
 *   {"cmd":"init", "ts":..,"lat":..,"lon":..,"tz":..} 计算一次后进入RTC心跳模式
 *   {"cmd":"stop"}                                     退出心跳模式
 * 响应:
 *   一次性: {"rise":"05:24","noon":"11:56","set":"18:28","solar":"13:55",
 *            "decl":11.42,"eot":-2.7,"to_set":269,"polar":null}
 *   心跳(100ms): {"t":"13:52:05","d":"08-26","s":"13:55","r":"05:24","st":"18:28",
 *                 "ne":0,"tne":269,"dp":62,"ev":0,"p":0,"la":31.23,"lo":121.47}
 *             ne: 0白天(距日落)/1夜晚(距日出)/2极昼/3极夜
 *             tne: 距下一事件分钟(0-1439)
 *             dp: 日光进度%  ev: 0无/1日出事件/2日落事件(±30s窗口)  p: 0正常/1极昼/-1极夜
 * 全ASCII无中文, 与GPS芯片/RTC/LCD/UI链路兼容
 * ========================================================== */
static SolarCfg g_cfg = {CONFIG_SOLAR_LAT, CONFIG_SOLAR_LON, CONFIG_SOLAR_TZ};
static time_t g_t0 = 0;      /* 时间基准: 开机时对应的UTC秒 */
static volatile int g_hb = 0; /* 心跳模式标志 */
static SemaphoreHandle_t s_out; /* 输出互斥: 防应答与心跳printf行内交错 */

static const char *TAG = "solartime";

#define HB_MS 100 /* 心跳周期(用户决策: 100ms=10Hz) */

static time_t now_t(void) { return time(NULL) + g_t0; }

/* 分钟(小数) -> "HH:MM", 四舍五入 */
static const char *hm(double min) {
    static char b[3][24];
    static int i;
    i = (i + 1) % 3;
    int m = (int)(min + 0.5);
    m %= 1440;
    if (m < 0) m += 1440;
    snprintf(b[i], sizeof b[i], "%02d:%02d", m / 60, m % 60);
    return b[i];
}

/* 从JSON行提取 key 后的数值, 键序无关 */
static int json_num(const char *line, const char *key, double *out) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p + strlen(key), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return sscanf(p, "%lf", out) == 1;
}

/* 一次性完整应答 */
static void send_json(const SolarResult *r) {
    int polar = r->rise_min < 0;
    int sm = ((int)r->solar_min % 1440 + 1440) % 1440;
    xSemaphoreTake(s_out, portMAX_DELAY);
    printf("{\"rise\":\"%s\",\"noon\":\"%s\",\"set\":\"%s\",\"solar\":\"%02d:%02d\","
           "\"decl\":%.2f,\"eot\":%.1f,\"to_set\":%d,\"polar\":%s}\n",
           polar ? "--" : hm(r->rise_min),
           hm(r->noon_min),
           polar ? "--" : hm(r->set_min),
           sm / 60, sm % 60,
           r->decl, r->eot, (int)r->to_set_min,
           polar ? (r->to_set_min > 0 ? "\"day\"" : "\"night\"") : "null");
    xSemaphoreGive(s_out);
}

/* 心跳包: 时钟+日出日落+太阳时+距日落+日光进度+事件 */
static void send_hb(time_t now, const SolarCfg *cfg, const SolarResult *r) {
    int polar = r->rise_min < 0 ? (r->to_set_min > 0 ? 1 : -1) : 0;
    int sm = ((int)r->solar_min % 1440 + 1440) % 1440;

    /* 本地墙钟 */
    time_t lt = now + (time_t)(cfg->tz * 3600.0);
    struct tm *tm = gmtime(&lt);
    int cur_s = (lt % 86400 + 86400) % 86400;

    /* 日光进度(亚分钟精度): 极昼100 极夜0, 其余按日出日落区间钳制 */
    int dp;
    if (polar == 1) {
        dp = 100;
    } else if (polar == -1) {
        dp = 0;
    } else {
        double len = r->set_min - r->rise_min;
        if (len <= 0) len += 1440.0;
        double p = (cur_s / 60.0 - r->rise_min) * 100.0 / len;
        dp = (int)(p + 0.5);
        if (dp < 0) dp = 0;
        else if (dp > 100) dp = 100;
    }

    /* 日出/日落事件: ±30s 窗口, 亚分钟精度 */
    int ev = 0;
    if (!polar) {
        double d1 = cur_s - r->rise_min * 60.0;
        double d2 = cur_s - r->set_min * 60.0;
        d1 = fmod(d1 + 43200.0, 86400.0) - 43200.0;
        d2 = fmod(d2 + 43200.0, 86400.0) - 43200.0;
        if (fabs(d1) <= 30.0) ev = 1;
        else if (fabs(d2) <= 30.0) ev = 2;
    }

    /* 昼夜判定 + 下一事件: ne 0白天(距日落)/1夜晚(距日出)/2极昼/3极夜
     * 用亚分钟事件时刻, tne 四舍五入到分钟 */
    int ne, tne;
    if (polar == 1) {
        ne = 2; tne = 0;
    } else if (polar == -1) {
        ne = 3; tne = 0;
    } else {
        double cur_min = cur_s / 60.0;
        double wr = r->rise_min, ws = r->set_min;
        double dr = wr - cur_min;
        double ds = ws - cur_min;
        if (dr <= 0) dr += 1440.0;
        if (ds <= 0) ds += 1440.0;
        int day = (ws > wr) ? (cur_min >= wr && cur_min < ws)
                            : (cur_min >= wr || cur_min < ws); /* 日落跨午夜 */
        if (day) { ne = 0; tne = (int)(ds + 0.5); }
        else     { ne = 1; tne = (int)(dr + 0.5); }
    }

    xSemaphoreTake(s_out, portMAX_DELAY);
    printf("{\"t\":\"%02d:%02d:%02d\",\"d\":\"%02d-%02d\",\"s\":\"%02d:%02d\","
           "\"r\":\"%s\",\"st\":\"%s\",\"ne\":%d,\"tne\":%d,\"dp\":%d,\"ev\":%d,"
           "\"p\":%d,\"la\":%.4f,\"lo\":%.4f}\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec,
           tm->tm_mon + 1, tm->tm_mday,
           sm / 60, sm % 60,
           polar ? "--" : hm(r->rise_min),
           polar ? "--" : hm(r->set_min),
           ne, tne, dp, ev, polar,
           cfg->lat, cfg->lon);
    xSemaphoreGive(s_out);
}

/* 串口命令: 原始UART驱动轮询(5ms超时让出CPU, 避免stdio缓冲/锁与心跳printf互斥) */
static void cmd_task(void *arg) {
    char line[128];
    int n = 0;
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(5)) != 1) {
            continue; /* 超时, 已让出CPU */
        }
        if (c == '\n' || c == '\r') {
            if (!n) continue;
            line[n] = 0;
            n = 0;
            double ts, la, lo, tz;
            SolarResult r;
            if (strstr(line, "\"cmd\":\"get\"")) {
                printf("{\"lat\":%.6f,\"lon\":%.6f,\"tz\":%g}\n",
                       g_cfg.lat, g_cfg.lon, g_cfg.tz);
            } else if (strstr(line, "\"cmd\":\"stop\"")) {
                g_hb = 0;
                printf("{\"ok\":\"stopped\"}\n");
            } else if (strstr(line, "\"cmd\"") && json_num(line, "\"lat\"", &la)
                       && json_num(line, "\"lon\"", &lo) && json_num(line, "\"tz\"", &tz)) {
                if (!json_num(line, "\"ts\"", &ts)) ts = 0;
                g_cfg = (SolarCfg){la, lo, tz};
                if (ts > 0) g_t0 = (time_t)ts - time(NULL);
                solar_compute(&g_cfg, now_t(), &r);
                send_json(&r);
                if (strstr(line, "\"cmd\":\"init\"")) g_hb = 1;
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    /* v6控制台默认裸寄存器轮询, 无uart驱动; 显式安装以支持阻塞读取,
     * 输出侧printf仍走原控制台路径不受影响 */
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    s_out = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "SolarTime ready, GPS: %.4f %.4f UTC%+g", g_cfg.lat, g_cfg.lon, g_cfg.tz);

    xTaskCreatePinnedToCore(cmd_task, "cmd", 4096, NULL, 5, NULL, 1);

    for (;;) {
        if (g_hb) {
            SolarResult r;
            solar_compute(&g_cfg, now_t(), &r);
            send_hb(now_t(), &g_cfg, &r);
        }
        vTaskDelay(pdMS_TO_TICKS(HB_MS));
    }
}
