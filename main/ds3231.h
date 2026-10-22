#pragma once

#include <time.h>

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t ds3231_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t ds3231_get_time(struct tm *time_info);
esp_err_t ds3231_set_time(const struct tm *time_info);
esp_err_t ds3231_clear_alarm_flags(void);
esp_err_t ds3231_set_alarm1(const struct tm *alarm_time);
