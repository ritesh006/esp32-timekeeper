// main.c — ESP-IDF (v5.x)
// Presence-based attendance timer using ESP32 SoftAP + TM1637 (HH:MM).
// DIO = GPIO16, CLK = GPIO17 (see tm1637_init call).
// Build: add tm1637.c/h to your component (see CMakeLists snippet below).

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "tm1637.h"

static const char *TAG = "attendance";

// ====== CONFIG ======
#define AP_SSID           "OFFICE_ATTENDANCE"
#define AP_PASS           "12345678"
#define AP_CHANNEL        6
#define AP_MAX_CONN       2
#define TM_BRIGHTNESS     7              // 0..7
#define TM_DIO_PIN        GPIO_NUM_16
#define TM_CLK_PIN        GPIO_NUM_17

// Target: 9h15m = 33300 seconds
#define TARGET_SECONDS    (9*3600 + 15*60)

// OPTIONAL allow-list: set to your phone's MAC to lock to one device.
// Leave as all zeros to accept ANY device as presence trigger.
static uint8_t ALLOW_MAC[6] = {0,0,0,0,0,0};

// ====== STATE ======
static volatile bool     s_session_active   = false;
static volatile int64_t  s_session_start_us = 0;
static volatile uint32_t s_session_accum_s  = 0;
// If no allow MAC configured, we use station count to gate start/stop
static volatile int      s_sta_count        = 0;

// ====== HELPERS ======
static inline void session_start(void) {
    if (!s_session_active) {
        s_session_active   = true;
        s_session_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "SESSION START");
    }
}

static inline void session_stop(void) {
    if (s_session_active) {
        int64_t delta_s = (esp_timer_get_time() - s_session_start_us) / 1000000;
        if (delta_s > 0) s_session_accum_s += (uint32_t)delta_s;
        s_session_active = false;
        ESP_LOGI(TAG, "SESSION STOP, accum=%us", (unsigned)s_session_accum_s);
    }
}

static bool mac_allowed(const uint8_t mac[6]) {
    static const uint8_t ZERO[6] = {0};
    if (memcmp(ALLOW_MAC, ZERO, 6) == 0) return true;         // no filter
    return memcmp(ALLOW_MAC, mac, 6) == 0;                    // filter set
}

static void log_mac(const char *prefix, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ====== WIFI AP & EVENTS ======
static void wifi_ap_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        log_mac("STA CONNECTED:", e->mac);
        if (!mac_allowed(e->mac)) return;

        if (memcmp(ALLOW_MAC, (uint8_t[6]){0}, 6) == 0) {
            // accept any device: count stations
            s_sta_count++;
            if (s_sta_count == 1) session_start();
        } else {
            // allow-list mode: start only for the allowed MAC
            session_start();
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        log_mac("STA DISCONNECTED:", e->mac);
        if (!mac_allowed(e->mac)) return;

        if (memcmp(ALLOW_MAC, (uint8_t[6]){0}, 6) == 0) {
            if (s_sta_count > 0) s_sta_count--;
            if (s_sta_count == 0) session_stop();
        } else {
            session_stop();
        }
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {0};
    strcpy((char*)ap.ap.ssid, AP_SSID);
    strcpy((char*)ap.ap.password, AP_PASS);
    ap.ap.ssid_len       = 0;
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode       = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,   &wifi_ap_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_ap_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP ready. SSID:%s PASS:%s CH:%d", AP_SSID, AP_PASS, AP_CHANNEL);
}

// ====== APP ======
void app_main(void)
{
    // Keep UART single-line updates immediate
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_softap();

    // Optional: quiet logs to keep UART line clean after boot
    // esp_log_level_set("*", ESP_LOG_WARN);

    // Init 4-digit TM1637
    tm1637_init(TM_DIO_PIN, TM_CLK_PIN, TM_BRIGHTNESS);

    // Main loop: update display + single-line UART every second
    while (1) {
        // Compute elapsed seconds
        uint32_t elapsed_s = s_session_accum_s;
        if (s_session_active) {
            int64_t now_us = esp_timer_get_time();
            elapsed_s += (uint32_t)((now_us - s_session_start_us) / 1000000);
        }

        uint32_t hh = elapsed_s / 3600;
        uint32_t mm = (elapsed_s % 3600) / 60;
        if (hh > 99) hh = 99; // 2-digit cap

        // TM1637: show HH:MM; blink colon while running
        bool colon = s_session_active ? ((elapsed_s % 2) == 0) : false;
        tm1637_show_hhmm((uint8_t)hh, (uint8_t)mm, colon);

        // UART single-line status
        char line[80];
        snprintf(line, sizeof(line), "%02u:%02u elapsed%s%s",
                 (unsigned)hh, (unsigned)mm,
                 s_session_active ? " (RUN)" : " (STOP)",
                 (elapsed_s >= TARGET_SECONDS) ? "  ✅ 9:15 reached" : "");

        // \r = return to start; ESC[K clears to end of line
        printf("\r\x1b[K%s", line);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
