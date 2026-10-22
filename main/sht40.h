#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t raw_temperature;
    uint16_t raw_humidity;
    float temperature_c;
    float humidity_percent;
} sht40_reading_t;

esp_err_t sht40_init(void);
esp_err_t sht40_read(sht40_reading_t *reading);
