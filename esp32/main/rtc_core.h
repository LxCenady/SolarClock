#ifndef RTC_CORE_H
#define RTC_CORE_H

#include <stdint.h>

#include "rtc.h"

/* 纯逻辑层(无硬件依赖, 可PC单测): BCD换算与写帧构造
 * DS3231 写帧必须 [寄存器指针][数据...], 缺指针会错位! */

uint8_t rtc_core_dec2bcd(uint8_t d);
uint8_t rtc_core_bcd2dec(uint8_t b);

/* 构造两次写帧:
 *   first[7] = {0x01, min, hour, dow, mday, mon(+世纪位), year}
 *   sec[2]   = {0x00, sec}   秒最后单独写(写秒复位分频链) */
void rtc_core_build(const RtcTime *t, uint8_t first[7], uint8_t sec[2]);

#endif
