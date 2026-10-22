#pragma once

#include <time.h>

#include "esp_err.h"

// Programs the next DS3231 Alarm 1 match and enters deep sleep.
// Alarm 1 matches day-of-month and time (not month); later matches are rejected.
// The requested time uses the same local-time convention as the DS3231 clock.
esp_err_t deep_sleep_until(const struct tm *wake_time);
