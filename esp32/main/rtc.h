#ifndef RTC_H
#define RTC_H

/* RTC 时间结构(十进制) */
typedef struct {
    int year;   /* 四位数年份 */
    int mon;    /* 1-12 */
    int mday;   /* 1-31 */
    int hour;   /* 0-23 (强制24小时制) */
    int min;
    int sec;
} RtcTime;

/* 初始化I2C总线并探测DS3231(0x68); 返回0=成功 */
int rtc_init(void);

/* 读取时间(连读00h-06h免撕裂, 检查OSF); 返回0=成功, -1=OSF(时间不可信) */
int rtc_read(RtcTime *t);

/* 写入时间(BCD, 24小时制, 秒最后写以复位分频链); 返回0=成功 */
int rtc_write(const RtcTime *t);

/* 读取片内温度(℃); 返回0=成功 */
int rtc_temp(float *celsius);

#endif
