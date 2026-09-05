/* SolarView: 显示层 (心跳JSON -> LCD渲染) */
#include "view.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd.h"

#if CONFIG_SOLAR_LCD
static SemaphoreHandle_t s_lcd_mutex;
#endif

void view_init(void) {
#if CONFIG_SOLAR_LCD
    s_lcd_mutex = xSemaphoreCreateMutex();
    if (lcd_init() == 0) ESP_LOGI("view", "LCD ready");
    else ESP_LOGW("view", "LCD init failed");
#else
    /* 无显示设备: 空实现 */
#endif
}

void view_render_hb(const char *hb_json) {
#if CONFIG_SOLAR_LCD
    /* 1Hz节流: LCD按秒刷新(省SPI/功耗), 从JSON的t字段取秒 */
    static char last_t[9] = "";
    const char *p = strstr(hb_json, "\"t\":\"");
    if (p && strlen(p) >= 12 && strncmp(p + 6, last_t, 8) == 0) return;
    if (p && strlen(p) >= 12) memcpy(last_t, p + 6, 8), last_t[8] = 0;
    lcd_render_hb(hb_json);
#else
    (void)hb_json;
#endif
}

void view_render_search(void) {
#if CONFIG_SOLAR_LCD
    lcd_render_search(0, 0);
#endif
}

int view_need_search(void) {
#if CONFIG_SOLAR_LCD
    return 1;
#else
    return 0;
#endif
}
