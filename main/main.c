// main.c — ESP-IDF v5.3.x
// DS3231-first clock: seed from RTC/NVS/build-time; SNTP corrects RTC & is cached to NVS
// UART: 12h single-line; TM1637: HH:MM with blinking colon (IST)

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "esp_sntp.h"          // IMPORTANT: use ONLY esp_sntp.h
#include "driver/i2c.h"

#include "tm1637.h"
#include "ds3231.h"

// ====== CONFIGURE ======
#define WIFI_SSID "WIFI_SSID"
#define WIFI_PASS "WIFI_PASS"

// TM1637 pins/brightness
#define TM_DIO_PIN     GPIO_NUM_16
#define TM_CLK_PIN     GPIO_NUM_17
#define TM_BRIGHTNESS  7   // 0..7

// I2C (DS3231) — 3.3V ok
#define I2C_PORT       I2C_NUM_0
#define I2C_SDA        GPIO_NUM_21
#define I2C_SCL        GPIO_NUM_22
#define I2C_FREQ_HZ    400000

// NVS keys
#define NVS_NS_TIME    "time"
#define NVS_KEY_EPOCH  "last_epoch"

// validity gate for RTC contents
#define MIN_VALID_YEAR 2023

static const char *TAG = "clock";
static volatile bool s_ntp_synced = false;
static bool s_rtc_ok = false;

// ---------- small helpers ----------
static void i2c_scan(i2c_port_t port) {
    printf("\n[I2C] scanning...\n");
    for (int addr = 0x03; addr <= 0x77; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) printf("  FOUND: 0x%02X\n", addr);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("[I2C] scan done.\n\n");
}

static inline time_t tm_to_time_t_local(struct tm *t_local) {
    // interprets struct tm as LOCAL time (TZ must be set before)
    return mktime(t_local);
}

static void set_system_time_from_tm(const struct tm *t_local) {
    struct tm tmp = *t_local;
    time_t tt = tm_to_time_t_local(&tmp);
    struct timeval tv = { .tv_sec = tt, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static bool nvs_load_epoch(time_t *out_epoch) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_TIME, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    int64_t v = 0;
    err = nvs_get_i64(h, NVS_KEY_EPOCH, &v);
    nvs_close(h);
    if (err == ESP_OK) { *out_epoch = (time_t)v; return true; }
    return false;
}

static void nvs_save_epoch(time_t epoch) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_TIME, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i64(h, NVS_KEY_EPOCH, (int64_t)epoch);
    (void)nvs_commit(h);
    nvs_close(h);
}

// parse __DATE__/__TIME__ (build machine local) into tm (local tz already set)
static void build_time_to_tm(struct tm *out) {
    memset(out, 0, sizeof(*out));
    char mon[4]; int d=1, y=2025, h=0, m=0, s=0;
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
    const char *p = strstr(months, mon);
    int mon_idx = p ? (int)((p - months) / 3) : 0;

    out->tm_year = y - 1900;
    out->tm_mon  = mon_idx;
    out->tm_mday = d;
    out->tm_hour = h;
    out->tm_min  = m;
    out->tm_sec  = s;
}

// ---------- Wi-Fi (STA) ----------
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void wait_for_ip_simple(void) {
    for (int i = 0; i < 100; ++i) { // ~20s
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------- SNTP ----------
static void on_time_sync(struct timeval *tv) {
    ESP_LOGI("sntp", "Time synchronized");
    s_ntp_synced = true; // main loop handles RTC+NVS update
}

static void sntp_init_ist(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_setservername(2, "time.google.com");

    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    // ok if it warns deprecated; harmless
    sntp_servermode_dhcp(1);

    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    setenv("TZ", "IST-5:30", 1); // IST = UTC+5:30 (POSIX sign inverted)
    tzset();
}

void app_main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_ERROR_CHECK(nvs_flash_init());

    // Set TZ early so RTC/NVS conversions are interpreted in IST
    setenv("TZ", "IST-5:30", 1);
    tzset();

    // --- Init DS3231 (installs I²C driver inside) ---
    ds3231_config_t rtc = { .port=I2C_PORT, .sda=I2C_SDA, .scl=I2C_SCL, .clk_hz=I2C_FREQ_HZ };
    if (ds3231_init(&rtc) == ESP_OK) {
        s_rtc_ok = true;
        i2c_scan(I2C_PORT); // expect 0x68; modules often also show 0x57/0x5F (EEPROM)
        struct tm tcur = {0};
        if (ds3231_get_time(&tcur) == ESP_OK) {
            printf("RTC @ boot: %04d-%02d-%02d %02d:%02d:%02d\n",
                   tcur.tm_year+1900, tcur.tm_mon+1, tcur.tm_mday,
                   tcur.tm_hour, tcur.tm_min, tcur.tm_sec);

            // If RTC looks uninitialized, seed from NVS last epoch or build time
            if ((tcur.tm_year + 1900) < MIN_VALID_YEAR) {
                time_t cached;
                if (nvs_load_epoch(&cached)) {
                    struct tm nvstm; localtime_r(&cached, &nvstm);
                    if (ds3231_set_time(&nvstm) == ESP_OK) {
                        printf("RTC seeded from NVS: %04d-%02d-%02d %02d:%02d:%02d\n",
                               nvstm.tm_year+1900, nvstm.tm_mon+1, nvstm.tm_mday,
                               nvstm.tm_hour, nvstm.tm_min, nvstm.tm_sec);
                        tcur = nvstm;
                    }
                } else {
                    struct tm bt; build_time_to_tm(&bt);
                    if (ds3231_set_time(&bt) == ESP_OK) {
                        printf("RTC seeded from build time: %04d-%02d-%02d %02d:%02d:%02d\n",
                               bt.tm_year+1900, bt.tm_mon+1, bt.tm_mday,
                               bt.tm_hour, bt.tm_min, bt.tm_sec);
                        tcur = bt;
                    }
                }
            }

            // set system clock from RTC immediately
            set_system_time_from_tm(&tcur);
        } else {
            printf("RTC read failed @ boot (check wiring)\n");
        }
    } else {
        ESP_LOGW(TAG, "RTC init failed");
    }

    // --- Wi-Fi + SNTP (internet time; may arrive later) ---
    wifi_init_sta();
    wait_for_ip_simple();
    sntp_init_ist();

    // --- TM1637 display init ---
    tm1637_init(TM_DIO_PIN, TM_CLK_PIN, TM_BRIGHTNESS);

    // Optional: quiet logs so they don't trash the one-line UART clock
    // esp_log_level_set("*", ESP_LOG_WARN);

    while (1) {
        // If NTP just synced, push system time → DS3231 and cache to NVS
        if (s_ntp_synced && s_rtc_ok) {
            time_t now; struct tm ti;
            time(&now); localtime_r(&now, &ti); // system time set by SNTP
            if (ds3231_set_time(&ti) == ESP_OK) {
                struct tm chk = {0};
                if (ds3231_get_time(&chk) == ESP_OK) {
                    printf("\nRTC updated from NTP: %04d-%02d-%02d %02d:%02d:%02d\n",
                           chk.tm_year+1900, chk.tm_mon+1, chk.tm_mday,
                           chk.tm_hour, chk.tm_min, chk.tm_sec);
                    // cache epoch for next cold boot
                    nvs_save_epoch(now);
                }
            } else {
                ESP_LOGW(TAG, "RTC update from NTP failed");
            }
            s_ntp_synced = false; // handle once per sync event
        }

        // ALWAYS fetch time from DS3231 for display
        struct tm t = {0};
        if (s_rtc_ok && ds3231_get_time(&t) == ESP_OK) {
            int h12 = t.tm_hour % 12; if (h12 == 0) h12 = 12;
            bool colon = (t.tm_sec % 2) == 0;

            tm1637_show_hhmm((uint8_t)h12, (uint8_t)t.tm_min, colon);

            char buf[64];
            strftime(buf, sizeof(buf), "%I:%M:%S %p %d-%m-%Y IST", &t);
            printf("\r\x1b[K%s", buf);
        } else {
            tm1637_show_hhmm(0, 0, false);
            printf("\r\x1b[KRTC read failed...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
