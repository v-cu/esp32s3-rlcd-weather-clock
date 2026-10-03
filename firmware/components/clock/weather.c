#include "weather.h"
#include "weather_config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Weather";

#define RESPONSE_BUF_SIZE  (24 * 1024)

#define HOURLY_FIELDS "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code," \
                      "wind_speed_10m,wind_direction_10m,precipitation_probability,pressure_msl"

typedef struct {
    char *buf;
    int len;
    int cap;
    bool overflow;
} HttpBuffer;

static char *s_response = NULL;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    HttpBuffer *hb = (HttpBuffer *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && hb) {
        if (hb->len + evt->data_len < hb->cap - 1) {
            memcpy(hb->buf + hb->len, evt->data, evt->data_len);
            hb->len += evt->data_len;
            hb->buf[hb->len] = '\0';
        } else {
            hb->overflow = true;
        }
    }
    return ESP_OK;
}

static int round_i(double v)
{
    return (v >= 0.0) ? (int)(v + 0.5) : (int)(v - 0.5);
}

static bool arr_num(const cJSON *arr, int i, double *out)
{
    const cJSON *it = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsNumber(it)) return false;
    *out = it->valuedouble;
    return true;
}

static bool obj_num(const cJSON *obj, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(it)) return false;
    *out = it->valuedouble;
    return true;
}

static void fill_hour_from_values(WeatherHour *h, double t, double feels, double rh, double code,
                                  double wind, double dir, double pp, bool pp_ok, double press, bool press_ok)
{
    h->temp_x10 = (int16_t)round_i(t * 10.0);
    h->feels_x10 = (int16_t)round_i(feels * 10.0);
    h->humidity = (uint8_t)round_i(rh);
    h->code = (uint8_t)round_i(code);
    h->wind_x10 = (uint16_t)round_i(wind * 10.0);
    h->wind_dir = (uint16_t)round_i(dir);
    h->precip_prob = pp_ok ? (uint8_t)round_i(pp) : WEATHER_UNKNOWN_U8;
    h->pressure = press_ok ? (uint16_t)round_i(press) : 0;
}

static bool parse_daily(const cJSON *root, WeatherData *out)
{
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *times = daily ? cJSON_GetObjectItem(daily, "time") : NULL;
    const cJSON *codes = daily ? cJSON_GetObjectItem(daily, "weather_code") : NULL;
    const cJSON *tmax  = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : NULL;
    const cJSON *tmin  = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : NULL;
    const cJSON *rise  = daily ? cJSON_GetObjectItem(daily, "sunrise") : NULL;
    const cJSON *set   = daily ? cJSON_GetObjectItem(daily, "sunset") : NULL;

    if (!times || !codes || !tmax || !tmin) {
        ESP_LOGW(TAG, "Incomplete daily data");
        return false;
    }

    int count = cJSON_GetArraySize(times);
    if (count < 4) return false;
    if (count > WEATHER_DAYS) count = WEATHER_DAYS;

    for (int i = 0; i < count; i++) {
        double t = 0, c = 0, hi = 0, lo = 0, sr = 0, ss = 0;
        if (!arr_num(times, i, &t) || !arr_num(codes, i, &c) || !arr_num(tmax, i, &hi) || !arr_num(tmin, i, &lo)) {
            return false;
        }
        time_t date = (time_t)t;
        struct tm lt;
        localtime_r(&date, &lt);

        WeatherDay *d = &out->days[i];
        d->date = date;
        d->day = lt.tm_mday;
        d->month = lt.tm_mon + 1;
        d->weekday = lt.tm_wday;
        d->weathercode = round_i(c);
        d->temp_max = round_i(hi);
        d->temp_min = round_i(lo);
        d->sunrise = (rise && arr_num(rise, i, &sr)) ? (time_t)sr : 0;
        d->sunset = (set && arr_num(set, i, &ss)) ? (time_t)ss : 0;
    }
    /* Pad missing days (if API returned only 4) with an invalid date. */
    for (int i = count; i < WEATHER_DAYS; i++) {
        memset(&out->days[i], 0, sizeof(out->days[i]));
    }
    return true;
}

static void parse_hourly(const cJSON *root, WeatherData *out, time_t now)
{
    out->hour_count = 0;

    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!hourly) return;
    const cJSON *times = cJSON_GetObjectItem(hourly, "time");
    const cJSON *temp  = cJSON_GetObjectItem(hourly, "temperature_2m");
    const cJSON *feels = cJSON_GetObjectItem(hourly, "apparent_temperature");
    const cJSON *rh    = cJSON_GetObjectItem(hourly, "relative_humidity_2m");
    const cJSON *code  = cJSON_GetObjectItem(hourly, "weather_code");
    const cJSON *wind  = cJSON_GetObjectItem(hourly, "wind_speed_10m");
    const cJSON *dir   = cJSON_GetObjectItem(hourly, "wind_direction_10m");
    const cJSON *pp    = cJSON_GetObjectItem(hourly, "precipitation_probability");
    const cJSON *press = cJSON_GetObjectItem(hourly, "pressure_msl");
    if (!times || !temp || !feels || !rh || !code || !wind || !dir) return;

    int n = cJSON_GetArraySize(times);
    for (int i = 0; i < n && out->hour_count < WEATHER_HOURS; i++) {
        double t, a, b, c, d, e, f, g = 0, p = 0;
        if (!arr_num(times, i, &t)) continue;
        if ((time_t)t + 3600 <= now) continue;          /* already in the past */
        if (!arr_num(temp, i, &a) || !arr_num(feels, i, &b) || !arr_num(rh, i, &c) ||
            !arr_num(code, i, &d) || !arr_num(wind, i, &e) || !arr_num(dir, i, &f)) continue;
        bool pp_ok = pp && arr_num(pp, i, &g);
        bool press_ok = press && arr_num(press, i, &p);

        WeatherHour *h = &out->hours[out->hour_count++];
        h->time = (time_t)t;
        fill_hour_from_values(h, a, b, c, d, e, f, g, pp_ok, p, press_ok);
    }
}

static void parse_current(const cJSON *root, WeatherData *out)
{
    out->current_valid = false;
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (!cur) return;

    double t, a, b, c, d, e, f, g = 0, p = 0;
    if (!obj_num(cur, "time", &t) || !obj_num(cur, "temperature_2m", &a) ||
        !obj_num(cur, "apparent_temperature", &b) || !obj_num(cur, "relative_humidity_2m", &c) ||
        !obj_num(cur, "weather_code", &d) || !obj_num(cur, "wind_speed_10m", &e) ||
        !obj_num(cur, "wind_direction_10m", &f)) return;
    bool press_ok = obj_num(cur, "pressure_msl", &p);

    out->current.time = (time_t)t;
    fill_hour_from_values(&out->current, a, b, c, d, e, f, g, false, p, press_ok);
    out->current_valid = true;
}

static bool weather_fetch_once(WeatherData *out)
{
    if (!s_response) {
        s_response = heap_caps_malloc(RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_response) s_response = malloc(RESPONSE_BUF_SIZE);
        if (!s_response) {
            ESP_LOGE(TAG, "No memory for response buffer");
            return false;
        }
    }

    char url[640];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
        "&hourly=" HOURLY_FIELDS
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,"
        "wind_speed_10m,wind_direction_10m,pressure_msl"
        "&timezone=auto&timeformat=unixtime&forecast_days=%d&forecast_hours=%d",
        WEATHER_LATITUDE, WEATHER_LONGITUDE, WEATHER_DAYS, WEATHER_HOURS);

    s_response[0] = '\0';
    HttpBuffer hb = { .buf = s_response, .len = 0, .cap = RESPONSE_BUF_SIZE, .overflow = false };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size_tx = 1024,       /* long URL */
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "esp_http_client_init failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Failed to fetch weather: %s, status %d", esp_err_to_name(err), status);
        return false;
    }
    if (hb.overflow) {
        ESP_LOGW(TAG, "Weather response larger than %d bytes", RESPONSE_BUF_SIZE);
        return false;
    }
    ESP_LOGI(TAG, "Weather response: %d bytes", hb.len);

    cJSON *root = cJSON_Parse(s_response);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse weather JSON");
        return false;
    }

    time_t now = time(NULL);
    bool ok = parse_daily(root, out);
    if (ok) {
        parse_hourly(root, out, now);
        parse_current(root, out);
        out->fetched_at = now;
        ESP_LOGI(TAG, "Parsed: %d hourly entries, current %s", out->hour_count, out->current_valid ? "yes" : "no");
    }
    cJSON_Delete(root);
    return ok;
}

bool Weather_Fetch(WeatherData *out)
{
    const int max_attempts = 2;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (weather_fetch_once(out)) {
            return true;
        }
        if (attempt < max_attempts) {
            ESP_LOGW(TAG, "Retrying weather fetch (attempt %d/%d)...", attempt + 1, max_attempts);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    return false;
}

const WeatherHour *Weather_FindHour(const WeatherData *w, time_t now)
{
    for (int i = 0; i < w->hour_count; i++) {
        if (now >= w->hours[i].time && now < w->hours[i].time + 3600) {
            return &w->hours[i];
        }
    }
    if (w->current_valid && now >= w->current.time && now < w->current.time + 90 * 60) {
        return &w->current;
    }
    return NULL;
}

int Weather_FirstDayIndex(const WeatherData *w, time_t now)
{
    for (int i = 0; i < WEATHER_DAYS; i++) {
        if (w->days[i].date == 0) continue;
        /* day entry covers [date, date + 24h) - DST days are 23/25 h, 1 h slack is fine here */
        if (now < w->days[i].date + 24 * 3600) return i;
    }
    return -1;
}

const char *Weather_CodeText(int code)
{
    switch (code) {
        case 0:  return "Clear sky";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: return "Light drizzle";
        case 53: return "Drizzle";
        case 55: return "Dense drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light showers";
        case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Storm with hail";
        default: return "Unknown";
    }
}
