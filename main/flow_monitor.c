#include "flow_monitor.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define FLOW_SENSOR_COUNT 4

static const char *TAG = "flow_monitor";

typedef struct {
    volatile uint32_t pulse_count;
    volatile int64_t last_pulse_us;
    uint32_t previous_pulse_count;
    uint32_t persisted_pulse_base;
    uint64_t persisted_pulses;
    uint32_t session_start_pulses;
    float last_session_liters;
    bool active;
} flow_counter_t;

static flow_counter_t s_counters[FLOW_SENSOR_COUNT];
static const gpio_num_t s_flow_gpios[FLOW_SENSOR_COUNT] = {
    CONFIG_N100_FLOW_ZONE_A_GPIO,
    CONFIG_N100_FLOW_ZONE_B_GPIO,
    CONFIG_N100_FLOW_ZONE_C_GPIO,
    CONFIG_N100_FLOW_MAIN_GPIO,
};
static portMUX_TYPE s_flow_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_zone_valves_closed[3];
static int64_t s_zone_valves_closed_since_us[3];
static bool s_all_zones_closed;
static int64_t s_all_zones_closed_since_us;
static int64_t s_previous_read_us;
static SemaphoreHandle_t s_totals_mutex;
static const char *const s_total_keys[FLOW_SENSOR_COUNT] = {
    "zone_a", "zone_b", "zone_c", "main_pump",
};

static esp_err_t persist_counter_total(size_t index)
{
    if (index >= FLOW_SENSOR_COUNT || s_totals_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_totals_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t pulse_base;
    uint32_t current_pulses;
    uint64_t persisted;
    portENTER_CRITICAL(&s_flow_mux);
    pulse_base = s_counters[index].persisted_pulse_base;
    current_pulses = s_counters[index].pulse_count;
    persisted = s_counters[index].persisted_pulses;
    portEXIT_CRITICAL(&s_flow_mux);
    const uint32_t pending =
        (uint32_t)(current_pulses - pulse_base);
    if (pending == 0) {
        xSemaphoreGive(s_totals_mutex);
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("flow_totals", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u64(handle, s_total_keys[index], persisted + pending);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_flow_mux);
        s_counters[index].persisted_pulses = persisted + pending;
        s_counters[index].persisted_pulse_base = current_pulses;
        portEXIT_CRITICAL(&s_flow_mux);
    } else {
        ESP_LOGE(TAG, "Could not persist flow total '%s': %s",
                 s_total_keys[index], esp_err_to_name(err));
    }
    xSemaphoreGive(s_totals_mutex);
    return err;
}

static void IRAM_ATTR flow_pulse_isr(void *argument)
{
    flow_counter_t *counter = argument;
    portENTER_CRITICAL_ISR(&s_flow_mux);
    ++counter->pulse_count;
    counter->last_pulse_us = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&s_flow_mux);
}

esp_err_t flow_monitor_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;

    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        const gpio_num_t gpio = s_flow_gpios[index];
        if (!GPIO_IS_VALID_GPIO(gpio) ||
            gpio == CONFIG_N100_I2C_SDA_GPIO ||
            gpio == CONFIG_N100_I2C_SCL_GPIO ||
            gpio == CONFIG_N100_RTC_INT_GPIO ||
            gpio == CONFIG_N100_SOIL_MOISTURE_GPIO) {
            ESP_LOGE(TAG, "Flow sensor GPIO%d is invalid or already in use", gpio);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (gpio == s_flow_gpios[previous]) {
                ESP_LOGE(TAG, "Flow sensor GPIO%d is configured more than once", gpio);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    s_totals_mutex = xSemaphoreCreateMutex();
    if (s_totals_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create flow totals mutex");
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t totals_handle;
    err = nvs_open("flow_totals", NVS_READWRITE, &totals_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open persistent flow totals: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        uint64_t total = 0;
        err = nvs_get_u64(totals_handle, s_total_keys[index], &total);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read persistent flow total '%s': %s",
                     s_total_keys[index], esp_err_to_name(err));
            nvs_close(totals_handle);
            return err;
        }
        s_counters[index].persisted_pulses = total;
    }
    nvs_close(totals_handle);

    gpio_config_t input_config = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        input_config.pin_bit_mask |= 1ULL << s_flow_gpios[index];
    }

    err = gpio_config(&input_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure flow sensor GPIOs: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Could not install GPIO ISR service: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        err = gpio_isr_handler_add(s_flow_gpios[index], flow_pulse_isr,
                                   &s_counters[index]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not attach flow ISR to GPIO%d: %s",
                     s_flow_gpios[index], esp_err_to_name(err));
            return err;
        }
    }

    s_previous_read_us = esp_timer_get_time();
    s_initialized = true;
    ESP_LOGI(TAG, "Flow sensors initialized on GPIO%d, GPIO%d, GPIO%d, GPIO%d",
             s_flow_gpios[0], s_flow_gpios[1], s_flow_gpios[2], s_flow_gpios[3]);
    ESP_LOGI(TAG, "Flow calibration: %d pulses/L",
             CONFIG_N100_FLOW_PULSES_PER_LITER);
    return ESP_OK;
}

void flow_monitor_set_zone_valves_closed(flow_zone_t zone, bool valves_closed)
{
    if (zone < FLOW_ZONE_A || zone > FLOW_ZONE_C) {
        ESP_LOGE(TAG, "Invalid flow zone for leak monitoring");
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_flow_mux);
    if (valves_closed && !s_zone_valves_closed[zone]) {
        s_zone_valves_closed_since_us[zone] = now_us;
    }
    s_zone_valves_closed[zone] = valves_closed;
    portEXIT_CRITICAL(&s_flow_mux);
    ESP_LOGI(TAG, "Zone %c valves reported %s", 'A' + (int)zone,
             valves_closed ? "closed" : "not closed");
}

void flow_monitor_set_all_zones_closed(bool all_zones_closed)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_flow_mux);
    if (all_zones_closed && !s_all_zones_closed) {
        s_all_zones_closed_since_us = now_us;
    }
    s_all_zones_closed = all_zones_closed;
    portEXIT_CRITICAL(&s_flow_mux);
}

static void set_counter_active(size_t index, bool active)
{
    if (index >= FLOW_SENSOR_COUNT) {
        return;
    }

    bool persist_total = false;
    portENTER_CRITICAL(&s_flow_mux);
    flow_counter_t *counter = &s_counters[index];
    if (active && !counter->active) {
        counter->session_start_pulses = counter->pulse_count;
    } else if (!active && counter->active) {
        const uint32_t pulses = counter->pulse_count - counter->session_start_pulses;
        counter->last_session_liters =
            (float)((double)pulses / CONFIG_N100_FLOW_PULSES_PER_LITER);
        persist_total = true;
    }
    counter->active = active;
    portEXIT_CRITICAL(&s_flow_mux);
    if (persist_total) {
        const esp_err_t err = persist_counter_total(index);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not persist completed flow session: %s",
                     esp_err_to_name(err));
        }
    }
}

void flow_monitor_set_zone_active(flow_zone_t zone, bool active)
{
    if (zone < FLOW_ZONE_A || zone > FLOW_ZONE_C) {
        ESP_LOGE(TAG, "Invalid flow zone for session volume");
        return;
    }
    set_counter_active((size_t)zone, active);
}

void flow_monitor_set_main_pump_active(bool active)
{
    set_counter_active(3, active);
}

esp_err_t flow_monitor_reset_totals(void)
{
    if (s_totals_mutex == NULL ||
        xSemaphoreTake(s_totals_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("flow_totals", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open flow totals for reset: %s", esp_err_to_name(err));
        xSemaphoreGive(s_totals_mutex);
        return err;
    }

    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        err = nvs_set_u64(handle, s_total_keys[index], 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not reset persistent flow total '%s': %s",
                     s_total_keys[index], esp_err_to_name(err));
            nvs_close(handle);
            xSemaphoreGive(s_totals_mutex);
            return err;
        }
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not commit flow total reset: %s", esp_err_to_name(err));
    } else {
        portENTER_CRITICAL(&s_flow_mux);
        for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
            s_counters[index].persisted_pulses = 0;
            s_counters[index].persisted_pulse_base =
                s_counters[index].pulse_count;
        }
        portEXIT_CRITICAL(&s_flow_mux);
    }
    xSemaphoreGive(s_totals_mutex);
    return err;
}

esp_err_t flow_monitor_read(flow_monitor_reading_t *reading)
{
    if (reading == NULL || !s_initialized ||
        CONFIG_N100_FLOW_PULSES_PER_LITER <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pulse_counts[FLOW_SENSOR_COUNT];
    int64_t last_pulse_us[FLOW_SENSOR_COUNT];
    bool zone_valves_closed[3];
    int64_t zone_valves_closed_since_us[3];
    bool all_zones_closed;
    int64_t all_zones_closed_since_us;
    uint64_t total_pulses[FLOW_SENSOR_COUNT];
    uint32_t persisted_bases[FLOW_SENSOR_COUNT];
    float last_session_liters[FLOW_SENSOR_COUNT];
    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_flow_mux);
    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        pulse_counts[index] = s_counters[index].pulse_count;
        last_pulse_us[index] = s_counters[index].last_pulse_us;
        total_pulses[index] = s_counters[index].persisted_pulses;
        persisted_bases[index] = s_counters[index].persisted_pulse_base;
        last_session_liters[index] = s_counters[index].last_session_liters;
    }
    for (size_t index = 0; index < 3; ++index) {
        zone_valves_closed[index] = s_zone_valves_closed[index];
        zone_valves_closed_since_us[index] = s_zone_valves_closed_since_us[index];
    }
    all_zones_closed = s_all_zones_closed;
    all_zones_closed_since_us = s_all_zones_closed_since_us;
    portEXIT_CRITICAL(&s_flow_mux);

    const int64_t elapsed_us = now_us - s_previous_read_us;
    if (elapsed_us <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const double elapsed_seconds = (double)elapsed_us / 1000000.0;
    flow_reading_t *sensor_readings[FLOW_SENSOR_COUNT] = {
        &reading->zone_a, &reading->zone_b, &reading->zone_c, &reading->main_pump,
    };
    bool any_leak = false;

    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        const uint32_t pulse_delta =
            pulse_counts[index] - s_counters[index].previous_pulse_count;
        s_counters[index].previous_pulse_count = pulse_counts[index];

        sensor_readings[index]->pulses_in_interval = pulse_delta;
        sensor_readings[index]->liters_in_interval =
            (float)((double)pulse_delta / CONFIG_N100_FLOW_PULSES_PER_LITER);
        sensor_readings[index]->cumulative_liters =
            (float)((double)(total_pulses[index] +
                             (uint32_t)(pulse_counts[index] - persisted_bases[index])) /
                    CONFIG_N100_FLOW_PULSES_PER_LITER);
        sensor_readings[index]->last_session_liters = last_session_liters[index];
        sensor_readings[index]->liters_per_minute =
            (float)((double)pulse_delta * 60.0 /
                    ((double)CONFIG_N100_FLOW_PULSES_PER_LITER * elapsed_seconds));
        if (index < 3) {
            const int64_t leak_start_us =
                zone_valves_closed_since_us[index] +
                (int64_t)CONFIG_N100_LEAK_GRACE_PERIOD_MS * 1000;
            sensor_readings[index]->leak_detected =
                zone_valves_closed[index] && pulse_delta > 0 &&
                last_pulse_us[index] >= leak_start_us;
        } else {
            const int64_t leak_start_us =
                all_zones_closed_since_us +
                (int64_t)CONFIG_N100_LEAK_GRACE_PERIOD_MS * 1000;
            sensor_readings[index]->leak_detected =
                all_zones_closed && pulse_delta > 0 &&
                last_pulse_us[index] >= leak_start_us;
        }
        any_leak = any_leak || sensor_readings[index]->leak_detected;
    }
    s_previous_read_us = now_us;
    reading->water_leak_detected = any_leak;

    for (size_t index = 0; index < FLOW_SENSOR_COUNT; ++index) {
        const uint32_t unsaved_pulses =
            (uint32_t)(pulse_counts[index] - persisted_bases[index]);
        if (unsaved_pulses < (uint32_t)CONFIG_N100_FLOW_PULSES_PER_LITER) {
            continue;
        }
        const esp_err_t persist_err = persist_counter_total(index);
        if (persist_err != ESP_OK) {
            return persist_err;
        }
    }
    return ESP_OK;
}
