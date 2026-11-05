#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "ds3231.h"
#include "deep_sleep.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "firebase_client.h"
#include "flow_monitor.h"
#include "nvs_flash.h"
#include "soil_moisture.h"
#include "sht40.h"
#include "valve_controller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"
#include "wifi_manager.h"

static const char *TAG = "app";
#define TELEMETRY_UPLOAD_PERIOD_MS 5000
#define GPIO19_MONITOR_PERIOD_MS 10000
#define MAX_DEEP_SLEEP_TIMERS 8

typedef struct {
    uint16_t sleep_minute;
    uint16_t wake_minute;
} daily_sleep_timer_t;

static portMUX_TYPE s_statistics_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_statistics_enabled = true;
static uint32_t s_statistics_interval_minutes = 5;
static int64_t s_statistics_last_sample_us;
static portMUX_TYPE s_power_schedule_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_deep_sleep_enabled;
static uint8_t s_deep_sleep_timer_count;
static daily_sleep_timer_t s_deep_sleep_timers[MAX_DEEP_SLEEP_TIMERS];
static bool s_gpio19_waiting_for_firebase;
static bool s_gpio19_monitor_enabled;

static void gpio19_monitor_task(void *argument)
{
    (void)argument;
    const TickType_t monitor_period = pdMS_TO_TICKS(GPIO19_MONITOR_PERIOD_MS);
    TickType_t last_wake_time = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake_time, monitor_period);
        const int level = gpio_get_level(GPIO_NUM_19);
        ESP_LOGI(TAG, "GPIO19 status: %s (level %s, active-low)",
                 level == 0 ? "ON" : "OFF",
                 level == 0 ? "LOW" : "HIGH");
    }
}

static void prepare_gpio19_for_deep_sleep_wake(uint32_t wakeup_causes)
{
    if ((wakeup_causes & (1U << ESP_SLEEP_WAKEUP_EXT1)) == 0) {
        return;
    }

    esp_err_t err = gpio_set_level(GPIO_NUM_19, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set GPIO19 OFF before Firebase connection: %s",
                 esp_err_to_name(err));
        return;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_19,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure GPIO19 output: %s", esp_err_to_name(err));
        return;
    }

    s_gpio19_waiting_for_firebase = true;
    s_gpio19_monitor_enabled = true;
}

static bool parse_schedule_minute(const cJSON *value, uint16_t *minute_out)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
        strlen(value->valuestring) != 5 || value->valuestring[2] != ':') {
        return false;
    }
    const char *clock = value->valuestring;
    if (clock[0] < '0' || clock[0] > '2' || clock[1] < '0' ||
        clock[1] > '9' || clock[3] < '0' || clock[3] > '5' ||
        clock[4] < '0' || clock[4] > '9') {
        return false;
    }
    const uint16_t hour =
        (uint16_t)((clock[0] - '0') * 10 + clock[1] - '0');
    const uint16_t minute =
        (uint16_t)((clock[3] - '0') * 10 + clock[4] - '0');
    if (hour > 23) {
        return false;
    }
    *minute_out = (uint16_t)(hour * 60 + minute);
    return true;
}

static esp_err_t apply_power_schedule_config(const cJSON *document)
{
    if (!cJSON_IsObject(document)) {
        return ESP_OK;
    }

    bool enabled;
    uint8_t timer_count;
    daily_sleep_timer_t timers[MAX_DEEP_SLEEP_TIMERS] = {0};
    portENTER_CRITICAL(&s_power_schedule_mux);
    enabled = s_deep_sleep_enabled;
    timer_count = s_deep_sleep_timer_count;
    memcpy(timers, s_deep_sleep_timers, sizeof(timers));
    portEXIT_CRITICAL(&s_power_schedule_mux);

    const cJSON *mode =
        cJSON_GetObjectItemCaseSensitive(document, "system_power_mode");
    if (mode != NULL) {
        if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
            ESP_LOGE(TAG, "Firebase system_power_mode must be a string");
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(mode->valuestring, "always_on") == 0) {
            enabled = false;
        } else if (strcmp(mode->valuestring, "deep_sleep") == 0) {
            enabled = true;
        } else {
            ESP_LOGE(TAG, "Unsupported Firebase system_power_mode");
            return ESP_ERR_INVALID_ARG;
        }
    }
    const cJSON *schedule =
        cJSON_GetObjectItemCaseSensitive(document, "deep_sleep_timers");
    if (schedule != NULL) {
        if (!cJSON_IsArray(schedule) ||
            cJSON_GetArraySize(schedule) > MAX_DEEP_SLEEP_TIMERS) {
            ESP_LOGE(TAG, "Firebase deep_sleep_timers must contain at most %d entries",
                     MAX_DEEP_SLEEP_TIMERS);
            return ESP_ERR_INVALID_ARG;
        }
        timer_count = (uint8_t)cJSON_GetArraySize(schedule);
        memset(timers, 0, sizeof(timers));
        const cJSON *timer = NULL;
        size_t index = 0;
        cJSON_ArrayForEach(timer, schedule) {
            const cJSON *sleep_at =
                cJSON_GetObjectItemCaseSensitive(timer, "sleep_at");
            const cJSON *wake_at =
                cJSON_GetObjectItemCaseSensitive(timer, "wake_at");
            if (!cJSON_IsObject(timer) ||
                !parse_schedule_minute(sleep_at, &timers[index].sleep_minute) ||
                !parse_schedule_minute(wake_at, &timers[index].wake_minute) ||
                timers[index].sleep_minute == timers[index].wake_minute) {
                ESP_LOGE(TAG, "Invalid Firebase deep sleep interval");
                return ESP_ERR_INVALID_ARG;
            }
            ++index;
        }
    }

    portENTER_CRITICAL(&s_power_schedule_mux);
    s_deep_sleep_enabled = enabled;
    s_deep_sleep_timer_count = timer_count;
    memcpy(s_deep_sleep_timers, timers, sizeof(timers));
    portEXIT_CRITICAL(&s_power_schedule_mux);
    return ESP_OK;
}

static bool sleep_schedule_active(const struct tm *now,
                                 const daily_sleep_timer_t *timers,
                                 uint8_t timer_count, uint16_t *wake_minute)
{
    const uint16_t minute = (uint16_t)(now->tm_hour * 60 + now->tm_min);
    for (size_t index = 0; index < timer_count; ++index) {
        const daily_sleep_timer_t *timer = &timers[index];
        const bool active = timer->sleep_minute < timer->wake_minute
                                ? minute >= timer->sleep_minute &&
                                      minute < timer->wake_minute
                                : minute >= timer->sleep_minute ||
                                      minute < timer->wake_minute;
        if (active) {
            *wake_minute = timer->wake_minute;
            return true;
        }
    }
    return false;
}

static void power_schedule_task(void *argument)
{
    (void)argument;
    for (;;) {
        bool enabled;
        uint8_t timer_count;
        daily_sleep_timer_t timers[MAX_DEEP_SLEEP_TIMERS];
        portENTER_CRITICAL(&s_power_schedule_mux);
        enabled = s_deep_sleep_enabled;
        timer_count = s_deep_sleep_timer_count;
        memcpy(timers, s_deep_sleep_timers, sizeof(timers));
        portEXIT_CRITICAL(&s_power_schedule_mux);

        struct tm now = {0};
        uint16_t wake_minute = 0;
        const esp_err_t rtc_err =
            enabled && timer_count > 0 ? ds3231_get_time(&now) : ESP_OK;
        const bool window_active =
            rtc_err == ESP_OK && enabled && timer_count > 0 &&
            sleep_schedule_active(&now, timers, timer_count, &wake_minute);
        if (enabled && timer_count > 0 && rtc_err != ESP_OK) {
            valve_controller_request_all_closed(true);
            ESP_LOGE(TAG, "Could not read DS3231 for power schedule; keeping zones closed: %s",
                     esp_err_to_name(rtc_err));
        } else if (window_active) {
            valve_controller_request_all_closed(true);
            ESP_LOGI(TAG, "Deep sleep window active; closing all zones before sleep");
            while (!valve_controller_all_zones_closed()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                portENTER_CRITICAL(&s_power_schedule_mux);
                enabled = s_deep_sleep_enabled;
                portEXIT_CRITICAL(&s_power_schedule_mux);
                if (!enabled) {
                    break;
                }
            }
            if (enabled && valve_controller_all_zones_closed()) {
                portENTER_CRITICAL(&s_power_schedule_mux);
                enabled = s_deep_sleep_enabled;
                timer_count = s_deep_sleep_timer_count;
                memcpy(timers, s_deep_sleep_timers, sizeof(timers));
                portEXIT_CRITICAL(&s_power_schedule_mux);
                const esp_err_t rtc_err = ds3231_get_time(&now);
                const bool still_sleep_window =
                    rtc_err == ESP_OK && timer_count > 0 &&
                    sleep_schedule_active(&now, timers, timer_count, &wake_minute);
                if (!enabled || timer_count == 0) {
                    valve_controller_request_all_closed(false);
                } else if (rtc_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not recheck DS3231 time before sleep: %s",
                             esp_err_to_name(rtc_err));
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else if (!still_sleep_window) {
                    valve_controller_request_all_closed(false);
                } else {
                    if ((uint16_t)(now.tm_hour * 60 + now.tm_min) >= wake_minute) {
                        ++now.tm_mday;
                    }
                    now.tm_hour = wake_minute / 60;
                    now.tm_min = wake_minute % 60;
                    now.tm_sec = 0;
                    now.tm_isdst = -1;
                    const time_t wake_timestamp = mktime(&now);
                    if (wake_timestamp == (time_t)-1 ||
                        localtime_r(&wake_timestamp, &now) == NULL) {
                        ESP_LOGE(TAG, "Could not calculate next DS3231 wake time");
                    } else {
                        ESP_LOGI(TAG, "All valves closed; entering deep sleep until %04d-%02d-%02d %02d:%02d:%02d",
                                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                                 now.tm_hour, now.tm_min, now.tm_sec);
                        const esp_err_t err = deep_sleep_until(&now);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Could not enter scheduled deep sleep: %s",
                                     esp_err_to_name(err));
                            valve_controller_request_all_closed(false);
                        }
                    }
                }
            } else {
                valve_controller_request_all_closed(false);
            }
        } else {
            valve_controller_request_all_closed(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t apply_statistics_config(const cJSON *document)
{
    if (!cJSON_IsObject(document)) {
        return ESP_OK;
    }
    const cJSON *enabled =
        cJSON_GetObjectItemCaseSensitive(document, "statistics_enabled");
    const cJSON *interval =
        cJSON_GetObjectItemCaseSensitive(document, "statistics_interval_minutes");
    if (enabled != NULL && !cJSON_IsBool(enabled)) {
        ESP_LOGE(TAG, "Firebase statistics_enabled must be boolean");
        return ESP_ERR_INVALID_ARG;
    }
    if (interval != NULL &&
        (!cJSON_IsNumber(interval) || interval->valuedouble < 1 ||
         interval->valuedouble > 1440 ||
         interval->valuedouble != (double)(uint32_t)interval->valuedouble)) {
        ESP_LOGE(TAG, "Firebase statistics interval must be from 1 to 1440 minutes");
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_statistics_mux);
    if (interval != NULL) {
        s_statistics_interval_minutes = (uint32_t)interval->valuedouble;
    }
    if (enabled != NULL) {
        const bool was_enabled = s_statistics_enabled;
        s_statistics_enabled = cJSON_IsTrue(enabled);
        if (s_statistics_enabled && !was_enabled) {
            s_statistics_last_sample_us = esp_timer_get_time();
        }
    }
    portEXIT_CRITICAL(&s_statistics_mux);
    return ESP_OK;
}

static esp_err_t initialize_statistics_defaults(const cJSON *document)
{
    if (document != NULL && !cJSON_IsNull(document) && !cJSON_IsObject(document)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *defaults = cJSON_CreateObject();
    if (defaults == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const bool add_enabled =
        cJSON_IsNull(document) ||
        cJSON_GetObjectItemCaseSensitive(document, "statistics_enabled") == NULL;
    const bool add_interval =
        cJSON_IsNull(document) ||
        cJSON_GetObjectItemCaseSensitive(document,
                                          "statistics_interval_minutes") == NULL;
    if ((add_enabled &&
         cJSON_AddBoolToObject(defaults, "statistics_enabled", true) == NULL) ||
        (add_interval &&
         cJSON_AddNumberToObject(defaults, "statistics_interval_minutes", 5) == NULL)) {
        cJSON_Delete(defaults);
        return ESP_ERR_NO_MEM;
    }
    if (defaults->child == NULL) {
        cJSON_Delete(defaults);
        return ESP_OK;
    }

    const esp_err_t err = firebase_client_patch(FIREBASE_RESOURCE_CONFIG, defaults);
    cJSON_Delete(defaults);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize Firebase statistics defaults: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Initialized missing Firebase statistics settings");
    }
    return err;
}

static bool statistics_sample_due(int64_t now_us)
{
    bool due = false;
    portENTER_CRITICAL(&s_statistics_mux);
    if (s_statistics_enabled &&
        now_us - s_statistics_last_sample_us >=
            (int64_t)s_statistics_interval_minutes * 60 * 1000000) {
        due = true;
    }
    portEXIT_CRITICAL(&s_statistics_mux);
    return due;
}

static void statistics_sample_completed(int64_t now_us)
{
    portENTER_CRITICAL(&s_statistics_mux);
    s_statistics_last_sample_us = now_us;
    portEXIT_CRITICAL(&s_statistics_mux);
}

static void firebase_document_received(firebase_resource_t resource,
                                       const cJSON *document, void *context)
{
    (void)context;

    if (s_gpio19_waiting_for_firebase) {
        const esp_err_t err = gpio_set_level(GPIO_NUM_19, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not turn GPIO19 ON after Firebase connection: %s",
                     esp_err_to_name(err));
        } else {
            s_gpio19_waiting_for_firebase = false;
            ESP_LOGI(TAG, "GPIO19 turned ON after successful Firebase connection");
        }
    }

    const char *resource_name;
    switch (resource) {
    case FIREBASE_RESOURCE_COMMANDS:
        resource_name = "commands";
        break;
    case FIREBASE_RESOURCE_CONFIG:
        resource_name = "system config";
        break;
    case FIREBASE_RESOURCE_TELEMETRY:
        resource_name = "telemetry";
        break;
    case FIREBASE_RESOURCE_STATUS:
        resource_name = "device status";
        break;
    default:
        ESP_LOGE(TAG, "Received unknown Firebase resource %d", resource);
        return;
    }
    if (cJSON_IsObject(document) || cJSON_IsArray(document)) {
        ESP_LOGI(TAG, "Received Firebase %s document", resource_name);
        if ((resource == FIREBASE_RESOURCE_CONFIG ||
             resource == FIREBASE_RESOURCE_COMMANDS) &&
            cJSON_IsObject(document)) {
            if (resource == FIREBASE_RESOURCE_COMMANDS) {
                const esp_err_t defaults_err =
                    valve_controller_initialize_firebase_commands(document);
                if (defaults_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not complete default Firebase commands: %s",
                             esp_err_to_name(defaults_err));
                }
            } else {
                const esp_err_t defaults_err =
                    initialize_statistics_defaults(document);
                if (defaults_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not initialize missing statistics settings: %s",
                             esp_err_to_name(defaults_err));
                }
            }
            const esp_err_t err = resource == FIREBASE_RESOURCE_COMMANDS
                                      ? valve_controller_apply_firebase_commands(document)
                                      : valve_controller_apply_firebase_config(document);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not apply Firebase %s: %s",
                         resource_name,
                         esp_err_to_name(err));
            } else if (resource == FIREBASE_RESOURCE_CONFIG) {
                const esp_err_t statistics_err = apply_statistics_config(document);
                if (statistics_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not apply Firebase statistics settings: %s",
                             esp_err_to_name(statistics_err));
                }
                const esp_err_t power_err = apply_power_schedule_config(document);
                if (power_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not apply Firebase power schedule: %s",
                             esp_err_to_name(power_err));
                }
            }
        }
    } else if (cJSON_IsNull(document)) {
        if (resource == FIREBASE_RESOURCE_COMMANDS) {
            const esp_err_t err =
                valve_controller_initialize_firebase_commands(document);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not create default Firebase commands: %s",
                         esp_err_to_name(err));
            }
        } else if (resource == FIREBASE_RESOURCE_CONFIG) {
            const esp_err_t err = valve_controller_apply_firebase_config(document);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize Firebase valve config: %s",
                         esp_err_to_name(err));
            }
            const esp_err_t defaults_err =
                initialize_statistics_defaults(document);
            if (defaults_err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize Firebase statistics defaults: %s",
                                 esp_err_to_name(defaults_err));
            }
            const esp_err_t statistics_err = apply_statistics_config(document);
            if (statistics_err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize Firebase statistics settings: %s",
                         esp_err_to_name(statistics_err));
            }
            const esp_err_t power_err = apply_power_schedule_config(document);
            if (power_err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize Firebase power schedule: %s",
                         esp_err_to_name(power_err));
            }
        } else {
            ESP_LOGW(TAG, "Firebase %s document is null", resource_name);
        }
    } else {
        ESP_LOGW(TAG, "Firebase %s document has an unexpected JSON type", resource_name);
    }
}

static void rtc_log_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct tm rtc_time = {0};
        esp_err_t err = ds3231_get_time(&rtc_time);
        if (err == ESP_OK) {
            char timestamp[32];
            if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &rtc_time) > 0) {
                ESP_LOGI(TAG, "DS3231: %s", timestamp);
            } else {
                ESP_LOGE(TAG, "Could not format DS3231 date/time");
            }
        } else {
            ESP_LOGE(TAG, "Could not read DS3231: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void firebase_telemetry_upload_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t upload_period = pdMS_TO_TICKS(TELEMETRY_UPLOAD_PERIOD_MS);

    for (;;) {
        struct tm rtc_time = {0};
        char date[11];
        char time[9];
        soil_moisture_reading_t moisture_reading = {0};
        const esp_err_t moisture_err = soil_moisture_read(&moisture_reading);
        if (moisture_err == ESP_OK) {
            ESP_LOGI(TAG, "Soil moisture: raw=%d, %d%%",
                     moisture_reading.raw_value, moisture_reading.moisture_percent);
        }
        sht40_reading_t sht40_reading = {0};
        const esp_err_t sht40_err = sht40_read(&sht40_reading);
        if (sht40_err == ESP_OK) {
            ESP_LOGI(TAG, "SHT40: temperature=%.2f C, humidity=%.2f%% "
                          "(raw temperature=%u, raw humidity=%u)",
                     (double)sht40_reading.temperature_c,
                     (double)sht40_reading.humidity_percent,
                     sht40_reading.raw_temperature, sht40_reading.raw_humidity);
        }
        flow_monitor_reading_t flow_reading = {0};
        const esp_err_t flow_err = flow_monitor_read(&flow_reading);
        if (flow_err == ESP_OK) {
            ESP_LOGI(TAG, "Water flow: Zone A=%" PRIu32 " pulses/%.3f L/min, "
                          "Zone B=%" PRIu32 " pulses/%.3f L/min, "
                          "Zone C=%" PRIu32 " pulses/%.3f L/min, "
                          "Main pump=%" PRIu32 " pulses/%.3f L/min",
                     flow_reading.zone_a.pulses_in_interval,
                     (double)flow_reading.zone_a.liters_per_minute,
                     flow_reading.zone_b.pulses_in_interval,
                     (double)flow_reading.zone_b.liters_per_minute,
                     flow_reading.zone_c.pulses_in_interval,
                     (double)flow_reading.zone_c.liters_per_minute,
                     flow_reading.main_pump.pulses_in_interval,
                     (double)flow_reading.main_pump.liters_per_minute);
            ESP_LOGI(TAG, "Water leak: Zone A=%s, Zone B=%s, Zone C=%s, "
                          "Main pump=%s, any=%s",
                     flow_reading.zone_a.leak_detected ? "true" : "false",
                     flow_reading.zone_b.leak_detected ? "true" : "false",
                     flow_reading.zone_c.leak_detected ? "true" : "false",
                     flow_reading.main_pump.leak_detected ? "true" : "false",
                     flow_reading.water_leak_detected ? "true" : "false");
        }
        esp_err_t err = ds3231_get_time(&rtc_time);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read DS3231 for Firebase upload: %s",
                     esp_err_to_name(err));
        } else if (strftime(date, sizeof(date), "%Y-%m-%d", &rtc_time) == 0 ||
                   strftime(time, sizeof(time), "%H:%M:%S", &rtc_time) == 0) {
            ESP_LOGE(TAG, "Could not format DS3231 date/time for Firebase upload");
        } else {
            cJSON *document = cJSON_CreateObject();
            if (document == NULL) {
                ESP_LOGE(TAG, "Could not allocate Firebase telemetry document");
            } else {
                cJSON *server_timestamp = cJSON_CreateObject();
                if (server_timestamp == NULL ||
                    cJSON_AddStringToObject(server_timestamp, ".sv", "timestamp") == NULL ||
                    !cJSON_AddItemToObject(document, "server_timestamp_ms",
                                           server_timestamp)) {
                    cJSON_Delete(server_timestamp);
                    ESP_LOGE(TAG, "Could not create Firebase server timestamp");
                    cJSON_Delete(document);
                    document = NULL;
                }
            }
            if (document != NULL &&
                       (cJSON_AddStringToObject(document, "rtc_date", date) == NULL ||
                       cJSON_AddStringToObject(document, "rtc_time", time) == NULL ||
                       (moisture_err == ESP_OK &&
                        (cJSON_AddNumberToObject(document, "soil_moisture_raw",
                                                 moisture_reading.raw_value) == NULL ||
                         cJSON_AddNumberToObject(document, "soil_moisture_percent",
                                                 moisture_reading.moisture_percent) == NULL)) ||
                       (sht40_err == ESP_OK &&
                        (cJSON_AddNumberToObject(document, "sht40_temperature_raw",
                                                 sht40_reading.raw_temperature) == NULL ||
                         cJSON_AddNumberToObject(document, "sht40_humidity_raw",
                                                 sht40_reading.raw_humidity) == NULL ||
                         cJSON_AddNumberToObject(document, "temperature_c",
                                                 sht40_reading.temperature_c) == NULL ||
                         cJSON_AddNumberToObject(document, "humidity_percent",
                                                 sht40_reading.humidity_percent) == NULL)) ||
                       (flow_err == ESP_OK &&
                        (cJSON_AddNumberToObject(document, "flow_zone_a_pulses",
                                                 flow_reading.zone_a.pulses_in_interval) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_b_pulses",
                                                 flow_reading.zone_b.pulses_in_interval) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_c_pulses",
                                                 flow_reading.zone_c.pulses_in_interval) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_main_pump_pulses",
                                                 flow_reading.main_pump.pulses_in_interval) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_a_session_liters",
                                                 flow_reading.zone_a.last_session_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_b_session_liters",
                                                 flow_reading.zone_b.last_session_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_c_session_liters",
                                                 flow_reading.zone_c.last_session_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_main_pump_session_liters",
                                                 flow_reading.main_pump.last_session_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_a_total_liters",
                                                 flow_reading.zone_a.cumulative_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_b_total_liters",
                                                 flow_reading.zone_b.cumulative_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_c_total_liters",
                                                 flow_reading.zone_c.cumulative_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_main_pump_total_liters",
                                                 flow_reading.main_pump.cumulative_liters) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_a_l_min",
                                                 flow_reading.zone_a.liters_per_minute) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_b_l_min",
                                                 flow_reading.zone_b.liters_per_minute) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_zone_c_l_min",
                                                 flow_reading.zone_c.liters_per_minute) == NULL ||
                         cJSON_AddNumberToObject(document, "flow_main_pump_l_min",
                                                 flow_reading.main_pump.liters_per_minute) == NULL ||
                         cJSON_AddBoolToObject(document, "leak_zone_a",
                                               flow_reading.zone_a.leak_detected) == NULL ||
                         cJSON_AddBoolToObject(document, "leak_zone_b",
                                               flow_reading.zone_b.leak_detected) == NULL ||
                         cJSON_AddBoolToObject(document, "leak_zone_c",
                                               flow_reading.zone_c.leak_detected) == NULL ||
                         cJSON_AddBoolToObject(document, "leak_main_pump",
                                               flow_reading.main_pump.leak_detected) == NULL ||
                         cJSON_AddBoolToObject(document, "water_leak_detected",
                                               flow_reading.water_leak_detected) == NULL)))) {
                ESP_LOGE(TAG, "Could not add telemetry to Firebase document");
                cJSON_Delete(document);
            } else if (document != NULL) {
                err = firebase_client_put(FIREBASE_RESOURCE_TELEMETRY, document);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Firebase telemetry upload succeeded: %s %s", date, time);
                    if (moisture_err != ESP_OK) {
                        ESP_LOGW(TAG, "Firebase date/time uploaded without soil moisture");
                    }
                    if (sht40_err != ESP_OK) {
                        ESP_LOGW(TAG, "Firebase telemetry uploaded without SHT40 readings");
                    }
                    if (flow_err != ESP_OK) {
                        ESP_LOGW(TAG, "Firebase telemetry uploaded without flow readings");
                    }

                    const int64_t sample_time_us = esp_timer_get_time();
                    const bool sample_due = statistics_sample_due(sample_time_us);
                    if (sample_due && flow_err != ESP_OK) {
                        ESP_LOGE(TAG, "Firebase history sample is due, but flow readings "
                                      "are unavailable: %s",
                                 esp_err_to_name(flow_err));
                    } else if (sample_due) {
                        const bool history_fields_added =
                            cJSON_AddNumberToObject(document, "total_zone_a_liters",
                                                    flow_reading.zone_a.cumulative_liters) != NULL &&
                            cJSON_AddNumberToObject(document, "total_zone_b_liters",
                                                    flow_reading.zone_b.cumulative_liters) != NULL &&
                            cJSON_AddNumberToObject(document, "total_zone_c_liters",
                                                    flow_reading.zone_c.cumulative_liters) != NULL &&
                            cJSON_AddNumberToObject(document, "total_main_pump_liters",
                                                    flow_reading.main_pump.cumulative_liters) != NULL &&
                            valve_controller_add_history_state(document) == ESP_OK;
                        if (!history_fields_added) {
                            ESP_LOGE(TAG, "Could not add flow and valve state to statistics");
                        } else {
                            const esp_err_t history_err =
                                firebase_client_post(FIREBASE_RESOURCE_HISTORY, document);
                            if (history_err == ESP_OK) {
                                statistics_sample_completed(sample_time_us);
                                ESP_LOGI(TAG, "Firebase statistics sample stored: %s %s",
                                         date, time);
                            } else {
                                ESP_LOGE(TAG, "Firebase statistics sample upload failed: %s",
                                         esp_err_to_name(history_err));
                            }
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Firebase telemetry upload failed: %s",
                             esp_err_to_name(err));
                }
                cJSON_Delete(document);
            }
        }

        vTaskDelayUntil(&last_wake_time, upload_period);
    }
}

void app_main(void)
{
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    prepare_gpio19_for_deep_sleep_wake(wakeup_causes);
    if (s_gpio19_monitor_enabled &&
        xTaskCreate(gpio19_monitor_task, "gpio19_monitor", 2048, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create GPIO19 monitor task");
    }
    esp_err_t err = ds3231_init(CONFIG_N104_I2C_SDA_GPIO, CONFIG_N104_I2C_SCL_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = ds3231_clear_alarm_flags();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not clear DS3231 alarm flags after boot: %s",
                 esp_err_to_name(err));
        return;
    }
    err = soil_moisture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Soil moisture sensor initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }
    err = sht40_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT40 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    if ((wakeup_causes & (1U << ESP_SLEEP_WAKEUP_EXT1)) != 0) {
        ESP_LOGI(TAG, "Woke from DS3231 SQW/INT (EXT1)");
    }

    if (xTaskCreate(rtc_log_task, "rtc_log", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create RTC logging task");
    }

    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed; Wi-Fi time sync unavailable: %s",
                 esp_err_to_name(err));
        return;
    }

    err = wifi_manager_init(CONFIG_N104_WIFI_SSID, CONFIG_N104_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = time_sync_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Time synchronization initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_wait_for_connection(portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection wait failed: %s", esp_err_to_name(err));
        return;
    }

    err = firebase_client_start(firebase_document_received, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firebase client start failed: %s", esp_err_to_name(err));
        return;
    }

    err = flow_monitor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flow monitor initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = valve_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Valve controller initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }

    err = firebase_client_set_power_mode(FIREBASE_POWER_MODE_ALWAYS_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firebase always-on status upload failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Firebase deep sleep status uploaded: always on");
    }

    if (xTaskCreate(firebase_telemetry_upload_task, "firebase_telemetry", 6144, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Could not create Firebase telemetry upload task");
    }
    if (xTaskCreate(power_schedule_task, "power_schedule", 4096, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Could not create power schedule task");
    }
}
