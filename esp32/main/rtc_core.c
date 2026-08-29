/* DS3231 纯逻辑层: BCD换算与写帧构造(可PC单测, 无硬件依赖) */
#include "rtc_core.h"

uint8_t rtc_core_dec2bcd(uint8_t d) { return (d / 10) << 4 | (d % 10); }
uint8_t rtc_core_bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

void rtc_core_build(const RtcTime *t, uint8_t first[7], uint8_t sec[2]) {
    uint8_t mon = rtc_core_dec2bcd((uint8_t)t->mon);
    if (t->year >= 2100) mon |= 0x80; /* 世纪位(月寄存器BIT7) */
    first[0] = 0x01;                  /* 寄存器指针: 从分开始 */
    first[1] = rtc_core_dec2bcd((uint8_t)t->min);
    first[2] = rtc_core_dec2bcd((uint8_t)t->hour); /* 24小时制(BIT6=0) */
    first[3] = 1;                     /* 星期占位(芯片自增) */
    first[4] = rtc_core_dec2bcd((uint8_t)t->mday);
    first[5] = mon;
    first[6] = rtc_core_dec2bcd((uint8_t)(t->year % 100));
    sec[0] = 0x00;                    /* 秒寄存器指针 */
    sec[1] = rtc_core_dec2bcd((uint8_t)t->sec);
}
