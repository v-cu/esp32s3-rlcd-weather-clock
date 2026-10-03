#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "weather.h"

#define APP_BATTERY_WARNING_MV   3200
#define APP_BATTERY_CRITICAL_MV  3100

#ifdef __cplusplus
extern "C" {
#endif

/* State owned by clock_task.c, read by the overlay pages (same task, no locking needed). */
typedef struct {
    time_t      now;                    /* local epoch from RTC, refreshed every wake-up */

    WeatherData weather;
    bool        weather_valid;

    /* Sync statistics */
    time_t      last_sync_ok;           /* 0 = never */
    time_t      last_attempt;
    bool        last_attempt_failed;
    char        last_error[32];
    time_t      next_sync;              /* next scheduled sync or retry */
    uint32_t    sync_ok_count;
    uint32_t    sync_fail_count;
    uint32_t    consecutive_fails;
    bool        last_time_sync_ok;      /* NTP succeeded during last sync */

    /* Sensors */
    int         battery_mv;
    float       temperature;
    float       humidity;
    bool        sensor_ok;
    uint32_t    sensor_errors;
} AppState;

extern AppState g_app;

#ifdef __cplusplus
}
#endif
