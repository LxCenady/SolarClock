/* SolarCore: 计算内核 + 同步 (时间源/RTC/GNSS/NVS/算法) */
#include "core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss.h"
#include "nvs_flash.h"
#include "rtc.h"

#ifndef CONFIG_SOLAR_GNSS_UART_RX
#define CONFIG_SOLAR_GNSS_UART_RX 17
#endif
#ifndef CONFIG_SOLAR_GNSS_UART_TX
#define CONFIG_SOLAR_GNSS_UART_TX 18
#endif
#ifndef CONFIG_SOLAR_GNSS_TZ
#define CONFIG_SOLAR_GNSS_TZ 8
#endif
#ifndef CONFIG_SOLAR_GNSS_BAUD
#define CONFIG_SOLAR_GNSS_BAUD 115200
#endif

/* ============ 状态 (全局, 静态于内核) ============ */
static SolarCfg g_cfg = {CONFIG_SOLAR_LAT, CONFIG_SOLAR_LON, CONFIG_SOLAR_TZ};
static time_t g_t0 = 0;
static volatile int g_hb = 0;
static SemaphoreHandle_t s_out = NULL;
#if CONFIG_SOLAR_RTC
static volatile int s_rtc_ok = 0;
#endif

/* GNSS 同步状态机 */
#define SYNC_WIN_S    600
#define SYNC_GAP_S    21600
static volatile int  s_syncing = 0;
static volatile int  s_sync_must = 0;
static time_t s_win_start = 0;
static time_t s_last_sync = 0;

static const char *TAG = "core";
#define D2R_LOCAL 0.017453292519943295

/* ---- 锁 ---- */
void core_lock(void) { xSemaphoreTake(s_out, portMAX_DELAY); }
void core_unlock(void) { xSemaphoreGive(s_out); }

/* ---- 时间源 ---- */
time_t core_now(void) {
#if CONFIG_SOLAR_RTC
    if (s_rtc_ok) {
        RtcTime t;
        if (ds3231_read(&t) == 0)
            return gnss_mkts(t.year, t.mon, t.mday, t.hour, t.min, t.sec);
    }
#endif
    time_t t0;
    core_lock();
    t0 = g_t0;
    core_unlock();
    return time(NULL) + t0;
}

static int rtc_trusted(void) {
#if CONFIG_SOLAR_RTC
    if (!s_rtc_ok) return 0;
    RtcTime t;
    if (ds3231_read(&t) != 0) return 0;
    if (t.year < 2024 || t.year > 2099) return 0; /* 出厂默认/丢电 */
    /* 若上次同步时间已知且 RTC 比之早超过1天, 视为被污染(测试注入旧时间等):
     * GNSS同步的时间只会前进, 真实RTC不应早于last_sync */
    if (s_last_sync > 0) {
        time_t rts = gnss_mkts(t.year, t.mon, t.mday, t.hour, t.min, t.sec);
        if (rts < s_last_sync - 86400) return 0;
    }
    return 1;
#else
    return 0;
#endif
}

SolarCfg core_get_cfg(void) {
    SolarCfg c;
    core_lock();
    c = g_cfg;
    core_unlock();
    return c;
}

int core_is_hb(void) { return g_hb; }

/* ---- NVS 持久化 ---- */
#define NVS_KEY "cfg"
static void nvs_load_cfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_KEY, NVS_READONLY, &h) != ESP_OK) return;
    double v[3];
    size_t len = sizeof v;
    if (nvs_get_blob(h, "llt", v, &len) == ESP_OK && len == sizeof v
        && v[0] >= -90 && v[0] <= 90 && v[1] >= -180 && v[1] <= 180) {
        g_cfg.lat = v[0];
        g_cfg.lon = v[1];
        g_cfg.tz = v[2];
        ESP_LOGI(TAG, "NVS缓存坐标: %.4f %.4f UTC%+g", g_cfg.lat, g_cfg.lon, g_cfg.tz);
    }
    nvs_close(h);
}

static void nvs_save_cfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_KEY, NVS_READWRITE, &h) != ESP_OK) return;
    double v[3] = {g_cfg.lat, g_cfg.lon, g_cfg.tz};
    nvs_set_blob(h, "llt", v, sizeof v);
    nvs_set_i64(h, "lsync", (int64_t)s_last_sync);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_last_sync(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_KEY, NVS_READONLY, &h) != ESP_OK) return;
    int64_t v = 0;
    if (nvs_get_i64(h, "lsync", &v) == ESP_OK && v > 0)
        s_last_sync = (time_t)v;
    nvs_close(h);
}

/* ---- 命令层接口: init/solar/stop/nmea ---- */

static void do_sync_done(time_t tnow) {
    s_last_sync = tnow;
    s_syncing = 0;
    s_sync_must = 0;
    /* NVS持久化(限频5min) */
    static time_t s_last_nvs = 0;
    static double s_last_lat = 1e9, s_last_lon;
    if (fabs(g_cfg.lat - s_last_lat) > 0.001 || fabs(g_cfg.lon - s_last_lon) > 0.001
        || tnow - s_last_nvs >= 300) {
        s_last_lat = g_cfg.lat;
        s_last_lon = g_cfg.lon;
        s_last_nvs = tnow;
        nvs_save_cfg();
        ESP_LOGI(TAG, "NVS已存(坐标/同步时间)");
    }
}

/* GNSS fix 统一处理: 更新配置->(对时RTC)->重算->心跳->同步完成 */
static void handle_fix(const GnssFix *fix) {
    if (!fix->valid) return;
    if (fix->lat < -90.0 || fix->lat > 90.0 || fix->lon < -180.0 || fix->lon > 180.0) {
        ESP_LOGW(TAG, "GNSS FIX 坐标越界, 忽略: %.6f %.6f", fix->lat, fix->lon);
        return;
    }

    SolarCfg cfg;
    core_lock();
    cfg = (SolarCfg){fix->lat, fix->lon, CONFIG_SOLAR_GNSS_TZ};
    g_cfg = cfg;
    g_t0 = fix->ts - time(NULL);
    core_unlock();

#if CONFIG_SOLAR_RTC
    if (s_rtc_ok) {
        struct tm *tm = gmtime(&fix->ts);
        if (tm) {
            ds3231_write(&(RtcTime){tm->tm_year + 1900, tm->tm_mon + 1,
                                 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec});
            ESP_LOGI(TAG, "GNSS FIX -> RTC 对时");
        }
    }
#endif

    g_hb = 1;
    ESP_LOGI(TAG, "GNSS FIX %.6f %.6f ts=%lld -> 心跳模式",
             cfg.lat, cfg.lon, (long long)fix->ts);
    do_sync_done(core_now());
}

void core_apply_init(double lat, double lon, double tz, int has_ts, double ts) {
    SolarCfg cfg = {lat, lon, tz};
    core_lock();
    g_cfg = cfg;
    if (has_ts) g_t0 = (time_t)ts - time(NULL);
    core_unlock();
#if CONFIG_SOLAR_RTC
    if (s_rtc_ok && has_ts) {
        struct tm *tm = gmtime(&(time_t){ts});
        if (tm) {
            ds3231_write(&(RtcTime){tm->tm_year + 1900, tm->tm_mon + 1,
                                 tm->tm_mday, tm->tm_hour, tm->tm_min,
                                 tm->tm_sec});
        }
    }
#endif
    g_hb = 1;
}


int core_apply_solar(double lat, double lon, double tz, int has_ts, double ts,
                     SolarResult *r, time_t *when) {
    SolarCfg cfg = {lat, lon, tz};
    *when = has_ts ? (time_t)ts : core_now();
    solar_compute(&cfg, *when, r);
    return 0;
}

int core_apply_nmea(const char *nmea_line) {
    GnssFix fix;
    if (gnss_parse_rmc(nmea_line, &fix) != 0) return -1;
    handle_fix(&fix);
    return 0;
}

void core_apply_stop(void) {
    g_hb = 0;
}

/* 调用处需已持有锁(link层)或自己做: get返回缓存 */
void core_get_cfg_json(char *buf, int n) {
    SolarCfg c = core_get_cfg();
    snprintf(buf, n, "{\"lat\":%.6f,\"lon\":%.6f,\"tz\":%g}", c.lat, c.lon, c.tz);
}

/* ---- GNSS UART1 数据链路 + 搜星窗口门控 ---- */
#if CONFIG_SOLAR_GNSS
static void pcas_send(const char *body) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "$%s", body);
    if (n < 0 || n >= (int)sizeof buf) return;
    unsigned char cs = 0;
    for (int i = 1; i < n; i++) cs ^= (unsigned char)buf[i];
    int m = snprintf(buf + n, sizeof buf - n, "*%02X\r\n", cs);
    if (m < 0 || m >= (int)(sizeof buf - n)) return;
    uart_write_bytes(UART_NUM_1, buf, n + m);
}

static void gnss_task(void *arg) {
    uart_config_t uc = {
        .baud_rate = CONFIG_SOLAR_GNSS_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_1, &uc);
    uart_set_pin(UART_NUM_1, CONFIG_SOLAR_GNSS_UART_TX,
                 CONFIG_SOLAR_GNSS_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "GNSS UART1 RX=GPIO%d %d 8N1", CONFIG_SOLAR_GNSS_UART_RX, CONFIG_SOLAR_GNSS_BAUD);

    vTaskDelay(pdMS_TO_TICKS(300));
    pcas_send("PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0,0");
    vTaskDelay(pdMS_TO_TICKS(200));
    pcas_send("PCAS00");
    ESP_LOGI(TAG, "GNSS 配置已下发(GGA+RMC)");

    char line[128];
    int n = 0;
    int overflow = 0;
    int sats = 0;
    int was_win = 0;
    GnssFix prev = {0}, fix;
    for (;;) {
        int in_win = s_syncing || s_sync_must;
        if (in_win && !was_win) {
            was_win = 1;
            uart_flush_input(UART_NUM_1);
            vTaskDelay(pdMS_TO_TICKS(100));
            pcas_send("PCAS10,0");
            ESP_LOGI(TAG, "GNSS 搜星窗口开启");
        } else if (!in_win && was_win) {
            was_win = 0;
        }
        if (!in_win) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint8_t c;
        if (uart_read_bytes(UART_NUM_1, &c, 1, pdMS_TO_TICKS(10)) != 1) continue;
        if (c == '\n') {
            if (!n) { overflow = 0; continue; }
            if (overflow) { overflow = 0; n = 0; continue; }
            line[n] = 0;
            n = 0;
            if (gnss_parse_gga(line, &sats) == 0) continue; /* GGA: 记卫星数 */
            int rc = gnss_parse_rmc(line, &fix);
            if (rc != 0) continue;
            int stable = prev.valid && fabs(fix.lat - prev.lat) < 0.001
                         && fabs(fix.lon - prev.lon) < 0.001
                         && llabs((long long)(fix.ts - prev.ts)) < 10;
            prev = fix;
            if (stable) handle_fix(&fix);
        } else if (c == '\r') {
            /* 忽略 */
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        } else {
            overflow = 1;
        }
    }
    vTaskDelete(NULL);
}
#endif /* CONFIG_SOLAR_GNSS */

/* ---- 同步状态机(每秒节拍) ---- */
void core_tick(void) {
    time_t tn = core_now();
    if (s_syncing) {
        if (!s_sync_must && tn - s_win_start > SYNC_WIN_S) {
            s_syncing = 0;
            ESP_LOGI(TAG, "搜星窗口超时(%ds), 放弃, 6h后重试", SYNC_WIN_S);
        }
    } else if (s_sync_must) {
        s_win_start = tn;
        s_syncing = 1;
        ESP_LOGI(TAG, "必须同步: 持续搜星");
    } else if (rtc_trusted() && (s_last_sync == 0 || tn - s_last_sync > SYNC_GAP_S)) {
        s_win_start = tn;
        s_syncing = 1;
        ESP_LOGI(TAG, "距上次同步>6h, 开搜星窗口(%ds)", SYNC_WIN_S);
    }
}

/* ---- 心跳快照计算 ---- */
int core_compute_hb(CloudHb *hb) {
    if (!g_hb) return -1;
    SolarCfg cfg = core_get_cfg();
    time_t now = core_now();
    SolarResult r;
    solar_compute(&cfg, now, &r);

    int polar = r.rise_min < 0 ? (r.to_set_min > 0 ? 1 : -1) : 0;
    int sm = ((int)r.solar_min % 1440 + 1440) % 1440;
    time_t lt = now + (time_t)(cfg.tz * 3600.0);
    struct tm *tm = gmtime(&lt);
    int cur_s = (lt % 86400 + 86400) % 86400;

    /* 太阳高度角/方位角 (NOAA):
     *   真太阳时 = r.solar_min 分钟; 时角 H = (真太阳时-720)*0.25 度
     *   高度 alt = asin(sin(lat)·sin(decl)+cos(lat)·cos(decl)·cos(H))
     *   方位 az = atan2(sin(H), cos(H)·sin(lat)-tan(decl)·cos(lat))  北=0 东=90 南=180 西=270 */
    {
        double H = (r.solar_min - 720.0) / 4.0;
        double latR = cfg.lat * D2R_LOCAL, declR = r.decl * D2R_LOCAL, HR = H * D2R_LOCAL;
        double altDeg = asin(sin(latR) * sin(declR)
                             + cos(latR) * cos(declR) * cos(HR)) * 180.0 / M_PI;
        /* NOAA方位角变体输出"南为0", 转北=0标准: +180 */
        double azDeg = atan2(sin(HR), cos(HR) * sin(latR) - tan(declR) * cos(latR))
                       * 180.0 / M_PI + 180.0;
        if (azDeg >= 360.0) azDeg -= 360.0;
        hb->alt = (int)(altDeg + (altDeg >= 0 ? 0.5 : -0.5));
        if (hb->alt < 0) hb->alt = 0; /* 日落后高度角归0(UI固定最低点) */
        hb->az = (int)(azDeg + 0.5) % 360;
    }

    int dp;
    if (polar == 1) dp = 100;
    else if (polar == -1) dp = 0;
    else {
        double len = r.set_min - r.rise_min;
        if (len <= 0) len += 1440.0;
        double p = fmod(cur_s / 60.0 - r.rise_min + 1440.0, 1440.0) * 100.0 / len;
        dp = (int)(p + 0.5);
        if (dp < 0) dp = 0;
        else if (dp > 100) dp = 100;
    }

    int ev = 0;
    if (!polar) {
        double d1 = cur_s - r.rise_min * 60.0;
        double d2 = cur_s - r.set_min * 60.0;
        if (d1 > 43200.0) d1 -= 86400.0;
        else if (d1 < -43200.0) d1 += 86400.0;
        if (d2 > 43200.0) d2 -= 86400.0;
        else if (d2 < -43200.0) d2 += 86400.0;
        if (fabs(d1) <= 30.0) ev = 1;
        else if (fabs(d2) <= 30.0) ev = 2;
    }

    int ne, tne;
    if (polar == 1) { ne = 2; tne = 0; }
    else if (polar == -1) { ne = 3; tne = 0; }
    else {
        double cur_min = cur_s / 60.0;
        double wr = r.rise_min, ws = r.set_min;
        double dr = wr - cur_min, ds = ws - cur_min;
        if (dr <= 0) dr += 1440.0;
        if (ds <= 0) ds += 1440.0;
        int day = (ws > wr) ? (cur_min >= wr && cur_min < ws)
                            : (cur_min >= wr || cur_min < ws);
        if (day) { ne = 0; tne = (int)(ds + 0.5); }
        else     { ne = 1; tne = (int)(dr + 0.5); }
        if (tne > 1439) tne = 1439;
    }

    long syn;
    if (s_syncing || s_sync_must) syn = -1;
    else if (s_last_sync == 0) syn = -2;
    else {
        syn = (long)(now - s_last_sync);
        if (syn < 0) syn = 0;  /* 时钟倒拨(如PC注入旧时间)时视为刚同步 */
    }

    snprintf(hb->t, sizeof hb->t, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(hb->d, sizeof hb->d, "%02d-%02d", tm->tm_mon + 1, tm->tm_mday);
    snprintf(hb->s, sizeof hb->s, "%02d:%02d", sm / 60, sm % 60);
    if (polar)
        snprintf(hb->r, sizeof hb->r, "--");
    else
        snprintf(hb->r, sizeof hb->r, "%02d:%02d",
                 (int)(r.rise_min + 0.5) % 1440 / 60, (int)(r.rise_min + 0.5) % 60);
    if (polar)
        snprintf(hb->st, sizeof hb->st, "--");
    else
        snprintf(hb->st, sizeof hb->st, "%02d:%02d",
                 (int)(r.set_min + 0.5) % 1440 / 60, (int)(r.set_min + 0.5) % 60);
    hb->ne = ne; hb->tne = tne; hb->dp = dp; hb->ev = ev; hb->p = polar;
    hb->la = cfg.lat; hb->lo = cfg.lon; hb->syn = syn;
    return 0;
}

/* ---- 初始化 ---- */
void core_init(void) {
    esp_err_t nr = nvs_flash_init();
    if (nr == ESP_ERR_NVS_NO_FREE_PAGES || nr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_out = xSemaphoreCreateMutex();
#if CONFIG_SOLAR_RTC
    if (ds3231_init() == 0) s_rtc_ok = 1;
#endif
    nvs_load_cfg();
    nvs_load_last_sync();

    if (rtc_trusted()) {
        g_hb = 1;
        ESP_LOGI(TAG, "RTC时间可信, 直接进入心跳模式(缓存坐标)");
    } else {
        ESP_LOGW(TAG, "RTC时间不可信, 必须同步直到GNSS定位");
        s_sync_must = 1;
    }
#if CONFIG_SOLAR_GNSS
    xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, NULL, 5, NULL, 1);
#endif
    ESP_LOGI(TAG, "SolarCore ready, GPS: %.4f %.4f UTC%+g",
             g_cfg.lat, g_cfg.lon, g_cfg.tz);
}
