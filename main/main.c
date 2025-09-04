// main.c (ESP-IDF)
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"

#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASS      "YOUR_PASSWORD"
static const char *TAG = "clock";

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void wait_for_ip(void) {
    // simple wait loop; for production use event handlers
    int retries = 50;
    while (retries--) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void init_sntp(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    wait_for_ip();
    init_sntp();

    // Set timezone to Asia/Kolkata (IST). POSIX sign is reversed: UTC+5:30 => -5:30
    setenv("TZ", "IST-5:30", 1);
    tzset();

    // Wait until time is set
    time_t now = 0;
    struct tm timeinfo = {0};
    int wait = 0;
    while (timeinfo.tm_year < (2016 - 1900) && wait < 30) { // wait up to ~30s
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
        wait++;
    }

    ESP_LOGI(TAG, "Time sync %s", (timeinfo.tm_year >= (2016-1900)) ? "OK" : "FAILED (using epoch 0)");

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
        // Goes to UART0 (USB serial) by default
        printf("%s\r\n", buf);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
