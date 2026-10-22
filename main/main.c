#include <inttypes.h>
#include <stdint.h>
#include <time.h>

#include "ds3231.h"
#include "esp_log.h"
#include "esp_sleep.h"
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

static void firebase_document_received(firebase_resource_t resource,
                                       const cJSON *document, void *context)
{
    (void)context;

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
            const esp_err_t err = resource == FIREBASE_RESOURCE_COMMANDS
                                      ? valve_controller_apply_firebase_commands(document)
                                      : valve_controller_apply_firebase_config(document);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not apply Firebase %s: %s",
                         resource_name,
                         esp_err_to_name(err));
            }
        }
    } else if (cJSON_IsNull(document)) {
        if (resource == FIREBASE_RESOURCE_COMMANDS) {
            const esp_err_t err = valve_controller_initialize_firebase_commands();
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
            } else if (cJSON_AddStringToObject(document, "rtc_date", date) == NULL ||
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
                                               flow_reading.water_leak_detected) == NULL))) {
                ESP_LOGE(TAG, "Could not add telemetry to Firebase document");
                cJSON_Delete(document);
            } else {
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
    esp_err_t err = ds3231_init(CONFIG_N100_I2C_SDA_GPIO, CONFIG_N100_I2C_SCL_GPIO);
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

    err = wifi_manager_init(CONFIG_N100_WIFI_SSID, CONFIG_N100_WIFI_PASSWORD);
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
}
