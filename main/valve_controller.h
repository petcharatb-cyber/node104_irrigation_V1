#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t valve_controller_init(void);
esp_err_t valve_controller_initialize_firebase_commands(const cJSON *document);
esp_err_t valve_controller_apply_firebase_commands(const cJSON *document);
esp_err_t valve_controller_apply_firebase_config(const cJSON *document);
esp_err_t valve_controller_add_history_state(cJSON *document);
void valve_controller_request_all_closed(bool requested);
bool valve_controller_all_zones_closed(void);
