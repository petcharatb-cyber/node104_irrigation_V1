#include "n100_i2c_bus.h"

#include "driver/gpio.h"

static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t n100_i2c_bus_init(int sda_gpio, int scl_gpio)
{
    if (s_i2c_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)sda_gpio,
        .scl_io_num = (gpio_num_t)scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

esp_err_t n100_i2c_bus_add_device(uint16_t address, uint32_t speed_hz,
                                  i2c_master_dev_handle_t *device)
{
    if (s_i2c_bus == NULL || device == NULL || address > 0x7f || speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed_hz,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &device_config, device);
}
