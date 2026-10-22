#include "valve_controller.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "firebase_client.h"
#include "flow_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define VALVE_ZONE_COUNT 3
#define VALVE_CONTROLLER_TASK_STACK_SIZE 4096
#define VALVE_CONTROLLER_POLL_DELAY_MS 100

static const char *TAG = "valve_controller";

typedef struct {
    gpio_num_t open_gpio;
    gpio_num_t close_gpio;
    gpio_num_t solenoid_gpio;
} zone_gpio_t;

static const zone_gpio_t s_zone_gpios[VALVE_ZONE_COUNT] = {
    {CONFIG_N100_BALL_VALVE_A_OPEN_GPIO, CONFIG_N100_BALL_VALVE_A_CLOSE_GPIO,
     CONFIG_N100_SOLENOID_A_GPIO},
    {CONFIG_N100_BALL_VALVE_B_OPEN_GPIO, CONFIG_N100_BALL_VALVE_B_CLOSE_GPIO,
     CONFIG_N100_SOLENOID_B_GPIO},
    {CONFIG_N100_BALL_VALVE_C_OPEN_GPIO, CONFIG_N100_BALL_VALVE_C_CLOSE_GPIO,
     CONFIG_N100_SOLENOID_C_GPIO},
};
static const gpio_num_t s_pump_gpio = CONFIG_N100_MAIN_PUMP_RELAY_GPIO;
static const char *const s_zone_names[VALVE_ZONE_COUNT] = {"zone_a", "zone_b", "zone_c"};
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_desired_open[VALVE_ZONE_COUNT];
static bool s_ball_valve_open[VALVE_ZONE_COUNT];
static bool s_solenoid_open[VALVE_ZONE_COUNT];
static bool s_initialized;
static bool s_controller_ready;
static bool s_main_pump_on;
static uint32_t s_ball_valve_run_time_seconds =
    CONFIG_N100_BALL_VALVE_RUN_TIME_SECONDS;
static uint32_t s_solenoid_close_delay_seconds =
    CONFIG_N100_SOLENOID_CLOSE_DELAY_SECONDS;

static esp_err_t validate_output_gpios(void)
{
    gpio_num_t output_gpios[VALVE_ZONE_COUNT * 3 + 1];
    size_t output_count = 0;
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        output_gpios[output_count++] = s_zone_gpios[zone].open_gpio;
        output_gpios[output_count++] = s_zone_gpios[zone].close_gpio;
        output_gpios[output_count++] = s_zone_gpios[zone].solenoid_gpio;
    }
    output_gpios[output_count++] = s_pump_gpio;

    for (size_t index = 0; index < output_count; ++index) {
        const gpio_num_t gpio = output_gpios[index];
        if (!GPIO_IS_VALID_GPIO(gpio) ||
            gpio == CONFIG_N100_I2C_SDA_GPIO ||
            gpio == CONFIG_N100_I2C_SCL_GPIO ||
            gpio == CONFIG_N100_RTC_INT_GPIO ||
            gpio == CONFIG_N100_SOIL_MOISTURE_GPIO ||
            gpio == CONFIG_N100_FLOW_ZONE_A_GPIO ||
            gpio == CONFIG_N100_FLOW_ZONE_B_GPIO ||
            gpio == CONFIG_N100_FLOW_ZONE_C_GPIO ||
            gpio == CONFIG_N100_FLOW_MAIN_GPIO) {
            ESP_LOGE(TAG, "Relay GPIO%d is invalid or already assigned", gpio);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (gpio == output_gpios[previous]) {
                ESP_LOGE(TAG, "Relay GPIO%d is assigned more than once", gpio);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static void relay_set(gpio_num_t gpio, bool enabled)
{
    const esp_err_t err = gpio_set_level(gpio, enabled ? 0 : 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set relay GPIO%d: %s", gpio, esp_err_to_name(err));
    }
}

static void update_pump_state(void)
{
    bool any_zone_open = false;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        any_zone_open = any_zone_open || s_ball_valve_open[zone];
    }
    if (s_main_pump_on != any_zone_open) {
        s_main_pump_on = any_zone_open;
        portEXIT_CRITICAL(&s_state_mux);
        relay_set(s_pump_gpio, any_zone_open);
        ESP_LOGI(TAG, "Main pump relay %s", any_zone_open ? "ON" : "OFF");
        return;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

static bool desired_open(size_t zone)
{
    bool desired;
    portENTER_CRITICAL(&s_state_mux);
    desired = s_desired_open[zone];
    portEXIT_CRITICAL(&s_state_mux);
    return desired;
}

static void set_zone_states(size_t zone, bool ball_open, bool solenoid_open)
{
    portENTER_CRITICAL(&s_state_mux);
    s_ball_valve_open[zone] = ball_open;
    s_solenoid_open[zone] = solenoid_open;
    portEXIT_CRITICAL(&s_state_mux);
}

static void set_solenoid_state(size_t zone, bool open)
{
    portENTER_CRITICAL(&s_state_mux);
    s_solenoid_open[zone] = open;
    portEXIT_CRITICAL(&s_state_mux);
    relay_set(s_zone_gpios[zone].solenoid_gpio, open);
}

static uint32_t ball_valve_run_time_seconds(void)
{
    uint32_t seconds;
    portENTER_CRITICAL(&s_state_mux);
    seconds = s_ball_valve_run_time_seconds;
    portEXIT_CRITICAL(&s_state_mux);
    return seconds;
}

static uint32_t solenoid_close_delay_seconds(void)
{
    uint32_t seconds;
    portENTER_CRITICAL(&s_state_mux);
    seconds = s_solenoid_close_delay_seconds;
    portEXIT_CRITICAL(&s_state_mux);
    return seconds;
}

static void drive_ball_valve(size_t zone, bool open)
{
    const gpio_num_t active_gpio =
        open ? s_zone_gpios[zone].open_gpio : s_zone_gpios[zone].close_gpio;
    const gpio_num_t inactive_gpio =
        open ? s_zone_gpios[zone].close_gpio : s_zone_gpios[zone].open_gpio;

    relay_set(s_zone_gpios[zone].open_gpio, false);
    relay_set(s_zone_gpios[zone].close_gpio, false);
    relay_set(inactive_gpio, false);
    relay_set(active_gpio, true);
    ESP_LOGI(TAG, "Zone %c ball valve motor moving %s for %" PRIu32 " seconds",
             'A' + (int)zone, open ? "OPEN" : "CLOSE",
             ball_valve_run_time_seconds());
    vTaskDelay(pdMS_TO_TICKS(ball_valve_run_time_seconds() * 1000U));
    relay_set(active_gpio, false);
}

static void set_zone_closed_for_leak_monitor(size_t zone, bool closed)
{
    flow_monitor_set_zone_valves_closed((flow_zone_t)zone, closed);
    bool all_zones_closed = true;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t index = 0; index < VALVE_ZONE_COUNT; ++index) {
        all_zones_closed = all_zones_closed &&
                           !s_ball_valve_open[index] &&
                           !s_solenoid_open[index];
    }
    portEXIT_CRITICAL(&s_state_mux);
    flow_monitor_set_all_zones_closed(all_zones_closed);
}

static void publish_valve_status(void)
{
    bool ball_open[VALVE_ZONE_COUNT];
    bool solenoid_open[VALVE_ZONE_COUNT];
    bool pump_on;
    bool controller_ready;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        ball_open[zone] = s_ball_valve_open[zone];
        solenoid_open[zone] = s_solenoid_open[zone];
    }
    pump_on = s_main_pump_on;
    controller_ready = s_controller_ready;
    portEXIT_CRITICAL(&s_state_mux);

    cJSON *document = cJSON_CreateObject();
    if (document == NULL) {
        ESP_LOGE(TAG, "Could not allocate valve status document");
        return;
    }
    bool added =
        cJSON_AddBoolToObject(document, "ball_valve_zone_a_open", ball_open[0]) != NULL &&
        cJSON_AddBoolToObject(document, "ball_valve_zone_b_open", ball_open[1]) != NULL &&
        cJSON_AddBoolToObject(document, "ball_valve_zone_c_open", ball_open[2]) != NULL &&
        cJSON_AddBoolToObject(document, "solenoid_zone_a_open", solenoid_open[0]) != NULL &&
        cJSON_AddBoolToObject(document, "solenoid_zone_b_open", solenoid_open[1]) != NULL &&
        cJSON_AddBoolToObject(document, "solenoid_zone_c_open", solenoid_open[2]) != NULL &&
        cJSON_AddBoolToObject(document, "main_pump_on", pump_on) != NULL &&
        cJSON_AddBoolToObject(document, "controller_ready", controller_ready) != NULL;
    if (!added) {
        ESP_LOGE(TAG, "Could not add all valve statuses to Firebase document");
    } else {
        const esp_err_t err = firebase_client_patch(FIREBASE_RESOURCE_STATUS, document);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not upload valve status: %s", esp_err_to_name(err));
        }
    }
    cJSON_Delete(document);
}

static void finish_zone_close(size_t zone)
{
    const uint32_t delay_seconds = solenoid_close_delay_seconds();
    if (delay_seconds > 0) {
        ESP_LOGI(TAG, "Zone %c solenoid will close in %" PRIu32 " seconds",
                 'A' + (int)zone, delay_seconds);
        vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000U));
    }
    if (desired_open(zone)) {
        return;
    }
    set_solenoid_state(zone, false);
    set_zone_closed_for_leak_monitor(zone, true);
    publish_valve_status();
}

static void valve_controller_task(void *argument)
{
    (void)argument;

    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        drive_ball_valve(zone, false);
        set_zone_states(zone, false, false);
        relay_set(s_zone_gpios[zone].solenoid_gpio, false);
        update_pump_state();
        set_zone_closed_for_leak_monitor(zone, true);
        publish_valve_status();
    }

    portENTER_CRITICAL(&s_state_mux);
    s_controller_ready = true;
    portEXIT_CRITICAL(&s_state_mux);
    publish_valve_status();

    for (;;) {
        for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
            if (desired_open(zone)) {
                bool was_open;
                portENTER_CRITICAL(&s_state_mux);
                was_open = s_ball_valve_open[zone];
                portEXIT_CRITICAL(&s_state_mux);
                if (was_open) {
                    continue;
                }

                drive_ball_valve(zone, false);
                if (desired_open(zone)) {
                    set_solenoid_state(zone, true);
                    set_zone_closed_for_leak_monitor(zone, false);
                    publish_valve_status();
                    drive_ball_valve(zone, true);
                    set_zone_states(zone, true, true);
                    set_zone_closed_for_leak_monitor(zone, false);
                    update_pump_state();
                    publish_valve_status();
                }
            } else {
                bool was_open;
                portENTER_CRITICAL(&s_state_mux);
                was_open = s_ball_valve_open[zone];
                portEXIT_CRITICAL(&s_state_mux);
                if (!was_open) {
                    continue;
                }

                drive_ball_valve(zone, false);
                set_zone_states(zone, false, true);
                update_pump_state();
                publish_valve_status();
                finish_zone_close(zone);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(VALVE_CONTROLLER_POLL_DELAY_MS));
    }
}

esp_err_t valve_controller_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_output_gpios();
    if (err != ESP_OK) {
        return err;
    }

    gpio_num_t output_gpios[VALVE_ZONE_COUNT * 3 + 1];
    size_t output_count = 0;
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        output_gpios[output_count++] = s_zone_gpios[zone].open_gpio;
        output_gpios[output_count++] = s_zone_gpios[zone].close_gpio;
        output_gpios[output_count++] = s_zone_gpios[zone].solenoid_gpio;
    }
    output_gpios[output_count++] = s_pump_gpio;

    gpio_config_t output_config = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (size_t index = 0; index < output_count; ++index) {
        output_config.pin_bit_mask |= 1ULL << output_gpios[index];
        err = gpio_set_level(output_gpios[index], 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not set relay GPIO%d to OFF: %s",
                     output_gpios[index], esp_err_to_name(err));
            return err;
        }
    }
    err = gpio_config(&output_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure relay GPIOs: %s", esp_err_to_name(err));
        return err;
    }

    publish_valve_status();
    s_initialized = true;
    if (xTaskCreate(valve_controller_task, "valve_control",
                    VALVE_CONTROLLER_TASK_STACK_SIZE, NULL, 6, NULL) != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Relay outputs initialized OFF (active-low); closing all zones");
    return ESP_OK;
}

static esp_err_t update_zone_command(const cJSON *commands, size_t zone)
{
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(commands, s_zone_names[zone]);
    if (command == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsBool(command)) {
        ESP_LOGE(TAG, "Firebase valve command '%s' must be boolean", s_zone_names[zone]);
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_desired_open[zone] = cJSON_IsTrue(command);
    portEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

esp_err_t valve_controller_initialize_firebase_commands(void)
{
    cJSON *commands = cJSON_CreateObject();
    if (commands == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const bool added =
        cJSON_AddBoolToObject(commands, "zone_a", false) != NULL &&
        cJSON_AddBoolToObject(commands, "zone_b", false) != NULL &&
        cJSON_AddBoolToObject(commands, "zone_c", false) != NULL;
    if (!added) {
        cJSON_Delete(commands);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = firebase_client_put(FIREBASE_RESOURCE_COMMANDS, commands);
    cJSON_Delete(commands);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Created default Firebase commands with all zones closed");
    } else {
        ESP_LOGE(TAG, "Could not initialize Firebase commands: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t valve_controller_apply_firebase_commands(const cJSON *document)
{
    if (document == NULL || !cJSON_IsObject(document) || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(document, "valve_commands");
    if (commands == NULL) {
        commands = document;
    } else if (!cJSON_IsObject(commands)) {
        ESP_LOGE(TAG, "Firebase 'valve_commands' must be an object");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        const cJSON *command =
            cJSON_GetObjectItemCaseSensitive(commands, s_zone_names[zone]);
        if (command != NULL && !cJSON_IsBool(command)) {
            ESP_LOGE(TAG, "Firebase valve command '%s' must be boolean",
                     s_zone_names[zone]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        esp_err_t err = update_zone_command(commands, zone);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t valve_controller_apply_firebase_config(const cJSON *document)
{
    if (!s_initialized || (document != NULL && !cJSON_IsObject(document) &&
                           !cJSON_IsNull(document))) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *run_time = cJSON_IsObject(document)
                                ? cJSON_GetObjectItemCaseSensitive(
                                      document, "ball_valve_run_time_seconds")
                                : NULL;
    const cJSON *close_delay = cJSON_IsObject(document)
                                   ? cJSON_GetObjectItemCaseSensitive(
                                         document, "solenoid_close_delay_seconds")
                                   : NULL;
    if (run_time != NULL &&
        (!cJSON_IsNumber(run_time) || run_time->valuedouble < 1 ||
         run_time->valuedouble > 120 ||
         run_time->valuedouble != (double)(uint32_t)run_time->valuedouble)) {
        ESP_LOGE(TAG, "Firebase ball valve run time must be from 1 to 120 seconds");
        return ESP_ERR_INVALID_ARG;
    }
    if (close_delay != NULL &&
        (!cJSON_IsNumber(close_delay) || close_delay->valuedouble < 0 ||
         close_delay->valuedouble > 300 ||
         close_delay->valuedouble != (double)(uint32_t)close_delay->valuedouble)) {
        ESP_LOGE(TAG, "Firebase solenoid close delay must be from 0 to 300 seconds");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t run_time_seconds =
        run_time != NULL ? (uint32_t)run_time->valuedouble
                         : CONFIG_N100_BALL_VALVE_RUN_TIME_SECONDS;
    const uint32_t close_delay_seconds =
        close_delay != NULL ? (uint32_t)close_delay->valuedouble
                            : CONFIG_N100_SOLENOID_CLOSE_DELAY_SECONDS;

    if (run_time == NULL || close_delay == NULL) {
        cJSON *defaults = cJSON_CreateObject();
        if (defaults == NULL) {
            return ESP_ERR_NO_MEM;
        }
        const bool added =
            (run_time != NULL ||
             cJSON_AddNumberToObject(defaults, "ball_valve_run_time_seconds",
                                     run_time_seconds) != NULL) &&
            (close_delay != NULL ||
             cJSON_AddNumberToObject(defaults, "solenoid_close_delay_seconds",
                                     close_delay_seconds) != NULL);
        if (!added) {
            cJSON_Delete(defaults);
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t err =
            firebase_client_patch(FIREBASE_RESOURCE_CONFIG, defaults);
        cJSON_Delete(defaults);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not initialize Firebase valve configuration: %s",
                     esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Initialized missing Firebase valve configuration defaults");
    }

    portENTER_CRITICAL(&s_state_mux);
    s_ball_valve_run_time_seconds = run_time_seconds;
    s_solenoid_close_delay_seconds = close_delay_seconds;
    portEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}
