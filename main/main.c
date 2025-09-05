// main.c — ESP-IDF v5.3.x
// Wi-Fi STA + SNTP (immediate apply) → UART single-line time + TM1637 HH:MM

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "esp_sntp.h"          // use ONLY this (do NOT include lwip/apps/sntp.h)
#include "tm1637.h"            // your TM1637 driver

// ====== Configure these ======
#define WIFI_SSID "Airtel_123"
#define WIFI_PASS "Ritesh@123"

// TM1637 pins/brightness
#define TM_DIO_PIN     GPIO_NUM_16
#define TM_CLK_PIN     GPIO_NUM_17
#define TM_BRIGHTNESS  7   // 0..7

static const char *TAG = "clock";

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

// simple polling until associated (for production, register IP_EVENT_STA_GOT_IP)
static void wait_for_ip_simple(void) {
    for (int i = 0; i < 100; ++i) {           // ~20s max
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------- SNTP (IST, immediate set) ----------
static void on_time_sync(struct timeval *tv) {
    ESP_LOGI("sntp", "Time synchronized");
}

static void sntp_init_ist(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_setservername(2, "time.google.com");

    // Apply time immediately (no smoothing)
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    // Use NTP server from DHCP if router provides one (IDF 5.3: this is the available API)
    sntp_servermode_dhcp(1);   // <-- changed back to this

    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    // IST timezone: POSIX sign is inverted for TZ (+5:30 => "IST-5:30")
    setenv("TZ", "IST-5:30", 1);
    tzset();
}

void app_main(void) {
    // Make printf unbuffered so our single-line UI is responsive
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    wait_for_ip_simple();
    sntp_init_ist();

    // Wait until SNTP reports completion (or 30s timeout)
    bool synced = false;
    for (int i = 0; i < 30; ++i) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) { synced = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Also check the calendar year looks sane
    time_t now = 0;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    if (!synced || ti.tm_year < (2016 - 1900)) {
        ESP_LOGW(TAG, "SNTP not ready yet; holding display until sync.");
    }

    // Init TM1637 display (HH:MM)
    tm1637_init(TM_DIO_PIN, TM_CLK_PIN, TM_BRIGHTNESS);

    // Optional: quiet logs so they don't break the single-line UART clock
    // esp_log_level_set("*", ESP_LOG_WARN);

    while (1) {
        time(&now);
        localtime_r(&now, &ti);

        // If not yet synced, keep waiting (don't show wrong time)
        if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && ti.tm_year < (2016 - 1900)) {
            tm1637_show_hhmm(0, 0, false);
            printf("\r\x1b[KWaiting for NTP...");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // --- SYNCED: display the real time ---
        int h12 = ti.tm_hour % 12; if (h12 == 0) h12 = 12;
        bool colon = (ti.tm_sec % 2) == 0;

        // TM1637: 12-hour HH:MM with blinking colon
        tm1637_show_hhmm((uint8_t)h12, (uint8_t)ti.tm_min, colon);

        // UART: single-line 12-hour time with seconds + date + IST tag
        char buf[64];
        strftime(buf, sizeof(buf), "%I:%M:%S %p %d-%m-%Y IST", &ti);
        printf("\r\x1b[K%s", buf);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
