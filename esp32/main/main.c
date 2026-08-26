#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar.h"

/* 默认GPS/时区取自 menuconfig; 之后可通过串口 GPS lat lon tz 热更新 */
static SolarCfg g_cfg = {CONFIG_SOLAR_LAT, CONFIG_SOLAR_LON, CONFIG_SOLAR_TZ};

static const char *TAG = "solartime";

static void show_day(time_t now, const SolarResult *r) {
    ESP_LOGI(TAG, "日出 %02d:%02d 正午 %02d:%02d 日落 %02d:%02d (赤纬%.2f° 均时差%+.1fmin)",
             r->rise_min / 60, r->rise_min % 60,
             r->noon_min / 60, r->noon_min % 60,
             r->set_min / 60, r->set_min % 60,
             r->decl, r->eot);
}

/* 串口注入: GPS 31.2304 121.4737 8 (stdin为raw模式, 逐字符组行) */
static void gps_task(void *arg) {
    char line[64];
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
            double la, lo;
            int tz;
            if (sscanf(line, "GPS %lf %lf %d", &la, &lo, &tz) == 3) {
                g_cfg = (SolarCfg){la, lo, (int8_t)tz};
                SolarResult r;
                solar_compute(&g_cfg, time(NULL), &r);
                ESP_LOGI(TAG, "GPS已注入: %.4f°N %.4f°E UTC%+d",
                         g_cfg.lat, g_cfg.lon, (int)g_cfg.tz);
                show_day(time(NULL), &r);
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "SolarTime 启动 GPS: %.4f°N %.4f°E UTC%+d (串口输入 GPS lat lon tz 可更新)",
             g_cfg.lat, g_cfg.lon, (int)g_cfg.tz);

    xTaskCreate(gps_task, "gps", 4096, NULL, 5, NULL);

    int last_day = -1;
    for (;;) {
        /* 无RTC/NTP时 time() 为自启动秒数, 供算法演示;
         * 接入RTC或NTP后自动按真实日期运行 */
        time_t now = time(NULL);
        SolarResult r;
        solar_compute(&g_cfg, now, &r);

        int day = (int)(now / 86400);
        if (day != last_day) {
            show_day(now, &r);
            last_day = day;
        }
        int sm = ((int)r.solar_min % 1440 + 1440) % 1440;
        ESP_LOGI(TAG, "太阳时 %02d:%02d | 距日落 %+d 分",
                 sm / 60, sm % 60, (int)r.to_set_min);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
