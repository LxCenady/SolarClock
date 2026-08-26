#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar.h"

/* ================= 串口协议 (纯ASCII JSON) =================
 * 请求(一行): {"cmd":"solar","ts":1734782400,"lat":69.6492,"lon":18.9553,"tz":1}
 * 响应(一行): {"rise":"05:24","noon":"11:56","set":"18:28","solar":"13:55",
 *              "decl":11.42,"eot":-2.7,"to_set":269,"polar":null}
 * polar: null|"day"|"night"; 极区时 rise/set 为 "--"; to_set 哨兵 ±1440
 * 与GPS芯片/RTC/LCD/UI链路兼容: 全ASCII, 无中文
 * ========================================================== */
static SolarCfg g_cfg = {CONFIG_SOLAR_LAT, CONFIG_SOLAR_LON, CONFIG_SOLAR_TZ};

static const char *TAG = "solartime";

static const char *hm(int min) {
    static char b[3][8];
    static int i;
    i = (i + 1) % 3;
    if (min < 0) min += 1440;
    min %= 1440;
    snprintf(b[i], sizeof b[i], "%02d:%02d", min / 60, min % 60);
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

static void send_json(const SolarResult *r) {
    int polar = r->rise_min < 0;
    int sm = ((int)r->solar_min % 1440 + 1440) % 1440;
    printf("{\"rise\":\"%s\",\"noon\":\"%s\",\"set\":\"%s\",\"solar\":\"%02d:%02d\","
           "\"decl\":%.2f,\"eot\":%.1f,\"to_set\":%d,\"polar\":%s}\n",
           polar ? "--" : hm(r->rise_min),
           hm(r->noon_min),
           polar ? "--" : hm(r->set_min),
           sm / 60, sm % 60,
           r->decl, r->eot, (int)r->to_set_min,
           polar ? (r->to_set_min > 0 ? "\"day\"" : "\"night\"") : "null");
}

/* 串口注入 (stdin为raw模式, 逐字符组行) */
static void cmd_task(void *arg) {
    char line[128];
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c <= 0 || c == 255) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!n) continue;
            line[n] = 0;
            n = 0;
            double ts, la, lo, tz;
            if (strstr(line, "\"cmd\"") && json_num(line, "\"lat\"", &la)
                && json_num(line, "\"lon\"", &lo) && json_num(line, "\"tz\"", &tz)) {
                if (!json_num(line, "\"ts\"", &ts)) ts = 0;
                g_cfg = (SolarCfg){la, lo, tz};
                SolarResult r;
                solar_compute(&g_cfg, (time_t)ts, &r);
                send_json(&r);
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "SolarTime ready, GPS: %.4f %.4f UTC%+g", g_cfg.lat, g_cfg.lon, g_cfg.tz);
    ESP_LOGI(TAG, "proto: {\"cmd\":\"solar\",\"ts\":<utc>,\"lat\":..,\"lon\":..,\"tz\":..}");

    xTaskCreate(cmd_task, "cmd", 4096, NULL, 5, NULL);

    for (;;) {
        SolarResult r;
        solar_compute(&g_cfg, time(NULL), &r);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
