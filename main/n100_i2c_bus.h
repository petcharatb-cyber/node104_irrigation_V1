#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t n100_i2c_bus_init(int sda_gpio, int scl_gpio);
esp_err_t n100_i2c_bus_add_device(uint16_t address, uint32_t speed_hz,
                                  i2c_master_dev_handle_t *device);
