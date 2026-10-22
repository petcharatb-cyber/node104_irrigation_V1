#include "ds3231.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "n100_i2c_bus.h"

#define DS3231_I2C_ADDRESS 0x68
#define DS3231_REG_SECONDS 0x00
#define DS3231_REG_ALARM1  0x07
#define DS3231_REG_CONTROL 0x0E
#define DS3231_REG_STATUS  0x0F
#define DS3231_A1F_MASK    0x01
#define DS3231_A2F_MASK    0x02
#define DS3231_A1IE_MASK   0x01
#define DS3231_A2IE_MASK   0x02
#define DS3231_INTCN_MASK  0x04
#define DS3231_OSF_MASK    0x80

static i2c_master_dev_handle_t s_ds3231;

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static bool is_valid_bcd(uint8_t value)
{
    return (value & 0x0F) <= 9 && ((value >> 4) & 0x0F) <= 9;
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 1 && (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0))) {
        return 29;
    }
    return days[month];
}

static int compare_time(const struct tm *left, const struct tm *right)
{
    const int left_values[] = {
        left->tm_year, left->tm_mon, left->tm_mday,
        left->tm_hour, left->tm_min, left->tm_sec,
    };
    const int right_values[] = {
        right->tm_year, right->tm_mon, right->tm_mday,
        right->tm_hour, right->tm_min, right->tm_sec,
    };

    for (size_t i = 0; i < sizeof(left_values) / sizeof(left_values[0]); ++i) {
        if (left_values[i] != right_values[i]) {
            return left_values[i] < right_values[i] ? -1 : 1;
        }
    }
    return 0;
}

static bool is_valid_alarm_time(const struct tm *time_info)
{
    const int year = time_info->tm_year + 1900;
    return year >= 2000 && year <= 2199 &&
           time_info->tm_mon >= 0 && time_info->tm_mon <= 11 &&
           time_info->tm_mday >= 1 &&
           time_info->tm_mday <= days_in_month(year, time_info->tm_mon) &&
           time_info->tm_hour >= 0 && time_info->tm_hour <= 23 &&
           time_info->tm_min >= 0 && time_info->tm_min <= 59 &&
           time_info->tm_sec >= 0 && time_info->tm_sec <= 59;
}

static bool is_next_alarm1_match(const struct tm *alarm_time, const struct tm *current_time)
{
    int year = current_time->tm_year + 1900;
    int month = current_time->tm_mon;

    for (int i = 0; i < 12 * 200; ++i) {
        if (alarm_time->tm_mday <= days_in_month(year, month)) {
            struct tm next_match = *current_time;
            next_match.tm_year = year - 1900;
            next_match.tm_mon = month;
            next_match.tm_mday = alarm_time->tm_mday;
            next_match.tm_hour = alarm_time->tm_hour;
            next_match.tm_min = alarm_time->tm_min;
            next_match.tm_sec = alarm_time->tm_sec;

            if (compare_time(&next_match, current_time) > 0) {
                return compare_time(&next_match, alarm_time) == 0;
            }
        }

        if (++month > 11) {
            month = 0;
            ++year;
        }
        if (year > 2199) {
            break;
        }
    }
    return false;
}

static esp_err_t read_registers(uint8_t first_register, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_ds3231, &first_register, sizeof(first_register),
                                       data, length, 1000);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_ds3231, data, sizeof(data), 1000);
}

esp_err_t ds3231_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    esp_err_t err = n100_i2c_bus_init(sda_gpio, scl_gpio);
    if (err != ESP_OK) {
        return err;
    }

    return n100_i2c_bus_add_device(DS3231_I2C_ADDRESS, 100000, &s_ds3231);
}

esp_err_t ds3231_get_time(struct tm *time_info)
{
    if (time_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    esp_err_t err = read_registers(DS3231_REG_STATUS, &status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }
    if ((status & DS3231_OSF_MASK) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7] = {0};
    err = read_registers(DS3231_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    if (!is_valid_bcd(raw[0] & 0x7F) || !is_valid_bcd(raw[1] & 0x7F) ||
        !is_valid_bcd(raw[4] & 0x3F) || !is_valid_bcd(raw[5] & 0x1F) ||
        !is_valid_bcd(raw[6])) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    struct tm result = {0};
    result.tm_sec = from_bcd(raw[0] & 0x7F);
    result.tm_min = from_bcd(raw[1] & 0x7F);

    if ((raw[2] & 0x40) != 0) {
        const int hour_12 = from_bcd(raw[2] & 0x1F);
        if (hour_12 < 1 || hour_12 > 12) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        result.tm_hour = (hour_12 % 12) + ((raw[2] & 0x20) ? 12 : 0);
    } else {
        if (!is_valid_bcd(raw[2] & 0x3F)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        result.tm_hour = from_bcd(raw[2] & 0x3F);
    }

    const int day_of_week = raw[3] & 0x07;
    const int month = from_bcd(raw[5] & 0x1F);
    const int year = 2000 + from_bcd(raw[6]) + ((raw[5] & 0x80) ? 100 : 0);
    if (day_of_week < 1 || day_of_week > 7 || result.tm_sec > 59 ||
        result.tm_min > 59 || result.tm_hour > 23 || month < 1 || month > 12) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    result.tm_mday = from_bcd(raw[4] & 0x3F);
    result.tm_mon = month - 1;
    result.tm_year = year - 1900;
    result.tm_wday = day_of_week % 7;
    if (result.tm_mday < 1 || result.tm_mday > days_in_month(year, result.tm_mon)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *time_info = result;
    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *time_info)
{
    if (time_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int year = time_info->tm_year + 1900;
    if (year < 2000 || year > 2199 || time_info->tm_mon < 0 || time_info->tm_mon > 11 ||
        time_info->tm_mday < 1 ||
        time_info->tm_mday > days_in_month(year, time_info->tm_mon) ||
        time_info->tm_hour < 0 ||
        time_info->tm_hour > 23 || time_info->tm_min < 0 || time_info->tm_min > 59 ||
        time_info->tm_sec < 0 || time_info->tm_sec > 59 || time_info->tm_wday < 0 ||
        time_info->tm_wday > 6) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t month = (uint8_t)(time_info->tm_mon + 1);
    const uint8_t raw[8] = {
        DS3231_REG_SECONDS,
        to_bcd((uint8_t)time_info->tm_sec),
        to_bcd((uint8_t)time_info->tm_min),
        to_bcd((uint8_t)time_info->tm_hour),
        to_bcd((uint8_t)(time_info->tm_wday + 1)),
        to_bcd((uint8_t)time_info->tm_mday),
        (uint8_t)(to_bcd(month) | ((year >= 2100) ? 0x80 : 0)),
        to_bcd((uint8_t)(year % 100)),
    };
    esp_err_t err = i2c_master_transmit(s_ds3231, raw, sizeof(raw), 1000);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status = 0;
    err = read_registers(DS3231_REG_STATUS, &status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t status_write[] = {
        DS3231_REG_STATUS,
        (uint8_t)(status & (uint8_t)~DS3231_OSF_MASK),
    };
    return i2c_master_transmit(s_ds3231, status_write, sizeof(status_write), 1000);
}

esp_err_t ds3231_clear_alarm_flags(void)
{
    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    esp_err_t err = read_registers(DS3231_REG_STATUS, &status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }

    return write_register(DS3231_REG_STATUS,
                          (uint8_t)(status & (uint8_t)~(DS3231_A1F_MASK |
                                                        DS3231_A2F_MASK)));
}

esp_err_t ds3231_set_alarm1(const struct tm *alarm_time)
{
    if (alarm_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_valid_alarm_time(alarm_time)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm current_time = {0};
    esp_err_t err = ds3231_get_time(&current_time);
    if (err != ESP_OK) {
        return err;
    }
    if (compare_time(alarm_time, &current_time) <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_next_alarm1_match(alarm_time, &current_time)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t control = 0;
    err = read_registers(DS3231_REG_CONTROL, &control, sizeof(control));
    if (err != ESP_OK) {
        return err;
    }

    control &= (uint8_t)~(DS3231_A1IE_MASK | DS3231_A2IE_MASK);
    control |= DS3231_INTCN_MASK;
    err = write_register(DS3231_REG_CONTROL, control);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t alarm_registers[] = {
        DS3231_REG_ALARM1,
        to_bcd((uint8_t)alarm_time->tm_sec),
        to_bcd((uint8_t)alarm_time->tm_min),
        to_bcd((uint8_t)alarm_time->tm_hour),
        to_bcd((uint8_t)alarm_time->tm_mday),
    };
    err = i2c_master_transmit(s_ds3231, alarm_registers, sizeof(alarm_registers), 1000);
    if (err != ESP_OK) {
        return err;
    }

    err = ds3231_clear_alarm_flags();
    if (err != ESP_OK) {
        return err;
    }

    control |= DS3231_INTCN_MASK | DS3231_A1IE_MASK;
    control &= (uint8_t)~DS3231_A2IE_MASK;
    return write_register(DS3231_REG_CONTROL, control);
}
