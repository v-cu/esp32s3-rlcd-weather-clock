#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_DAYS   5      /* fetched; 4 are shown starting from "today" */
#define WEATHER_HOURS  48     /* hourly forecast kept from the fetch time */

#define WEATHER_UNKNOWN_U8   0xFF

typedef struct {
    int day;
    int month;
    int weekday;          /* 0 = Sunday */
    int temp_max;
    int temp_min;
    int weathercode;
    time_t date;          /* local midnight (epoch) */
    time_t sunrise;       /* epoch, 0 if unknown */
    time_t sunset;
} WeatherDay;

typedef struct {
    time_t   time;        /* start of the hour (epoch) */
    int16_t  temp_x10;
    int16_t  feels_x10;
    uint8_t  humidity;    /* % */
    uint8_t  precip_prob; /* %, WEATHER_UNKNOWN_U8 if not provided */
    uint8_t  code;        /* WMO weather code */
    uint16_t wind_x10;    /* km/h * 10 */
    uint16_t wind_dir;    /* degrees */
    uint16_t pressure;    /* hPa (MSL), 0 if unknown */
} WeatherHour;

typedef struct {
    WeatherDay  days[WEATHER_DAYS];
    WeatherHour hours[WEATHER_HOURS];
    int         hour_count;
    WeatherHour current;          /* "current" block measured at fetch time */
    bool        current_valid;
    time_t      fetched_at;
} WeatherData;

/* Downloads daily + hourly + current data from Open-Meteo (one request). */
bool Weather_Fetch(WeatherData *out);

/* Hourly entry covering 'now' (falls back to 'current' within 90 min of the fetch), NULL if expired. */
const WeatherHour *Weather_FindHour(const WeatherData *w, time_t now);

/* Index of the first day entry that is today or later, -1 if none. */
int Weather_FirstDayIndex(const WeatherData *w, time_t now);

/* Short English description of a WMO weather code. */
const char *Weather_CodeText(int code);

#ifdef __cplusplus
}
#endif
