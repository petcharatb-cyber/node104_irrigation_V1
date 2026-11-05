#include "soil_moisture.h"

#include <stdint.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define SOIL_MOISTURE_SAMPLE_COUNT 16

static const char *TAG = "soil_moisture";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_channel_t s_adc_channel;

esp_err_t soil_moisture_init(void)
{
    if (CONFIG_N104_SOIL_MOISTURE_DRY_RAW == CONFIG_N104_SOIL_MOISTURE_WET_RAW) {
        ESP_LOGE(TAG, "Dry and saturated calibration values must differ");
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t adc_unit;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_N104_SOIL_MOISTURE_GPIO,
                                               &adc_unit, &s_adc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not a valid ADC input: %s",
                 CONFIG_N104_SOIL_MOISTURE_GPIO, esp_err_to_name(err));
        return err;
    }
    if (adc_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d must map to ADC1", CONFIG_N104_SOIL_MOISTURE_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    err = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize ADC: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure ADC channel: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "AO configured on GPIO%d (ADC1)",
             CONFIG_N104_SOIL_MOISTURE_GPIO);
    ESP_LOGI(TAG, "Calibration raw values: dry=%d, saturated=%d",
             CONFIG_N104_SOIL_MOISTURE_DRY_RAW,
             CONFIG_N104_SOIL_MOISTURE_WET_RAW);
    return ESP_OK;
}

esp_err_t soil_moisture_read(soil_moisture_reading_t *reading)
{
    if (reading == NULL || s_adc_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t raw_total = 0;
    for (int sample = 0; sample < SOIL_MOISTURE_SAMPLE_COUNT; ++sample) {
        int raw_sample;
        esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw_sample);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read ADC: %s", esp_err_to_name(err));
            return err;
        }
        raw_total += raw_sample;
    }

    const int averaged_raw =
        (int)((raw_total + (SOIL_MOISTURE_SAMPLE_COUNT / 2)) /
              SOIL_MOISTURE_SAMPLE_COUNT);
    const int32_t dry_raw = CONFIG_N104_SOIL_MOISTURE_DRY_RAW;
    const int32_t saturated_raw = CONFIG_N104_SOIL_MOISTURE_WET_RAW;
    const int32_t raw_range = saturated_raw - dry_raw;
    int32_t percent = ((int32_t)averaged_raw - dry_raw) * 100 / raw_range;

    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    reading->raw_value = averaged_raw;
    reading->moisture_percent = (int)percent;
    return ESP_OK;
}
