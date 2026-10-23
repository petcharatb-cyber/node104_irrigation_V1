#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float liters_per_minute;
    float liters_in_interval;
    float cumulative_liters;
    float last_session_liters;
    uint32_t pulses_in_interval;
    bool leak_detected;
} flow_reading_t;

typedef struct {
    flow_reading_t zone_a;
    flow_reading_t zone_b;
    flow_reading_t zone_c;
    flow_reading_t main_pump;
    bool water_leak_detected;
} flow_monitor_reading_t;

typedef enum {
    FLOW_ZONE_A,
    FLOW_ZONE_B,
    FLOW_ZONE_C,
} flow_zone_t;

esp_err_t flow_monitor_init(void);
esp_err_t flow_monitor_read(flow_monitor_reading_t *reading);
void flow_monitor_set_zone_valves_closed(flow_zone_t zone, bool valves_closed);
void flow_monitor_set_all_zones_closed(bool all_zones_closed);
void flow_monitor_set_zone_active(flow_zone_t zone, bool active);
void flow_monitor_set_main_pump_active(bool active);
esp_err_t flow_monitor_reset_totals(void);
