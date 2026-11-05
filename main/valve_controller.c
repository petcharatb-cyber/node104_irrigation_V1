#include "valve_controller.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ds3231.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "firebase_client.h"
#include "flow_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define VALVE_ZONE_COUNT 3
#define VALVE_MAX_TIMER_INTERVALS 8
#define VALVE_CONTROLLER_TASK_STACK_SIZE 4096
#define VALVE_CONTROLLER_POLL_DELAY_MS 100
#define VALVE_TIMER_CHECK_INTERVAL_MS 1000

static const char *TAG = "valve_controller";

typedef struct {
    gpio_num_t open_gpio;
    gpio_num_t close_gpio;
    gpio_num_t solenoid_gpio;
} zone_gpio_t;

static const zone_gpio_t s_zone_gpios[VALVE_ZONE_COUNT] = {
    {CONFIG_N103_BALL_VALVE_A_OPEN_GPIO, CONFIG_N103_BALL_VALVE_A_CLOSE_GPIO,
     CONFIG_N103_SOLENOID_A_GPIO},
    {CONFIG_N103_BALL_VALVE_B_OPEN_GPIO, CONFIG_N103_BALL_VALVE_B_CLOSE_GPIO,
     CONFIG_N103_SOLENOID_B_GPIO},
    {CONFIG_N103_BALL_VALVE_C_OPEN_GPIO, CONFIG_N103_BALL_VALVE_C_CLOSE_GPIO,
     CONFIG_N103_SOLENOID_C_GPIO},
};
static const gpio_num_t s_pump_gpio = CONFIG_N103_MAIN_PUMP_RELAY_GPIO;
static const char *const s_zone_names[VALVE_ZONE_COUNT] = {"zone_a", "zone_b", "zone_c"};
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_desired_open[VALVE_ZONE_COUNT];
static bool s_ball_valve_open[VALVE_ZONE_COUNT];
static bool s_ball_motor_moving[VALVE_ZONE_COUNT];
static bool s_solenoid_open[VALVE_ZONE_COUNT];
static bool s_initialized;
static bool s_controller_ready;
static bool s_main_pump_on;
static bool s_force_all_closed;
static bool s_flow_reset_command_applied;
static bool s_solenoid_enabled = true;
static bool s_zone_timer_mode[VALVE_ZONE_COUNT];
static bool s_manual_overwrite_allowed[VALVE_ZONE_COUNT];
static bool s_manual_override[VALVE_ZONE_COUNT];
static uint8_t s_timer_count[VALVE_ZONE_COUNT];
static uint16_t s_timer_open_minute[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS];
static uint16_t s_timer_close_minute[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS];
static uint32_t s_ball_valve_run_time_seconds =
    CONFIG_N103_BALL_VALVE_RUN_TIME_SECONDS;
static uint32_t s_solenoid_close_delay_seconds =
    CONFIG_N103_SOLENOID_CLOSE_DELAY_SECONDS;

static bool parse_clock_minute(const cJSON *value, uint16_t *minute_out)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
        strlen(value->valuestring) != 5 || value->valuestring[2] != ':') {
        return false;
    }
    const char *clock = value->valuestring;
    if (clock[0] < '0' || clock[0] > '2' || clock[1] < '0' || clock[1] > '9' ||
        clock[3] < '0' || clock[3] > '5' || clock[4] < '0' || clock[4] > '9') {
        return false;
    }
    const uint16_t hour = (uint16_t)((clock[0] - '0') * 10 + clock[1] - '0');
    const uint16_t minute = (uint16_t)((clock[3] - '0') * 10 + clock[4] - '0');
    if (hour > 23) {
        return false;
    }
    *minute_out = (uint16_t)(hour * 60 + minute);
    return true;
}

static bool timer_interval_active(uint16_t now, uint16_t open_at, uint16_t close_at)
{
    if (open_at < close_at) {
        return now >= open_at && now < close_at;
    }
    return now >= open_at || now < close_at;
}

static void update_timer_targets(void)
{
    struct tm now = {0};
    if (ds3231_get_time(&now) != ESP_OK) {
        return;
    }
    const uint16_t minute = (uint16_t)(now.tm_hour * 60 + now.tm_min);
    bool timer_mode[VALVE_ZONE_COUNT];
    bool manual_override[VALVE_ZONE_COUNT];
    uint8_t timer_count[VALVE_ZONE_COUNT];
    uint16_t opens[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS];
    uint16_t closes[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS];
    portENTER_CRITICAL(&s_state_mux);
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        timer_mode[zone] = s_zone_timer_mode[zone];
        manual_override[zone] = s_manual_override[zone];
        timer_count[zone] = s_timer_count[zone];
        memcpy(opens[zone], s_timer_open_minute[zone], sizeof(opens[zone]));
        memcpy(closes[zone], s_timer_close_minute[zone], sizeof(closes[zone]));
    }
    portEXIT_CRITICAL(&s_state_mux);

    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        bool force_closed;
        portENTER_CRITICAL(&s_state_mux);
        force_closed = s_force_all_closed;
        portEXIT_CRITICAL(&s_state_mux);
        if (force_closed) {
            portENTER_CRITICAL(&s_state_mux);
            s_desired_open[zone] = false;
            portEXIT_CRITICAL(&s_state_mux);
            continue;
        }
        if (!timer_mode[zone] || manual_override[zone]) {
            continue;
        }
        bool should_open = false;
        for (size_t interval = 0; interval < timer_count[zone]; ++interval) {
            if (timer_interval_active(minute, opens[zone][interval], closes[zone][interval])) {
                should_open = true;
                break;
            }
        }
        portENTER_CRITICAL(&s_state_mux);
        s_desired_open[zone] = should_open;
        portEXIT_CRITICAL(&s_state_mux);
    }
}

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
            gpio == CONFIG_N103_I2C_SDA_GPIO ||
            gpio == CONFIG_N103_I2C_SCL_GPIO ||
            gpio == CONFIG_N103_RTC_INT_GPIO ||
            gpio == CONFIG_N103_SOIL_MOISTURE_GPIO ||
            gpio == CONFIG_N103_FLOW_ZONE_A_GPIO ||
            gpio == CONFIG_N103_FLOW_ZONE_B_GPIO ||
            gpio == CONFIG_N103_FLOW_ZONE_C_GPIO ||
            gpio == CONFIG_N103_FLOW_MAIN_GPIO) {
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
        flow_monitor_set_main_pump_active(any_zone_open);
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
    s_solenoid_open[zone] = solenoid_open && s_solenoid_enabled;
    portEXIT_CRITICAL(&s_state_mux);
}

static void set_solenoid_state(size_t zone, bool open)
{
    portENTER_CRITICAL(&s_state_mux);
    open = open && s_solenoid_enabled;
    s_solenoid_open[zone] = open;
    portEXIT_CRITICAL(&s_state_mux);
    relay_set(s_zone_gpios[zone].solenoid_gpio, open);
}

static bool solenoid_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_state_mux);
    enabled = s_solenoid_enabled;
    portEXIT_CRITICAL(&s_state_mux);
    return enabled;
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
    portENTER_CRITICAL(&s_state_mux);
    s_ball_motor_moving[zone] = true;
    portEXIT_CRITICAL(&s_state_mux);
    relay_set(active_gpio, true);
    ESP_LOGI(TAG, "Zone %c ball valve motor moving %s for %" PRIu32 " seconds",
             'A' + (int)zone, open ? "OPEN" : "CLOSE",
             ball_valve_run_time_seconds());
    vTaskDelay(pdMS_TO_TICKS(ball_valve_run_time_seconds() * 1000U));
    relay_set(active_gpio, false);
    portENTER_CRITICAL(&s_state_mux);
    s_ball_motor_moving[zone] = false;
    portEXIT_CRITICAL(&s_state_mux);
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
    const uint32_t delay_seconds =
        solenoid_enabled() ? solenoid_close_delay_seconds() : 0;
    if (delay_seconds > 0) {
        ESP_LOGI(TAG, "Zone %c solenoid will close in %" PRIu32 " seconds",
                 'A' + (int)zone, delay_seconds);
        const uint32_t delay_ms = delay_seconds * 1000U;
        for (uint32_t elapsed_ms = 0; elapsed_ms < delay_ms && solenoid_enabled();
             elapsed_ms += 100U) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
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
        static TickType_t last_timer_check;
        const TickType_t now = xTaskGetTickCount();
        if (now - last_timer_check >= pdMS_TO_TICKS(VALVE_TIMER_CHECK_INTERVAL_MS)) {
            update_timer_targets();
            last_timer_check = now;
        }
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
                    flow_monitor_set_zone_active((flow_zone_t)zone, true);
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
                flow_monitor_set_zone_active((flow_zone_t)zone, false);
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

esp_err_t valve_controller_initialize_firebase_commands(const cJSON *document)
{
    const bool document_is_missing = document == NULL || cJSON_IsNull(document);
    if (!document_is_missing && !cJSON_IsObject(document)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *commands = cJSON_CreateObject();
    if (commands == NULL) {
        return ESP_ERR_NO_MEM;
    }

    static const char *const command_names[] = {
        "zone_a",
        "zone_b",
        "zone_c",
        "manual_override_zone_a",
        "manual_override_zone_b",
        "manual_override_zone_c",
        "reset_flow_totals",
    };
    for (size_t index = 0;
         index < sizeof(command_names) / sizeof(command_names[0]); ++index) {
        if ((document_is_missing ||
             cJSON_GetObjectItemCaseSensitive(document, command_names[index]) == NULL) &&
            cJSON_AddBoolToObject(commands, command_names[index], false) == NULL) {
            cJSON_Delete(commands);
            return ESP_ERR_NO_MEM;
        }
    }

    if (commands->child == NULL) {
        cJSON_Delete(commands);
        return ESP_OK;
    }

    const esp_err_t err = document_is_missing
                              ? firebase_client_put(FIREBASE_RESOURCE_COMMANDS, commands)
                              : firebase_client_patch(FIREBASE_RESOURCE_COMMANDS, commands);
    cJSON_Delete(commands);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialized missing Firebase command defaults");
    } else {
        ESP_LOGE(TAG, "Could not initialize Firebase command defaults: %s",
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

    const cJSON *reset_totals =
        cJSON_GetObjectItemCaseSensitive(commands, "reset_flow_totals");
    if (reset_totals != NULL && !cJSON_IsBool(reset_totals)) {
        ESP_LOGE(TAG, "Firebase reset_flow_totals command must be boolean");
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
        char override_name[40];
        const int written = snprintf(override_name, sizeof(override_name),
                                     "manual_override_%s", s_zone_names[zone]);
        if (written < 0 || (size_t)written >= sizeof(override_name)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const cJSON *override =
            cJSON_GetObjectItemCaseSensitive(commands, override_name);
        if (override != NULL && !cJSON_IsBool(override)) {
            ESP_LOGE(TAG, "Firebase command '%s' must be boolean", override_name);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        char override_name[40];
        const int written = snprintf(override_name, sizeof(override_name),
                                     "manual_override_%s", s_zone_names[zone]);
        if (written < 0 || (size_t)written >= sizeof(override_name)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const cJSON *override =
            cJSON_GetObjectItemCaseSensitive(commands, override_name);
        const cJSON *command =
            cJSON_GetObjectItemCaseSensitive(commands, s_zone_names[zone]);
        portENTER_CRITICAL(&s_state_mux);
        if (override != NULL) {
            const bool requested_override = cJSON_IsTrue(override);
            s_manual_override[zone] =
                requested_override && s_manual_overwrite_allowed[zone];
        }
        if (command != NULL &&
            (!s_zone_timer_mode[zone] || s_manual_override[zone])) {
            s_desired_open[zone] = cJSON_IsTrue(command);
        }
        portEXIT_CRITICAL(&s_state_mux);
    }

    if (reset_totals != NULL) {
        if (cJSON_IsTrue(reset_totals)) {
            if (!s_flow_reset_command_applied) {
                const esp_err_t reset_err = flow_monitor_reset_totals();
                if (reset_err != ESP_OK) {
                    return reset_err;
                }
                s_flow_reset_command_applied = true;
                ESP_LOGI(TAG, "Persistent flow totals reset");
            }
            cJSON *ack = cJSON_CreateObject();
            if (ack == NULL) {
                return ESP_ERR_NO_MEM;
            }
            if (cJSON_AddBoolToObject(ack, "reset_flow_totals", false) == NULL) {
                cJSON_Delete(ack);
                return ESP_ERR_NO_MEM;
            }
            const esp_err_t ack_err =
                firebase_client_patch(FIREBASE_RESOURCE_COMMANDS, ack);
            cJSON_Delete(ack);
            if (ack_err != ESP_OK) {
                ESP_LOGE(TAG, "Flow total reset acknowledgement failed: %s",
                         esp_err_to_name(ack_err));
                return ack_err;
            }
            ESP_LOGI(TAG, "Persistent flow total reset acknowledged");
            s_flow_reset_command_applied = false;
        } else {
            s_flow_reset_command_applied = false;
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
    const cJSON *solenoid_enabled_setting =
        cJSON_IsObject(document)
            ? cJSON_GetObjectItemCaseSensitive(document, "solenoid_enabled")
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
    if (solenoid_enabled_setting != NULL && !cJSON_IsBool(solenoid_enabled_setting)) {
        ESP_LOGE(TAG, "Firebase solenoid_enabled must be boolean");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t run_time_seconds =
        run_time != NULL ? (uint32_t)run_time->valuedouble
                         : CONFIG_N103_BALL_VALVE_RUN_TIME_SECONDS;
    const uint32_t close_delay_seconds =
        close_delay != NULL ? (uint32_t)close_delay->valuedouble
                            : CONFIG_N103_SOLENOID_CLOSE_DELAY_SECONDS;
    const bool use_solenoid = solenoid_enabled_setting == NULL ||
                              cJSON_IsTrue(solenoid_enabled_setting);

    bool timer_mode[VALVE_ZONE_COUNT];
    bool overwrite_allowed[VALVE_ZONE_COUNT];
    uint8_t timer_count[VALVE_ZONE_COUNT];
    uint16_t timer_open[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS] = {{0}};
    uint16_t timer_close[VALVE_ZONE_COUNT][VALVE_MAX_TIMER_INTERVALS] = {{0}};
    const cJSON *zone_control =
        cJSON_IsObject(document)
            ? cJSON_GetObjectItemCaseSensitive(document, "zone_control")
            : NULL;
    if (zone_control != NULL && !cJSON_IsObject(zone_control)) {
        ESP_LOGE(TAG, "Firebase zone_control must be an object");
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        portENTER_CRITICAL(&s_state_mux);
        timer_mode[zone] = s_zone_timer_mode[zone];
        overwrite_allowed[zone] = s_manual_overwrite_allowed[zone];
        timer_count[zone] = s_timer_count[zone];
        memcpy(timer_open[zone], s_timer_open_minute[zone], sizeof(timer_open[zone]));
        memcpy(timer_close[zone], s_timer_close_minute[zone], sizeof(timer_close[zone]));
        portEXIT_CRITICAL(&s_state_mux);
        if (zone_control == NULL) {
            continue;
        }
        const cJSON *zone_settings =
            cJSON_GetObjectItemCaseSensitive(zone_control, s_zone_names[zone]);
        if (zone_settings == NULL) {
            continue;
        }
        if (!cJSON_IsObject(zone_settings)) {
            ESP_LOGE(TAG, "Firebase zone_control.%s must be an object",
                     s_zone_names[zone]);
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *mode =
            cJSON_GetObjectItemCaseSensitive(zone_settings, "mode");
        if (mode != NULL) {
            if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
                ESP_LOGE(TAG, "Firebase zone mode must be a string");
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(mode->valuestring, "manual_only") == 0) {
                timer_mode[zone] = false;
            } else if (strcmp(mode->valuestring, "timer_only") == 0) {
                timer_mode[zone] = true;
            } else {
                ESP_LOGE(TAG, "Unsupported mode for %s", s_zone_names[zone]);
                return ESP_ERR_INVALID_ARG;
            }
        }
        const cJSON *allow =
            cJSON_GetObjectItemCaseSensitive(zone_settings,
                                             "manual_overwrite_allowed");
        if (allow != NULL) {
            if (!cJSON_IsBool(allow)) {
                ESP_LOGE(TAG, "Firebase manual_overwrite_allowed must be boolean");
                return ESP_ERR_INVALID_ARG;
            }
            overwrite_allowed[zone] = cJSON_IsTrue(allow);
        }
        const cJSON *timers =
            cJSON_GetObjectItemCaseSensitive(zone_settings, "timers");
        if (timers != NULL) {
            if (!cJSON_IsArray(timers) ||
                cJSON_GetArraySize(timers) > VALVE_MAX_TIMER_INTERVALS) {
                ESP_LOGE(TAG, "Firebase zone timers must be an array of at most %d entries",
                         VALVE_MAX_TIMER_INTERVALS);
                return ESP_ERR_INVALID_ARG;
            }
            timer_count[zone] = (uint8_t)cJSON_GetArraySize(timers);
            memset(timer_open[zone], 0, sizeof(timer_open[zone]));
            memset(timer_close[zone], 0, sizeof(timer_close[zone]));
            const cJSON *timer = NULL;
            size_t index = 0;
            cJSON_ArrayForEach(timer, timers) {
                const cJSON *open_at =
                    cJSON_GetObjectItemCaseSensitive(timer, "open_at");
                const cJSON *close_at =
                    cJSON_GetObjectItemCaseSensitive(timer, "close_at");
                if (!cJSON_IsObject(timer) ||
                    !parse_clock_minute(open_at, &timer_open[zone][index]) ||
                    !parse_clock_minute(close_at, &timer_close[zone][index]) ||
                    timer_open[zone][index] == timer_close[zone][index]) {
                    ESP_LOGE(TAG, "Invalid timer interval in zone %s",
                             s_zone_names[zone]);
                    return ESP_ERR_INVALID_ARG;
                }
                ++index;
            }
        }
    }

    if (run_time == NULL || close_delay == NULL || solenoid_enabled_setting == NULL) {
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
                                     close_delay_seconds) != NULL) &&
            (solenoid_enabled_setting != NULL ||
             cJSON_AddBoolToObject(defaults, "solenoid_enabled", true) != NULL);
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

    bool solenoid_configuration_changed;
    bool zone_open[VALVE_ZONE_COUNT];
    portENTER_CRITICAL(&s_state_mux);
    solenoid_configuration_changed = s_solenoid_enabled != use_solenoid;
    s_solenoid_enabled = use_solenoid;
    s_ball_valve_run_time_seconds = run_time_seconds;
    s_solenoid_close_delay_seconds = close_delay_seconds;
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        zone_open[zone] = s_ball_valve_open[zone];
        if (solenoid_configuration_changed) {
            s_solenoid_open[zone] = use_solenoid && zone_open[zone];
        }
        s_zone_timer_mode[zone] = timer_mode[zone];
        s_manual_overwrite_allowed[zone] = overwrite_allowed[zone];
        s_timer_count[zone] = timer_count[zone];
        memcpy(s_timer_open_minute[zone], timer_open[zone],
               sizeof(s_timer_open_minute[zone]));
        memcpy(s_timer_close_minute[zone], timer_close[zone],
               sizeof(s_timer_close_minute[zone]));
        if (!s_zone_timer_mode[zone] || !s_manual_overwrite_allowed[zone]) {
            s_manual_override[zone] = false;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (solenoid_configuration_changed) {
        for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
            const bool solenoid_open = use_solenoid && zone_open[zone];
            relay_set(s_zone_gpios[zone].solenoid_gpio, solenoid_open);
            set_zone_closed_for_leak_monitor(zone, !zone_open[zone] && !solenoid_open);
        }
        ESP_LOGI(TAG, "Solenoid valves %s by Firebase configuration",
                 use_solenoid ? "enabled" : "disabled");
        publish_valve_status();
    }
    return ESP_OK;
}

esp_err_t valve_controller_add_history_state(cJSON *document)
{
    if (document == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool ball_open[VALVE_ZONE_COUNT];
    bool pump_on;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        ball_open[zone] = s_ball_valve_open[zone];
    }
    pump_on = s_main_pump_on;
    portEXIT_CRITICAL(&s_state_mux);

    if (cJSON_AddBoolToObject(document, "relay_zone_a", ball_open[0]) == NULL ||
        cJSON_AddBoolToObject(document, "relay_zone_b", ball_open[1]) == NULL ||
        cJSON_AddBoolToObject(document, "relay_zone_c", ball_open[2]) == NULL ||
        cJSON_AddBoolToObject(document, "relay_main_pump", pump_on) == NULL) {
        ESP_LOGE(TAG, "Could not add valve state to statistics document");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void valve_controller_request_all_closed(bool requested)
{
    portENTER_CRITICAL(&s_state_mux);
    s_force_all_closed = requested;
    if (requested) {
        for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
            s_desired_open[zone] = false;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
}

bool valve_controller_all_zones_closed(void)
{
    bool all_closed = true;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t zone = 0; zone < VALVE_ZONE_COUNT; ++zone) {
        all_closed = all_closed && !s_ball_valve_open[zone] &&
                     !s_solenoid_open[zone] && !s_ball_motor_moving[zone];
    }
    all_closed = all_closed && !s_main_pump_on;
    portEXIT_CRITICAL(&s_state_mux);
    return all_closed;
}
