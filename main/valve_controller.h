#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t valve_controller_init(void);
esp_err_t valve_controller_initialize_firebase_commands(void);
esp_err_t valve_controller_apply_firebase_commands(const cJSON *document);
esp_err_t valve_controller_apply_firebase_config(const cJSON *document);
