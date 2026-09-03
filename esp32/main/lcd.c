/* ST7789 240x240 SPI LCD 驱动 (esp_lcd 组件)
 * 接线: SCK=GPIO11 MOSI=GPIO12 DC=GPIO13 RES=GPIO14 CS=GPIO15 BLK=GPIO16
 *
 * 渲染: 从心跳JSON解析字段, 用自绘简易字体绘制到帧缓冲,
 * 纯本地绘制(数据来自固件solar_compute结果, 不依赖PC)
 */
#include "lcd.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST SPI2_HOST

#ifndef CONFIG_SOLAR_LCD_PIN_SCK
#define CONFIG_SOLAR_LCD_PIN_SCK 11
#endif
#ifndef CONFIG_SOLAR_LCD_PIN_MOSI
#define CONFIG_SOLAR_LCD_PIN_MOSI 12
#endif
#ifndef CONFIG_SOLAR_LCD_PIN_DC
#define CONFIG_SOLAR_LCD_PIN_DC 13
#endif
#ifndef CONFIG_SOLAR_LCD_PIN_RES
#define CONFIG_SOLAR_LCD_PIN_RES 14
#endif
#ifndef CONFIG_SOLAR_LCD_PIN_CS
#define CONFIG_SOLAR_LCD_PIN_CS 15
#endif
#ifndef CONFIG_SOLAR_LCD_PIN_BLK
#define CONFIG_SOLAR_LCD_PIN_BLK 16
#endif

#define LCD_PIN_SCK  CONFIG_SOLAR_LCD_PIN_SCK
#define LCD_PIN_MOSI CONFIG_SOLAR_LCD_PIN_MOSI
#define LCD_PIN_DC   CONFIG_SOLAR_LCD_PIN_DC
#define LCD_PIN_RES  CONFIG_SOLAR_LCD_PIN_RES
#define LCD_PIN_CS   CONFIG_SOLAR_LCD_PIN_CS
#define LCD_PIN_BLK  CONFIG_SOLAR_LCD_PIN_BLK

#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_BUF_H 32 /* 部分刷新行缓冲 */

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_buf = NULL;
static SemaphoreHandle_t s_lcd_mutex = NULL;

/* ---- 极简5x7 ASCII字体(0x20-0x7E) ---- */
#include "lcd_font.h"

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    /* ST7789 SPI需要字节序交换(高字节先发) */
    uint16_t c = (uint16_t)((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

/* 字体放大倍数 */
#define FONT_SCALE 2
#define FONT_W      (5 * FONT_SCALE)   /* 字符宽10 */
#define FONT_H      (7 * FONT_SCALE)   /* 字符高14 */
#define FONT_STEP   (FONT_H + 2)       /* 行高16 */
#define LINE_Y(n)   (2 + (n) * FONT_STEP)

static void lcd_fill(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h && yy < LCD_V_RES; yy++) {
        for (int xx = 0; xx < w && xx < LCD_H_RES; xx++) s_buf[xx] = color;
        esp_lcd_panel_draw_bitmap(s_panel, x, yy, x + w, yy + 1, s_buf);
    }
}

/* 绘制单个字符(带缩放)到 (x,y), 返回字符宽度; x越界则跳过绘制 */
static int lcd_char(int x, int y, char c, uint16_t fg, uint16_t bg) {
    const uint8_t *glyph = font5x7[(unsigned char)c - 32];
    if (x + FONT_W > LCD_H_RES) return FONT_W + 2; /* 防溢出右侧 */
    for (int row = 0; row < 7; row++) {
        for (int rep_y = 0; rep_y < FONT_SCALE; rep_y++) {
            for (int col = 0; col < 5; col++) {
                uint16_t px = (glyph[row] >> (4 - col)) & 1 ? fg : bg;
                for (int rep_x = 0; rep_x < FONT_SCALE; rep_x++)
                    s_buf[col * FONT_SCALE + rep_x] = px;
            }
            esp_lcd_panel_draw_bitmap(s_panel, x, y + row * FONT_SCALE + rep_y,
                                      x + FONT_W, y + row * FONT_SCALE + rep_y + 1, s_buf);
        }
    }
    return FONT_W + 2; /* +2间距 */
}

static int lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    while (*s && x < LCD_H_RES - FONT_W) {
        char c = *s++;
        if (c < 32 || c > 126) c = '?';
        x += lcd_char(x, y, c, fg, bg);
    }
    return x;
}

/* 清除一行并绘制文本 */
static void lcd_line(int line, const char *text, uint16_t fg, uint16_t bg) {
    lcd_fill(0, LINE_Y(line), LCD_H_RES, FONT_STEP, bg);
    lcd_text(2, LINE_Y(line), text, fg, bg);
}

int lcd_init(void) {
    esp_lcd_panel_io_handle_t io = NULL;
    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_H * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) return -1;

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = LCD_PIN_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &pcfg, &s_panel) != ESP_OK) return -1;
    if (esp_lcd_panel_reset(s_panel) != ESP_OK) return -1;
    if (esp_lcd_panel_init(s_panel) != ESP_OK) return -1;
    esp_lcd_panel_invert_color(s_panel, true); /* ST7789需要反转 */
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* 背光: 直接高电平点亮(测试); 后续可PWM调光 */
    gpio_set_direction(LCD_PIN_BLK, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_PIN_BLK, 1);

    s_buf = heap_caps_malloc(LCD_H_RES * 2, MALLOC_CAP_DMA);
    if (!s_buf) return -1;

    s_lcd_mutex = xSemaphoreCreateMutex();
    if (!s_lcd_mutex) return -1;

    lcd_fill(0, 0, LCD_H_RES, LCD_V_RES, rgb565(0, 0, 0)); /* 清黑屏 */
    ESP_LOGI(TAG, "ST7789 240x240 initialized");
    return 0;
}

/* 简易进度条: 16字符宽(=192px, 不出界), 返回pct文本行由调用者处理 */
static void lcd_progress_bar(int line, int pct, uint16_t fg, uint16_t bg) {
    char s[17];
    int filled = pct * 16 / 100;
    for (int i = 0; i < 16; i++) s[i] = i < filled ? '#' : '-';
    s[16] = 0;
    lcd_line(line, s, fg, bg);
}

/* 从心跳JSON提取字段(简化: 直接字符串搜索, 心跳格式固定) */
static void json_get(const char *json, const char *key, char *out, int n) {
    const char *p = strstr(json, key);
    *out = 0;
    if (!p) return;
    p = strchr(p + strlen(key), ':');
    if (!p) return;
    p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n-- > 1) *out++ = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && n-- > 1) *out++ = *p++;
    }
    *out = 0;
}

void lcd_render_search(int v_frames, int sats) {
    if (!s_panel || !s_buf || !s_lcd_mutex) return;
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    uint16_t W = rgb565(255, 255, 255), K = rgb565(0, 0, 0);
    uint16_t Y = rgb565(255, 255, 0), D = rgb565(128, 128, 128);
    char line[40];
    lcd_line(0, "GPS ACQUIRING", Y, K);
    snprintf(line, sizeof line, "V %d", v_frames);
    lcd_line(2, line, W, K);
    snprintf(line, sizeof line, "SATS %d", sats);
    lcd_line(3, line, W, K);
    lcd_line(5, "move to open sky", D, K);
    /* 清掉下面行防止残留 */
    lcd_fill(0, LINE_Y(1), LCD_H_RES, FONT_STEP, K);
    lcd_fill(0, LINE_Y(4), LCD_H_RES, FONT_STEP, K);
    lcd_fill(0, LINE_Y(6), LCD_H_RES, LCD_V_RES - LINE_Y(6), K);
    xSemaphoreGive(s_lcd_mutex);
}

void lcd_render_hb(const char *hb_json) {
    if (!s_panel || !s_buf || !s_lcd_mutex) return;
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    /* 颜色一律经 rgb565() 保证字节序正确 */
    uint16_t FG = rgb565(255, 255, 255), BG = rgb565(0, 0, 0);
    uint16_t ACCENT = rgb565(0, 255, 0), DIM = rgb565(128, 128, 128);
    char t[9], d[6], s[6], r[6], st[6], la[10], lo[10], ne[4], tne[8], dp[4];
    json_get(hb_json, "\"t\"", t, sizeof t);
    json_get(hb_json, "\"d\"", d, sizeof d);
    json_get(hb_json, "\"s\"", s, sizeof s);
    json_get(hb_json, "\"r\"", r, sizeof r);
    json_get(hb_json, "\"st\"", st, sizeof st);
    json_get(hb_json, "\"la\"", la, sizeof la);
    json_get(hb_json, "\"lo\"", lo, sizeof lo);
    json_get(hb_json, "\"ne\"", ne, sizeof ne);
    json_get(hb_json, "\"tne\"", tne, sizeof tne);
    json_get(hb_json, "\"dp\"", dp, sizeof dp);

    char line[64];
    /* 行0: 日期 时间 */
    snprintf(line, sizeof line, "%s %s", d, t);
    lcd_line(0, line, FG, BG);
    /* 行1: 太阳时 */
    snprintf(line, sizeof line, "SOLAR %s", s);
    lcd_line(1, line, ACCENT, BG);
    /* 行2: 坐标(截断至18字符内) */
    snprintf(line, sizeof line, "%s %s", la, lo);
    line[18] = 0;
    lcd_line(2, line, DIM, BG);
    /* 行3: 日出日落 */
    snprintf(line, sizeof line, "R %s  S %s", r, st);
    lcd_line(3, line, FG, BG);
    /* 行4: 进度条 */
    int pct = atoi(dp);
    lcd_progress_bar(4, pct, ACCENT, BG);
    /* 行5: 进度% + 下一事件 */
    const char *ne_txt = "SET";
    if (ne[0] == '1') ne_txt = "RISE";
    else if (ne[0] == '2') ne_txt = "POLAR DAY";
    else if (ne[0] == '3') ne_txt = "POLAR NIGHT";
    snprintf(line, sizeof line, "%s%%  %s %sm", dp, ne_txt, tne);
    lcd_line(5, line, FG, BG);
    /* 行6: 空行(预留事件区) */
    lcd_fill(0, LINE_Y(6), LCD_H_RES, FONT_STEP, BG);
    xSemaphoreGive(s_lcd_mutex);
}
