#pragma once
#include <time.h>
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t port;
    gpio_num_t sda;
    gpio_num_t scl;
    uint32_t   clk_hz;   // e.g., 100000 or 400000
} ds3231_config_t;

esp_err_t ds3231_init(const ds3231_config_t *cfg);
esp_err_t ds3231_get_time(struct tm *out);         // RTC → tm (local time)
esp_err_t ds3231_set_time(const struct tm *in);    // tm (local time) → RTC

#ifdef __cplusplus
}
#endif
