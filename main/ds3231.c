#include "ds3231.h"
#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"

#define DS3231_ADDR         0x68
#define DS3231_REG_SECONDS  0x00
#define DS3231_REG_MINUTES  0x01
#define DS3231_REG_HOURS    0x02
#define DS3231_REG_DAY      0x03
#define DS3231_REG_DATE     0x04
#define DS3231_REG_MONTH    0x05
#define DS3231_REG_YEAR     0x06

static const char *TAG = "ds3231";
static i2c_port_t s_port;

static inline uint8_t bcd2bin(uint8_t v){ return (v & 0x0F) + 10 * ((v >> 4) & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v){ return (uint8_t)((v / 10) << 4) | (v % 10); }

esp_err_t ds3231_init(const ds3231_config_t *cfg)
{
    s_port = cfg->port;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda,
        .scl_io_num = cfg->scl,
        .sda_pullup_en = GPIO_PULLUP_DISABLE, // DS3231 modules have pull-ups; 3.3V ok
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = cfg->clk_hz,
        .clk_flags = 0
    };
    ESP_ERROR_CHECK(i2c_param_config(s_port, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(s_port, conf.mode, 0, 0, 0));
    return ESP_OK;
}

esp_err_t ds3231_get_time(struct tm *out)
{
    uint8_t reg = DS3231_REG_SECONDS;
    uint8_t b[7] = {0};
    esp_err_t err = i2c_master_write_read_device(s_port, DS3231_ADDR, &reg, 1, b, 7, 1000/portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read time failed: %s", esp_err_to_name(err));
        return err;
    }

    int sec  = bcd2bin(b[0] & 0x7F);
    int min  = bcd2bin(b[1] & 0x7F);

    uint8_t hr_reg = b[2];
    int hour;
    if (hr_reg & 0x40) { // 12-hour mode
        hour = bcd2bin(hr_reg & 0x1F);
        if (hour == 12) hour = 0;
        if (hr_reg & 0x20) hour += 12; // PM
    } else {
        hour = bcd2bin(hr_reg & 0x3F); // 24h
    }

    int mday = bcd2bin(b[4] & 0x3F);
    int mon  = bcd2bin(b[5] & 0x1F) - 1;
    int year = bcd2bin(b[6]); // 00..99 → 2000..2099
    int y1900 = (2000 + year) - 1900;

    memset(out, 0, sizeof(*out));
    out->tm_sec  = sec;
    out->tm_min  = min;
    out->tm_hour = hour;
    out->tm_mday = mday;
    out->tm_mon  = mon;
    out->tm_year = y1900;
    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *in)
{
    uint8_t w[8];
    w[0] = DS3231_REG_SECONDS;
    w[1] = bin2bcd(in->tm_sec)  & 0x7F;
    w[2] = bin2bcd(in->tm_min)  & 0x7F;
    w[3] = bin2bcd(in->tm_hour) & 0x3F; // 24h mode

    int wday = in->tm_wday;
    if (wday == 0) wday = 7;            // DS3231: 1..7
    w[4] = bin2bcd((uint8_t)wday) & 0x07;

    w[5] = bin2bcd(in->tm_mday) & 0x3F;
    w[6] = bin2bcd(in->tm_mon + 1) & 0x1F;

    int y2000 = (in->tm_year + 1900) - 2000;
    if (y2000 < 0) {
        y2000 = 0;
    } else if (y2000 > 99) {
        y2000 = 99;
    }
    w[7] = bin2bcd((uint8_t)y2000);

    esp_err_t err = i2c_master_write_to_device(s_port, DS3231_ADDR, w, sizeof(w), 1000/portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write time failed: %s", esp_err_to_name(err));
    }
    return err;
}
