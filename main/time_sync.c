#include "time_sync.h"

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "ds3231.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

static const char *TAG = "time_sync";
static bool s_sntp_initialized;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!s_sntp_initialized) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_N104_NTP_SERVER);
            config.wait_for_sync = false;
            esp_err_t err = esp_netif_sntp_init(&config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize SNTP: %s", esp_err_to_name(err));
                return;
            }
            s_sntp_initialized = true;
            ESP_LOGI(TAG, "SNTP started with server %s", CONFIG_N104_NTP_SERVER);
        } else {
            esp_err_t err = esp_netif_sntp_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not restart SNTP after reconnect: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    if (event_base == NETIF_SNTP_EVENT && event_id == NETIF_SNTP_TIME_SYNC) {
        time_t now;
        struct tm local_time;
        time(&now);
        if (localtime_r(&now, &local_time) == NULL) {
            ESP_LOGE(TAG, "Could not convert synchronized system time");
            return;
        }

        esp_err_t err = ds3231_set_time(&local_time);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not update DS3231: %s", esp_err_to_name(err));
            return;
        }

        char timestamp[32];
        if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time) > 0) {
            ESP_LOGI(TAG, "System time synchronized and written to DS3231: %s", timestamp);
        } else {
            ESP_LOGI(TAG, "System time synchronized and written to DS3231");
        }
    }
}

esp_err_t time_sync_init(void)
{
    if (setenv("TZ", CONFIG_N104_TIMEZONE, 1) != 0) {
        ESP_LOGE(TAG, "Could not configure timezone");
        return ESP_FAIL;
    }
    tzset();

    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not register IP event handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC,
                                     event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not register SNTP event handler: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Timezone configured: %s", CONFIG_N104_TIMEZONE);
    return ESP_OK;
}
