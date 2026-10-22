#pragma once

#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    FIREBASE_RESOURCE_ZONES,
    FIREBASE_RESOURCE_CONFIG,
} firebase_resource_t;

typedef enum {
    FIREBASE_POWER_MODE_ALWAYS_ON,
    FIREBASE_POWER_MODE_DEEP_SLEEP,
} firebase_power_mode_t;

// The document is owned by the client and is valid only for the callback duration.
typedef void (*firebase_document_callback_t)(firebase_resource_t resource,
                                             const cJSON *document,
                                             void *context);

esp_err_t firebase_client_start(firebase_document_callback_t callback, void *context);
esp_err_t firebase_client_patch(firebase_resource_t resource, const cJSON *document);
esp_err_t firebase_client_put(firebase_resource_t resource, const cJSON *document);
esp_err_t firebase_client_set_power_mode(firebase_power_mode_t mode);
