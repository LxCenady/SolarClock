/* ST7789 240x240 SPI LCD 驱动 (esp_lcd 组件)
 * 接线: SCK=GPIO11 MOSI=GPIO12 DC=GPIO13 RES=GPIO14 CS=GPIO15 BLK=GPIO16
 *
 * 渲染: 从心跳JSON解析字段, 用自绘简易字体绘制到帧缓冲,
 * 纯本地绘制(数据来自固件solar_compute结果, 不依赖PC)
 */
#include "lcd.h"

#include <math.h>
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
#include "lcd_font.h"

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
static uint16_t *g_gfx = NULL;   /* 图形区帧缓冲: 高度刻度+罗盘 */
#define GFX_H 90
#define GFX_W LCD_H_RES

/* ---- 图形原语(写局部帧缓冲 g_gfx, 整块后blit) ---- */
static void gfx_clear(uint16_t c) {
    for (int i = 0; i < GFX_W * GFX_H; i++) g_gfx[i] = c;
}
static void gfx_px(int x, int y, uint16_t c) {
    if (x >= 0 && x < GFX_W && y >= 0 && y < GFX_H) g_gfx[y * GFX_W + x] = c;
}
static void gfx_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) + 1;
    for (int i = 0; i <= steps; i++) {
        gfx_px(x0 + dx * i / steps, y0 + dy * i / steps, c);
    }
}
static void gfx_circle(int cx, int cy, int r, uint16_t c) {
    int x0 = cx - r, x1 = cx + r;
    for (int x = x0; x <= x1; x++) {
        int dx = x - cx;
        int dy = (int)sqrt((double)(r * r - dx * dx));
        gfx_px(x, cy - dy, c);
        gfx_px(x, cy + dy, c);
    }
}
static void gfx_char(int x, int y, char ch, uint16_t c) { /* 1x1 5x7字体 */
    const uint8_t *glyph = font5x7[(unsigned char)ch - 32];
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if ((glyph[row] >> (4 - col)) & 1) gfx_px(x + col, y + row, c);
}
static void gfx_text(int x, int y, const char *s, uint16_t c) {
    while (*s) { gfx_char(x, y, *s++, c); x += 6; }
}
/* 整块blit到LCD */
static void gfx_blit(int y0) {
    for (int yy = 0; yy < GFX_H; yy++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0 + yy, GFX_W, y0 + yy + 1,
                                  g_gfx + yy * GFX_W);
    }
}
static SemaphoreHandle_t s_lcd_mutex = NULL;

/* ---- 极简5x7 ASCII字体(0x20-0x7E) ---- */

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
    g_gfx = heap_caps_malloc(GFX_W * GFX_H * 2, MALLOC_CAP_8BIT);
    if (!g_gfx) return -1;

    s_lcd_mutex = xSemaphoreCreateMutex();
    if (!s_lcd_mutex) return -1;

    lcd_fill(0, 0, LCD_H_RES, LCD_V_RES, rgb565(0, 0, 0)); /* 清黑屏 */
    ESP_LOGI(TAG, "ST7789 240x240 initialized");
    return 0;
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

/* ========== 太阳360°显示: 左侧高度刻度 + 右侧罗盘 (90px) ==========
 * 高度刻度: 0-90° 15°间隔, 绿线, 黄三角指示(日落后固定最低点)
 * 罗盘: N/S/E/W 圆周刻度(r=40), 中心直线段指向az
 * 数字 ALT/AZ 由调用方画在图形正上方 */
static void lcd_render_sun_graphics(int alt, int az, int hide_ptr) {
    uint16_t K = rgb565(0, 0, 0);
    uint16_t GRN = rgb565(0, 255, 0), YEL = rgb565(255, 255, 0);
    uint16_t W = rgb565(255, 255, 255), DIM = rgb565(128, 128, 128);

    gfx_clear(K);

    /* --- 左侧高度刻度: 映射 0(86px)->90(4px) --- */
    int xL = 20, yTop = 4, yBot = 86;
    gfx_line(xL, yTop, xL, yBot, DIM);
    for (int d = 0; d <= 90; d += 15) {
        int y = yBot - d * (yBot - yTop) / 90.0f;
        gfx_line(xL - 3, y, xL + 3, y, W);
        char s[4];
        snprintf(s, sizeof s, "%d", d);
        gfx_text(xL - 12, y - 3, s, DIM);
    }
    /* 绿色填充 0°→当前高度; 日落后固定最低点 */
    int altC = alt < 0 ? 0 : (alt > 90 ? 90 : alt);
    int yA = yBot - altC * (yBot - yTop) / 90.0f;
    gfx_line(xL + 1, yBot, xL + 1, yA, GRN);
    /* 黄色三角指示 */
    gfx_line(xL - 4, yA, xL + 6, yA, YEL);
    gfx_line(xL - 4, yA, xL + 1, yA - 3, YEL);
    gfx_line(xL - 4, yA, xL + 1, yA + 3, YEL);

    /* --- 右侧罗盘: 圆心(r=34, 图区90px内放下圆+NSWE), NSWE贴圆外缘 --- */
    int cx = 140, cy = 45, R = 34;
    gfx_circle(cx, cy, R, DIM);
    gfx_text(cx - 3, cy - R - 8, "N", W);   /* 贴圆上缘 */
    gfx_text(cx - 3, cy + R + 2, "S", W);
    gfx_text(cx - R - 8, cy - 3, "W", W);
    gfx_text(cx + R + 2, cy - 3, "E", W);
    if (!hide_ptr) { /* 夜晚/极夜隐藏指针(太阳在地平线下) */
        double rad = az * 3.14159265358979 / 180.0;
        int x2 = cx + (int)((R - 4) * sin(rad));
        int y2 = cy - (int)((R - 4) * cos(rad));
        gfx_line(cx, cy, x2, y2, YEL);
        gfx_px(cx, cy, YEL);
    }
}

void lcd_render_hb(const char *hb_json) {
    if (!s_panel || !s_buf || !s_lcd_mutex) return;
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    uint16_t FG = rgb565(255, 255, 255), BG = rgb565(0, 0, 0);
    uint16_t ACCENT = rgb565(0, 255, 0), DIM = rgb565(128, 128, 128);
    uint16_t ORG = rgb565(255, 128, 0), YEL = rgb565(255, 255, 0);

    /* 解析全部字段 */
    char t[9], d[6], s[6], r[6], st[6], la[10], lo[10], ne[4], tne[8], dp[4];
    char syn[16], alt[8], az[8];
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
    json_get(hb_json, "\"syn\"", syn, sizeof syn);
    json_get(hb_json, "\"alt\"", alt, sizeof alt);
    json_get(hb_json, "\"az\"", az, sizeof az);

    /* 静态缓存(跨帧比较) */
    static char c_t[9]="", c_d[6]="", c_s[6]="", c_r[6]="", c_st[6]="", c_ne[4]="";
    static char c_tne[8]="", c_dp[4]="", c_syn[16]="", c_alt[8]="", c_az[8]="", c_loc[20]="";
    char line[64];

    /* 块1: 日期(日变才刷) + 时间(秒变刷), 分段绘制 */
    if (strcmp(d, c_d)) { strcpy(c_d, d);
        lcd_fill(2, LINE_Y(0), 6 * 12, FONT_STEP, BG);
        lcd_text(2, LINE_Y(0), d, FG, BG);
    }
    if (strcmp(t, c_t)) { strcpy(c_t, t);
        lcd_fill(2 + 6 * 12, LINE_Y(0), 9 * 12, FONT_STEP, BG);
        lcd_text(2 + 6 * 12, LINE_Y(0), t, FG, BG);
    }
    /* 块2: 太阳时行(1) - 分钟变刷 */
    if (strcmp(s, c_s)) { strcpy(c_s, s);
        snprintf(line, sizeof line, "SOLAR %s", s);
        lcd_line(1, line, ACCENT, BG);
    }
    /* 块3: 日出日落行(2) - 天变刷 */
    if (strcmp(r, c_r) || strcmp(st, c_st)) { strcpy(c_r, r); strcpy(c_st, st);
        snprintf(line, sizeof line, "R %s  S %s", r, st);
        lcd_line(2, line, FG, BG);
    }
    /* 块4+5共用脏标志(先算后更新, 否则块5恒false)
     * 夜晚(ne==1)/极夜(ne==3): az显示"--", 罗盘指针隐藏(太阳在地平线下) */
    int night = (ne[0] == '1' || ne[0] == '3');
    const char *az_disp = night ? "--" : az;
    int az_alt_dirty = strcmp(alt, c_alt) || strcmp(az_disp, c_az);
    if (az_alt_dirty) { strcpy(c_alt, alt); strcpy(c_az, az_disp);
        snprintf(line, sizeof line, "ALT %s   AZ %s", alt, az_disp);
        lcd_line(3, line, YEL, BG);
        lcd_render_sun_graphics(atoi(alt), atoi(az), night);
        gfx_blit(66);
    }
    /* 块6: 进度条+%+事件 - dp/ne/tne变刷 */
    if (strcmp(dp, c_dp) || strcmp(ne, c_ne) || strcmp(tne, c_tne)) {
        strcpy(c_dp, dp); strcpy(c_ne, ne); strcpy(c_tne, tne);
        int pct = atoi(dp);
        lcd_fill(2, 166, 16 * 14, 8, BG);
        if (ne[0] != '1') {
            for (int c = 0; c < 16; c++)
                lcd_fill(2 + c * 14, 166, 10, 8, c * 100 / 16 < pct ? ORG : DIM);
        }
        const char *ne_txt = "SET";
        if (ne[0] == '1') ne_txt = "RISE";
        else if (ne[0] == '2') ne_txt = "POLAR DAY";
        else if (ne[0] == '3') ne_txt = "POLAR NIGHT";
        snprintf(line, sizeof line, "%s%%  %s %sm", dp, ne_txt, tne);
        lcd_fill(0, 178, LCD_H_RES, FONT_STEP, BG);
        lcd_text(2, 178, line, FG, BG);
    }
    /* 块7: 同步状态 - 显示级变刷(h/m/s取整) */
    long sv = atol(syn);
    char syncbuf[24];
    const char *sy;
    uint16_t syc = DIM;
    if (sv == -1) { sy = "SYNCING"; syc = YEL; }
    else if (sv == -2) { sy = "NO SYNC"; syc = rgb565(255, 80, 0); }
    else {
        if (sv >= 3600) snprintf(syncbuf, sizeof syncbuf, "SYNCED %ldh AGO", sv / 3600);
        else if (sv >= 60) snprintf(syncbuf, sizeof syncbuf, "SYNCED %ldm AGO", sv / 60);
        else snprintf(syncbuf, sizeof syncbuf, "SYNCED %lds AGO", sv);
        sy = syncbuf; syc = ACCENT;
    }
    if (strcmp(sy, c_syn)) { strcpy(c_syn, sy);
        lcd_fill(0, 194, LCD_H_RES, FONT_STEP, BG);
        lcd_text(2, 194, sy, syc, BG);
    }
    /* 块8: 坐标 - 几乎不变 */
    snprintf(line, sizeof line, "%s %s", la, lo);
    line[18] = 0;
    if (strcmp(line, c_loc)) { strcpy(c_loc, line);
        lcd_fill(0, 210, LCD_H_RES, FONT_STEP, BG);
        lcd_text(2, 210, line, DIM, BG);
    }
    xSemaphoreGive(s_lcd_mutex);
}