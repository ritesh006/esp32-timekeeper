// tm1637.h — minimal TM1637 driver for ESP-IDF
#pragma once
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize display on given pins; brightness 0..7 (also turns display ON)
void tm1637_init(gpio_num_t dio, gpio_num_t clk, int brightness_0_to_7);

// Show HH:MM (12/24-agnostic, you pass the numbers). Colon on/off.
void tm1637_show_hhmm(uint8_t hh, uint8_t mm, bool colon);

#ifdef __cplusplus
}
#endif
