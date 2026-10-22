#include <stdint.h>
#include <time.h>

#include "ds3231.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "firebase_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"
#include "wifi_manager.h"

static const char *TAG = "app";

static void firebase_document_received(firebase_resource_t resource,
                                       const cJSON *document, void *context)
{
    (void)context;

    const char *resource_name = resource == FIREBASE_RESOURCE_ZONES ? "zones" : "system config";
    if (cJSON_IsObject(document) || cJSON_IsArray(document)) {
        ESP_LOGI(TAG, "Received Firebase %s document", resource_name);
    } else if (cJSON_IsNull(document)) {
        ESP_LOGW(TAG, "Firebase %s document is null", resource_name);
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

static void firebase_time_upload_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t upload_period = pdMS_TO_TICKS(3000);

    for (;;) {
        struct tm rtc_time = {0};
        char date[11];
        char time[9];
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
                ESP_LOGE(TAG, "Could not allocate Firebase time document");
            } else if (cJSON_AddStringToObject(document, "rtc_date", date) == NULL ||
                       cJSON_AddStringToObject(document, "rtc_time", time) == NULL ||
                       cJSON_AddStringToObject(document, "deep_sleep_status", "always on") == NULL) {
                ESP_LOGE(TAG, "Could not add date/time to Firebase document");
                cJSON_Delete(document);
            } else {
                err = firebase_client_patch(FIREBASE_RESOURCE_ZONES, document);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Firebase date/time upload succeeded: %s %s", date, time);
                    ESP_LOGI(TAG, "Firebase deep sleep status uploaded: always on");
                } else {
                    ESP_LOGE(TAG, "Firebase date/time upload failed: %s",
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

    err = firebase_client_set_power_mode(FIREBASE_POWER_MODE_ALWAYS_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firebase always-on status upload failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Firebase deep sleep status uploaded: always on");
    }

    if (xTaskCreate(firebase_time_upload_task, "firebase_time", 6144, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Could not create Firebase date/time upload task");
    }
}
