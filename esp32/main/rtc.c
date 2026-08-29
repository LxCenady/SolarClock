/* DS3231 RTC 占位驱动 (I2C0, 0x68)
 * 物理接入见 GNSS_RTC_PROTOCOL.md: SCL=GPIO9 SDA=GPIO8, 4.7kΩ上拉
 * 寄存器: 00h-06h时间(BCD), 0Eh控制, 0Fh状态(OSF), 11h-12h温度 */
#include "rtc.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rtc_core.h"

#define RTC_ADDR  0x68
#define REG_SEC   0x00
#define REG_STAT  0x0F
#define REG_TEMP  0x11

#ifndef CONFIG_SOLAR_RTC_SDA
#define CONFIG_SOLAR_RTC_SDA 8
#endif
#ifndef CONFIG_SOLAR_RTC_SCL
#define CONFIG_SOLAR_RTC_SCL 9
#endif

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev = NULL;

int rtc_init(void) {
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_SOLAR_RTC_SDA,
        .scl_io_num = CONFIG_SOLAR_RTC_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t h;
    if (i2c_new_master_bus(&bus, &h) != ESP_OK) return -1;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(h, &dev, &s_dev) != ESP_OK) return -1;

    /* 探测: 读状态寄存器 */
    uint8_t st = 0;
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){REG_STAT}, 1, &st, 1, 1000) != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 未响应(检查接线)");
        return -1;
    }
    ESP_LOGI(TAG, "DS3231 ok, 状态=0x%02x (OSF=%d)", st, st >> 7 & 1);
    return 0;
}

int rtc_read(RtcTime *t) {
    if (!s_dev) return -1;
    uint8_t b[7];
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){REG_SEC}, 1, b, 7, 1000) != ESP_OK)
        return -1;
    uint8_t st = 0;
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){REG_STAT}, 1, &st, 1, 1000) != ESP_OK)
        return -1;
    if (st >> 7 & 1) return -1; /* OSF: 振荡器曾停振, 时间不可信 */

    t->sec = rtc_core_bcd2dec(b[0]);
    t->min = rtc_core_bcd2dec(b[1]);
    t->hour = rtc_core_bcd2dec(b[2] & 0x3F);      /* 强制24小时制 */
    t->mday = rtc_core_bcd2dec(b[4]);
    int mon = rtc_core_bcd2dec(b[5] & 0x1F);
    int cent = b[5] >> 7 & 1;            /* 世纪位 */
    t->year = 2000 + rtc_core_bcd2dec(b[6]) + (cent ? 100 : 0);
    t->mon = mon;
    return 0;
}

int rtc_write(const RtcTime *t) {
    if (!s_dev) return -1;
    /* 帧结构: [寄存器指针][数据...] (rtc_core 构造, 单测覆盖)
     * 秒最后单独写: 写秒寄存器会复位分频链 */
    uint8_t first[7], sec_frame[2];
    rtc_core_build(t, first, sec_frame);
    if (i2c_master_transmit(s_dev, first, sizeof first, 1000) != ESP_OK) return -1;
    if (i2c_master_transmit(s_dev, sec_frame, sizeof sec_frame, 1000) != ESP_OK) return -1;
    return 0;
}

int rtc_temp(float *celsius) {
    if (!s_dev) return -1;
    uint8_t b[2];
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){REG_TEMP}, 1, b, 2, 1000) != ESP_OK)
        return -1;
    int16_t raw = (int16_t)((b[0] << 8) | b[1]) >> 6; /* 10位补码 */
    *celsius = raw / 4.0f;
    return 0;
}
