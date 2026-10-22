#include "firebase_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FIREBASE_RESPONSE_INITIAL_CAPACITY 2048
#define FIREBASE_RESPONSE_MAX_CAPACITY 16384
#define FIREBASE_TASK_STACK_SIZE 8192

static const char *TAG = "firebase";
extern const char firebase_root_pem_start[] asm("_binary_firebase_root_pem_start");
static firebase_document_callback_t s_document_callback;
static void *s_callback_context;
static bool s_auth_query_logged;
static uint32_t s_logged_resource_paths;

typedef struct {
    const char *value;
    size_t length;
    bool from_url;
} auth_value_t;

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
} response_buffer_t;

static const char *resource_path(firebase_resource_t resource)
{
    switch (resource) {
    case FIREBASE_RESOURCE_TELEMETRY:
        return CONFIG_N100_FIREBASE_TELEMETRY_PATH;
    case FIREBASE_RESOURCE_STATUS:
        return CONFIG_N100_FIREBASE_STATUS_PATH;
    case FIREBASE_RESOURCE_COMMANDS:
        return CONFIG_N100_FIREBASE_COMMANDS_PATH;
    case FIREBASE_RESOURCE_CONFIG:
        return CONFIG_N100_FIREBASE_CONFIG_PATH;
    default:
        return NULL;
    }
}

static const firebase_resource_t s_firebase_resources[] = {
    FIREBASE_RESOURCE_TELEMETRY,
    FIREBASE_RESOURCE_STATUS,
    FIREBASE_RESOURCE_COMMANDS,
    FIREBASE_RESOURCE_CONFIG,
};

static const char *resource_name(firebase_resource_t resource)
{
    switch (resource) {
    case FIREBASE_RESOURCE_TELEMETRY:
        return "telemetry";
    case FIREBASE_RESOURCE_STATUS:
        return "status";
    case FIREBASE_RESOURCE_COMMANDS:
        return "commands";
    case FIREBASE_RESOURCE_CONFIG:
        return "config";
    default:
        return "unknown";
    }
}

static bool auth_token_is_url(void)
{
    return strncmp(CONFIG_N100_FIREBASE_AUTH_TOKEN, "https://",
                   sizeof("https://") - 1) == 0 ||
           strncmp(CONFIG_N100_FIREBASE_AUTH_TOKEN, "http://",
                   sizeof("http://") - 1) == 0;
}

static esp_err_t get_auth_value(auth_value_t *auth)
{
    if (auth == NULL || CONFIG_N100_FIREBASE_AUTH_TOKEN[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *configured_value = CONFIG_N100_FIREBASE_AUTH_TOKEN;
    if (!auth_token_is_url()) {
        auth->value = configured_value;
        auth->length = strlen(configured_value);
        auth->from_url = false;
        return ESP_OK;
    }

    if (strncmp(configured_value, "https://", sizeof("https://") - 1) != 0) {
        ESP_LOGE(TAG, "Firebase legacy auth URL must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }

    const char *database_url = CONFIG_N100_FIREBASE_DATABASE_URL;
    if (strncmp(database_url, "https://", sizeof("https://") - 1) != 0) {
        ESP_LOGE(TAG, "Firebase database URL must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }
    const char *configured_host = configured_value + sizeof("https://") - 1;
    const char *database_host = database_url + sizeof("https://") - 1;
    const char *configured_host_end = strpbrk(configured_host, "/?#");
    const char *database_host_end = strpbrk(database_host, "/?#");
    const size_t configured_host_length = configured_host_end != NULL
                                              ? (size_t)(configured_host_end - configured_host)
                                              : strlen(configured_host);
    const size_t database_host_length = database_host_end != NULL
                                            ? (size_t)(database_host_end - database_host)
                                            : strlen(database_host);
    if (configured_host_length == 0 || configured_host_length != database_host_length ||
        strncasecmp(configured_host, database_host, configured_host_length) != 0) {
        ESP_LOGE(TAG, "Firebase auth URL host must match the configured database URL");
        return ESP_ERR_INVALID_ARG;
    }

    const char *query = strchr(configured_value, '?');
    if (query == NULL) {
        ESP_LOGE(TAG, "Firebase auth URL must include an ?auth= query parameter");
        return ESP_ERR_INVALID_ARG;
    }
    ++query;
    while (*query != '\0') {
        if (strncmp(query, "auth=", sizeof("auth=") - 1) == 0) {
            const char *value = query + sizeof("auth=") - 1;
            const char *value_end = strpbrk(value, "&#");
            const size_t value_length = value_end != NULL
                                            ? (size_t)(value_end - value)
                                            : strlen(value);
            if (value_length == 0) {
                ESP_LOGE(TAG, "Firebase auth URL has an empty auth query parameter");
                return ESP_ERR_INVALID_ARG;
            }
            auth->value = value;
            auth->length = value_length;
            auth->from_url = true;
            return ESP_OK;
        }

        const char *next_parameter = strchr(query, '&');
        if (next_parameter == NULL) {
            break;
        }
        query = next_parameter + 1;
    }

    ESP_LOGE(TAG, "Firebase auth URL must include an ?auth= query parameter");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t append_response_data(response_buffer_t *response,
                                      const char *data, size_t length)
{
    if (length > FIREBASE_RESPONSE_MAX_CAPACITY - response->length) {
        ESP_LOGE(TAG, "Firebase response exceeds %u bytes",
                 (unsigned)FIREBASE_RESPONSE_MAX_CAPACITY);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t required = response->length + length + 1;
    if (required > response->capacity) {
        size_t new_capacity = response->capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        if (new_capacity > FIREBASE_RESPONSE_MAX_CAPACITY + 1) {
            new_capacity = FIREBASE_RESPONSE_MAX_CAPACITY + 1;
        }

        char *new_buffer = realloc(response->buffer, new_capacity);
        if (new_buffer == NULL) {
            ESP_LOGE(TAG, "Could not grow Firebase response buffer");
            return ESP_ERR_NO_MEM;
        }
        response->buffer = new_buffer;
        response->capacity = new_capacity;
    }

    memcpy(response->buffer + response->length, data, length);
    response->length += length;
    response->buffer[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t firebase_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    if (event->data == NULL || event->user_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buffer_t *response = event->user_data;
    return append_response_data(response, event->data, (size_t)event->data_len);
}

static bool is_unreserved_url_char(unsigned char character)
{
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') ||
           character == '-' || character == '.' || character == '_' ||
           character == '~';
}

static esp_err_t build_resource_url(firebase_resource_t resource, char **url_out)
{
    const char *path = resource_path(resource);
    if (path == NULL || url_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t path_length = strlen(path);

    const char *base_url = CONFIG_N100_FIREBASE_DATABASE_URL;
    const size_t base_length = strlen(base_url);
    if (strncmp(base_url, "https://", sizeof("https://") - 1) != 0 ||
        base_length == sizeof("https://") - 1 ||
        base_url[base_length - 1] == '/' || path_length == 0 ||
        path[0] == '/' || path[path_length - 1] == '/') {
        ESP_LOGE(TAG, "Firebase URL or resource path is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (path_length > SIZE_MAX - sizeof("//.json") ||
        base_length > SIZE_MAX - path_length - sizeof("//.json")) {
        return ESP_ERR_INVALID_SIZE;
    }

    auth_value_t auth = {0};
    esp_err_t err = get_auth_value(&auth);
    if (err != ESP_OK) {
        return err;
    }
    const bool use_query_auth =
        auth.from_url
#ifdef CONFIG_N100_FIREBASE_AUTH_QUERY
        || true
#endif
        ;
    const uint32_t resource_log_bit = 1U << resource;
    if ((s_logged_resource_paths & resource_log_bit) == 0) {
        ESP_LOGI(TAG, "Firebase resource %s path: %.*s",
                 resource_name(resource),
                 (int)path_length, path);
        s_logged_resource_paths |= resource_log_bit;
    }
    if (use_query_auth && !s_auth_query_logged) {
        ESP_LOGI(TAG, "Using Firebase auth query from configured legacy URL");
        s_auth_query_logged = true;
    }

    size_t url_capacity = base_length + path_length + sizeof("//.json");
    if (use_query_auth) {
    size_t encoded_token_length = 0;
        if (auth.from_url) {
            encoded_token_length = auth.length;
        } else {
            for (const unsigned char *token = (const unsigned char *)auth.value;
                 *token != '\0'; ++token) {
                const size_t encoded_character_length =
                    is_unreserved_url_char(*token) ? 1 : 3;
                if (encoded_token_length > SIZE_MAX - encoded_character_length) {
                    return ESP_ERR_INVALID_SIZE;
                }
                encoded_token_length += encoded_character_length;
            }
        }
        if (url_capacity > SIZE_MAX - (sizeof("?auth=") - 1) ||
            url_capacity + (sizeof("?auth=") - 1) > SIZE_MAX - encoded_token_length) {
            return ESP_ERR_INVALID_SIZE;
        }
        url_capacity += sizeof("?auth=") - 1 + encoded_token_length;
    }
    char *url = malloc(url_capacity);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t offset = base_length;
    memcpy(url, base_url, base_length);
    url[offset++] = '/';
    memcpy(url + offset, path, path_length);
    offset += path_length;
    memcpy(url + offset, ".json", sizeof(".json"));
    offset += sizeof(".json") - 1;

    if (use_query_auth) {
        static const char hex_digits[] = "0123456789ABCDEF";
        memcpy(url + offset, "?auth=", sizeof("?auth=") - 1);
        offset += sizeof("?auth=") - 1;
        for (size_t i = 0; i < auth.length; ++i) {
            const unsigned char token = (unsigned char)auth.value[i];
            if (auth.from_url || is_unreserved_url_char(token)) {
                url[offset++] = (char)token;
            } else {
                url[offset++] = '%';
                url[offset++] = hex_digits[token >> 4];
                url[offset++] = hex_digits[token & 0x0F];
            }
        }
        url[offset] = '\0';
    }

    *url_out = url;
    return ESP_OK;
}

static esp_err_t perform_request(firebase_resource_t resource,
                                 esp_http_client_method_t method,
                                 const char *body, size_t body_length,
                                 char **response_body, size_t *response_length)
{
    if (response_body != NULL) {
        *response_body = NULL;
    }
    if (response_length != NULL) {
        *response_length = 0;
    }
    if (CONFIG_N100_FIREBASE_AUTH_TOKEN[0] == '\0') {
        ESP_LOGE(TAG, "Firebase auth token is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if ((method == HTTP_METHOD_PATCH || method == HTTP_METHOD_PUT) &&
        (body == NULL || body_length > INT32_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *url = NULL;
    esp_err_t err = build_resource_url(resource, &url);
    if (err != ESP_OK) {
        return err;
    }

    response_buffer_t response = {
        .buffer = malloc(FIREBASE_RESPONSE_INITIAL_CAPACITY),
        .length = 0,
        .capacity = FIREBASE_RESPONSE_INITIAL_CAPACITY,
    };
    if (response.buffer == NULL) {
        free(url);
        return ESP_ERR_NO_MEM;
    }
    response.buffer[0] = '\0';

    const esp_http_client_config_t client_config = {
        .url = url,
        .method = method,
        .cert_pem = firebase_root_pem_start,
        .skip_cert_common_name_check = false,
        .timeout_ms = 10000,
        .max_authorization_retries = -1,
        .event_handler = firebase_http_event_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == NULL) {
        free(response.buffer);
        free(url);
        return ESP_ERR_NO_MEM;
    }

#ifdef CONFIG_N100_FIREBASE_AUTH_QUERY
    err = ESP_OK;
#else
    auth_value_t auth = {0};
    err = get_auth_value(&auth);
    if (err == ESP_OK && !auth.from_url) {
        const char *auth_scheme =
            strncmp(auth.value, "Bearer ", sizeof("Bearer ") - 1) == 0
                ? ""
                : "Bearer ";
        const size_t scheme_length = strlen(auth_scheme);
        if (scheme_length == 0) {
            err = esp_http_client_set_header(client, "Authorization", auth.value);
        } else {
            const size_t header_length = scheme_length + auth.length + 1;
            char *header_value = malloc(header_length);
            if (header_value == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                snprintf(header_value, header_length, "%s%s", auth_scheme, auth.value);
                err = esp_http_client_set_header(client, "Authorization", header_value);
                free(header_value);
            }
        }
    }
#endif
    if (err == ESP_OK && method != HTTP_METHOD_GET) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (err == ESP_OK && method != HTTP_METHOD_GET) {
        err = esp_http_client_set_post_field(client, body, (int)body_length);
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err == ESP_OK) {
        const int status_code = esp_http_client_get_status_code(client);
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "Firebase HTTP request failed with status %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        const int status_code = esp_http_client_get_status_code(client);
        if (status_code == 401) {
            ESP_LOGE(TAG, "Firebase rejected the request with HTTP 401. Check the "
                          "configured token, auth transport, resource path, and "
                          "Realtime Database rules. ESP-IDF may return "
                          "ESP_ERR_NOT_SUPPORTED when the server omits a "
                          "WWW-Authenticate challenge");
        } else {
            ESP_LOGE(TAG, "Firebase HTTPS request failed: %s (HTTP status %d)",
                     esp_err_to_name(err), status_code);
        }
    }

    esp_err_t cleanup_err = esp_http_client_cleanup(client);
    free(url);
    if (err == ESP_OK && cleanup_err != ESP_OK) {
        ESP_LOGE(TAG, "Firebase HTTP client cleanup failed: %s",
                 esp_err_to_name(cleanup_err));
        err = cleanup_err;
    }

    if (err == ESP_OK && response_body != NULL) {
        *response_body = response.buffer;
        if (response_length != NULL) {
            *response_length = response.length;
        }
    } else {
        free(response.buffer);
    }
    return err;
}

static esp_err_t get_document(firebase_resource_t resource, cJSON **document_out)
{
    if (document_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *response = NULL;
    size_t response_length = 0;
    esp_err_t err = perform_request(resource, HTTP_METHOD_GET, NULL, 0,
                                    &response, &response_length);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *document = cJSON_ParseWithLength(response, response_length);
    free(response);
    if (document == NULL) {
        ESP_LOGE(TAG, "Firebase returned invalid JSON for resource %d", resource);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *document_out = document;
    return ESP_OK;
}

static esp_err_t write_document(firebase_resource_t resource,
                                esp_http_client_method_t method,
                                const cJSON *document)
{
    if (resource_path(resource) == NULL || document == NULL ||
        (method != HTTP_METHOD_PATCH && method != HTTP_METHOD_PUT)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = cJSON_PrintUnformatted(document);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t body_length = strlen(body);
    esp_err_t err = perform_request(resource, method, body, body_length, NULL, NULL);
    cJSON_free(body);
    return err;
}

esp_err_t firebase_client_patch(firebase_resource_t resource, const cJSON *document)
{
    return write_document(resource, HTTP_METHOD_PATCH, document);
}

esp_err_t firebase_client_put(firebase_resource_t resource, const cJSON *document)
{
    return write_document(resource, HTTP_METHOD_PUT, document);
}

esp_err_t firebase_client_set_power_mode(firebase_power_mode_t mode)
{
    const char *mode_name;
    switch (mode) {
    case FIREBASE_POWER_MODE_ALWAYS_ON:
        mode_name = "always on";
        break;
    case FIREBASE_POWER_MODE_DEEP_SLEEP:
        mode_name = "deep sleep";
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *document = cJSON_CreateObject();
    if (document == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document, "deep_sleep_status", mode_name) == NULL) {
        cJSON_Delete(document);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = firebase_client_patch(FIREBASE_RESOURCE_STATUS, document);
    cJSON_Delete(document);
    return err;
}

static void firebase_polling_task(void *argument)
{
    (void)argument;
    static const firebase_resource_t polled_resources[] = {
        FIREBASE_RESOURCE_COMMANDS,
        FIREBASE_RESOURCE_CONFIG,
    };
    const TickType_t poll_period =
        pdMS_TO_TICKS(CONFIG_N100_FIREBASE_POLL_INTERVAL_MS);
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        const TickType_t poll_start = xTaskGetTickCount();
        for (size_t index = 0;
             index < sizeof(polled_resources) / sizeof(polled_resources[0]); ++index) {
            const firebase_resource_t resource = polled_resources[index];
            cJSON *document = NULL;
            esp_err_t err = get_document(resource, &document);
            if (err == ESP_OK) {
                s_document_callback(resource, document, s_callback_context);
                cJSON_Delete(document);
            } else {
                ESP_LOGW(TAG, "Could not read Firebase resource %d: %s",
                         resource, esp_err_to_name(err));
            }
        }
        const TickType_t elapsed = xTaskGetTickCount() - poll_start;
        if (elapsed < poll_period) {
            vTaskDelay(poll_period - elapsed);
        }
    }
}

esp_err_t firebase_client_start(firebase_document_callback_t callback, void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    auth_value_t auth = {0};
    esp_err_t auth_err = get_auth_value(&auth);
    if (auth_err != ESP_OK) {
        if (CONFIG_N100_FIREBASE_AUTH_TOKEN[0] == '\0') {
            ESP_LOGE(TAG, "Set the Firebase auth token in menuconfig before starting the client");
        }
        return CONFIG_N100_FIREBASE_AUTH_TOKEN[0] == '\0'
                   ? ESP_ERR_INVALID_STATE
                   : auth_err;
    }
    if (s_document_callback != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t index = 0;
         index < sizeof(s_firebase_resources) / sizeof(s_firebase_resources[0]); ++index) {
        const firebase_resource_t resource = s_firebase_resources[index];
        char *url = NULL;
        esp_err_t err = build_resource_url(resource, &url);
        if (err != ESP_OK) {
            return err;
        }
        free(url);
    }

    s_document_callback = callback;
    s_callback_context = context;
    if (xTaskCreate(firebase_polling_task, "firebase_poll", FIREBASE_TASK_STACK_SIZE,
                    NULL, 5, NULL) != pdPASS) {
        s_document_callback = NULL;
        s_callback_context = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
