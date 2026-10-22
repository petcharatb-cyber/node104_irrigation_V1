#include "sht40.h"

#include <stddef.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "n100_i2c_bus.h"

#define SHT40_I2C_ADDRESS 0x44
#define SHT40_MEASURE_HIGH_PRECISION 0xFD
#define SHT40_MEASUREMENT_DELAY_MS 10

static const char *TAG = "sht40";
static i2c_master_dev_handle_t s_device;

static uint8_t sht40_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xff;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) != 0 ? (uint8_t)((crc << 1) ^ 0x31)
                                    : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht40_init(void)
{
    esp_err_t err = n100_i2c_bus_add_device(SHT40_I2C_ADDRESS, 100000, &s_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not add SHT40 to I2C bus: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SHT40 initialized at I2C address 0x%02x", SHT40_I2C_ADDRESS);
    return ESP_OK;
}

esp_err_t sht40_read(sht40_reading_t *reading)
{
    if (reading == NULL || s_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t command = SHT40_MEASURE_HIGH_PRECISION;
    esp_err_t err = i2c_master_transmit(s_device, &command, sizeof(command), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start measurement: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASUREMENT_DELAY_MS));

    uint8_t data[6];
    err = i2c_master_receive(s_device, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read measurement: %s", esp_err_to_name(err));
        return err;
    }
    if (sht40_crc8(data, 2) != data[2] || sht40_crc8(&data[3], 2) != data[5]) {
        ESP_LOGE(TAG, "SHT40 measurement CRC check failed");
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = (uint16_t)((data[0] << 8) | data[1]);
    const uint16_t raw_humidity = (uint16_t)((data[3] << 8) | data[4]);
    float humidity_percent = -6.0f + 125.0f * (float)raw_humidity / 65535.0f;
    if (humidity_percent < 0.0f) {
        humidity_percent = 0.0f;
    } else if (humidity_percent > 100.0f) {
        humidity_percent = 100.0f;
    }

    reading->raw_temperature = raw_temperature;
    reading->raw_humidity = raw_humidity;
    reading->temperature_c = -45.0f + 175.0f * (float)raw_temperature / 65535.0f;
    reading->humidity_percent = humidity_percent;
    return ESP_OK;
}
