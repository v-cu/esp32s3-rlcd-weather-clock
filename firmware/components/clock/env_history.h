#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal SHTC3 sensor: one averaged point per 10 minutes, 24 h. */
#define ENV_HISTORY_SLOT_SEC   600
#define ENV_HISTORY_POINTS     144     /* 144 * 10 min = 24 h */

/* Battery voltage: one averaged point per hour, 7 days. */
#define BAT_HISTORY_SLOT_SEC   3600
#define BAT_HISTORY_POINTS     168     /* 168 * 1 h = 7 days */

/* Restores history kept in RTC memory (survives software reset / panic / light and deep sleep,
 * not a power cycle). */
void EnvHistory_Init(void);

/* Feed a measurement (call once a minute). 'now' is the epoch from the RTC. */
void EnvHistory_AddSample(time_t now, float temperature_c, float humidity_percent);
void EnvHistory_AddBattery(time_t now, int battery_mv);

/* Fill arrays (ENV_HISTORY_POINTS each) oldest -> newest. The last point is the running
 * average of the current slot. Values are x10 (21.5 C -> 215, 45.3 % -> 453).
 * Missing points get 'missing'. Returns number of valid points. */
int EnvHistory_GetSeries(int32_t temp_x10[], int32_t hum_x10[], int32_t missing);

/* Battery history in mV, BAT_HISTORY_POINTS entries, oldest -> newest. */
int EnvHistory_GetBattery(int32_t mv[], int32_t missing);

#ifdef __cplusplus
}
#endif
