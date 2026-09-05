/* SolarLink: 串口协议层 (UART0命令解析 + 心跳JSON序列化) */
#include "link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar.h"

#ifndef CONFIG_SOLAR_GNSS_UART_TX
#define CONFIG_SOLAR_GNSS_UART_TX 18
#endif

static const char *TAG = "link";

/* 从JSON行提取 key 后的数值, 键序无关 */
static int json_num(const char *line, const char *key, double *out) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p + strlen(key), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return sscanf(p, "%lf", out) == 1 && isfinite(*out);
}

/* 一次性应答: 完整算法结果 */
static void reply_once(double lat, double lon, double tz, int has_ts, double ts) {
    SolarResult r;
    time_t when;
    core_apply_solar(lat, lon, tz, has_ts, ts, &r, &when);
    int polar = r.rise_min < 0;
    char rb[12], nb[12], sb[12];
    int m = (int)((polar ? -1 : r.rise_min) + 0.5);
    if (polar) { strcpy(rb, "--"); strcpy(sb, "--"); } else {
        m = (int)(r.rise_min + 0.5); m %= 1440; if (m < 0) m += 1440;
        snprintf(rb, sizeof rb, "%02d:%02d", m / 60 % 24, m % 60);
        m = (int)(r.set_min + 0.5); m %= 1440; if (m < 0) m += 1440;
        snprintf(sb, sizeof sb, "%02d:%02d", m / 60 % 24, m % 60);
    }
    m = (int)(r.noon_min + 0.5); m %= 1440; if (m < 0) m += 1440;
    snprintf(nb, sizeof nb, "%02d:%02d", m / 60 % 24, m % 60);
    int sm = ((int)r.solar_min % 1440 + 1440) % 1440;

    core_lock();
    printf("{\"rise\":\"%s\",\"noon\":\"%s\",\"set\":\"%s\",\"solar\":\"%02d:%02d\","
           "\"decl\":%.2f,\"eot\":%.1f,\"to_set\":%d,\"polar\":%s}\n",
           rb, nb, sb, sm / 60, sm % 60,
           r.decl, r.eot, (int)r.to_set_min,
           polar ? (r.to_set_min > 0 ? "\"day\"" : "\"night\"") : "null");
    core_unlock();
}

/* UART0命令接收任务 */
static void cmd_task(void *arg) {
    char line[128];
    int n = 0;
    int overflow = 0;
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(5)) != 1) {
            continue; /* 超时, 已让出CPU */
        }
        if (c == '\n' || c == '\r') {
            if (!n) { overflow = 0; continue; }
            if (overflow) {
                overflow = 0;
                n = 0;
                ESP_LOGW(TAG, "命令行超过%d字节, 已丢弃", (int)sizeof(line) - 1);
                continue;
            }
            line[n] = 0;
            n = 0;
            double ts, la, lo, tz;
            char *p;
            if (strstr(line, "\"cmd\":\"nmea\"") && (p = strstr(line, "\"line\""))) {
                /* 测试注入: {"cmd":"nmea","line":"$GNRMC,..."} */
                p = strchr(p, ':');
                if (p && *++p == '"') {
                    char *end = strchr(p + 1, '"');
                    if (end) {
                        *end = 0;
                        if (core_apply_nmea(p + 1) == 0) {
                            /* 注入成功: 回一次完整应答(当前配置+现时刻) */
                            SolarCfg cfg = core_get_cfg();
                            reply_once(cfg.lat, cfg.lon, cfg.tz, 0, 0);
                        } else {
                            ESP_LOGW(TAG, "nmea注入无效");
                        }
                    }
                }
            } else if (strstr(line, "\"cmd\":\"get\"")) {
                char gbuf[80];
                core_get_cfg_json(gbuf, sizeof gbuf);
                core_lock();
                printf("%s\n", gbuf);
                core_unlock();
            } else if (strstr(line, "\"cmd\":\"stop\"")) {
                core_apply_stop();
                core_lock();
                printf("{\"ok\":\"stopped\"}\n");
                core_unlock();
            } else if (strstr(line, "\"cmd\"") && json_num(line, "\"lat\"", &la)
                       && json_num(line, "\"lon\"", &lo) && json_num(line, "\"tz\"", &tz)) {
                int has_ts = json_num(line, "\"ts\"", &ts);
                int is_init = strstr(line, "\"cmd\":\"init\"") != NULL;
                if (is_init) {
                    core_apply_init(la, lo, tz, has_ts, ts);
                }
                /* init与solar都回一次性应答(init用注入时间) */
                reply_once(la, lo, tz, has_ts, ts);
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        } else {
            overflow = 1;
        }
    }
    vTaskDelete(NULL);
}

void link_init(void) {
    xTaskCreatePinnedToCore(cmd_task, "cmd", 4096, NULL, 5, NULL, 1);
}

void link_send_hb(const CloudHb *hb, char *out, int out_n) {
    core_lock();
    int n = snprintf(out, out_n,
             "{\"t\":\"%s\",\"d\":\"%s\",\"s\":\"%s\",\"r\":\"%s\",\"st\":\"%s\","
             "\"ne\":%d,\"tne\":%d,\"dp\":%d,\"ev\":%d,\"p\":%d,"
             "\"alt\":%d,\"az\":%d,\"la\":%.4f,\"lo\":%.4f,\"syn\":%ld}",
             hb->t, hb->d, hb->s, hb->r, hb->st,
             hb->ne, hb->tne, hb->dp, hb->ev, hb->p,
             hb->alt, hb->az, hb->la, hb->lo, hb->syn);
    printf("%s\n", out);
    core_unlock();
    (void)n;
}
