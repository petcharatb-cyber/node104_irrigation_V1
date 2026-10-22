#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_manager_init(const char *ssid, const char *password);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_wait_for_connection(TickType_t timeout_ticks);
