#pragma once

#include "esp_err.h"

typedef struct {
    int raw_value;
    int moisture_percent;
} soil_moisture_reading_t;

esp_err_t soil_moisture_init(void);
esp_err_t soil_moisture_read(soil_moisture_reading_t *reading);
