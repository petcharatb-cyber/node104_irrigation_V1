#include "deep_sleep.h"

#include "driver/rtc_io.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "firebase_client.h"

static const char *TAG = "deep_sleep";

esp_err_t deep_sleep_until(const struct tm *wake_time)
{
    const gpio_num_t int_gpio = CONFIG_N100_RTC_INT_GPIO;
    if (wake_time == NULL || !RTC_GPIO_IS_VALID_GPIO(int_gpio) ||
        int_gpio == CONFIG_N100_I2C_SDA_GPIO ||
        int_gpio == CONFIG_N100_I2C_SCL_GPIO) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ds3231_set_alarm1(wake_time);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure DS3231 Alarm 1: %s", esp_err_to_name(err));
        return err;
    }

    err = rtc_gpio_init(int_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize RTC GPIO%d: %s", int_gpio, esp_err_to_name(err));
        return err;
    }
    err = rtc_gpio_set_direction(int_gpio, RTC_GPIO_MODE_INPUT_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure RTC GPIO%d as input: %s",
                 int_gpio, esp_err_to_name(err));
        return err;
    }
    err = rtc_gpio_pulldown_dis(int_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not disable pulldown on RTC GPIO%d: %s",
                 int_gpio, esp_err_to_name(err));
        return err;
    }
    err = rtc_gpio_pullup_en(int_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable pull-up on RTC GPIO%d: %s",
                 int_gpio, esp_err_to_name(err));
        return err;
    }

    err = esp_sleep_enable_ext1_wakeup_io(1ULL << int_gpio, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure EXT1 wakeup: %s", esp_err_to_name(err));
        return err;
    }

    err = firebase_client_set_power_mode(FIREBASE_POWER_MODE_DEEP_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not upload deep-sleep status; staying awake: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Firebase deep sleep status uploaded: deep sleep");
    ESP_LOGI(TAG, "Entering deep sleep until DS3231 Alarm 1 on GPIO%d", int_gpio);
    esp_deep_sleep_start();
}
