#ifndef LCD_H
#define LCD_H

/* ST7789 240x240 SPI LCD (1.54寸)
 * 接线: SCK=GPIO11 MOSI=GPIO12 DC=GPIO13 RES=GPIO14 CS=GPIO15 BLK=GPIO16
 * 数据源: 心跳JSON(由固件计算渲染, 不依赖PC) */

/* 渲染单帧到LCD(从心跳数据绘制: 时钟/太阳时/日出日落/进度/事件) */
void lcd_render_hb(const char *hb_json);

/* 搜星中界面: V帧数+可见卫星数 */
void lcd_render_search(int v_frames, int sats);

/* 初始化ST7789 (返回0=成功) */
int lcd_init(void);

#endif
