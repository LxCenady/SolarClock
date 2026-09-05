/* SolarView: 显示层 (心跳JSON -> LCD渲染, 20Hz判定+分块脏检查) */
#include "view.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd.h"

#if CONFIG_SOLAR_LCD
static SemaphoreHandle_t s_lcd_mutex;
static char s_hb_cache[256];
static volatile int s_has_hb = 0;
#endif

void view_init(void) {
#if CONFIG_SOLAR_LCD
    s_lcd_mutex = xSemaphoreCreateMutex();
    if (lcd_init() == 0) ESP_LOGI("view", "LCD ready");
    else ESP_LOGW("view", "LCD init failed");
#endif
}

/* 喂最新心跳JSON (main按心跳节拍调用; lcd内分块脏检查只刷变化块) */
void view_render_hb(const char *hb_json) {
#if CONFIG_SOLAR_LCD
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    strncpy(s_hb_cache, hb_json, sizeof s_hb_cache - 1);
    s_hb_cache[sizeof s_hb_cache - 1] = 0;
    s_has_hb = 1;
    xSemaphoreGive(s_lcd_mutex);
    lcd_render_hb(s_hb_cache);
#else
    (void)hb_json;
#endif
}

/* 20Hz判定入口: main按50ms调用; 无变化时lcd分块全部跳过, 零绘制 */
void view_poll(void) {
#if CONFIG_SOLAR_LCD
    if (s_has_hb) lcd_render_hb(s_hb_cache);
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
