#include "clock_task.h"
#include "clock_config.h"
#include "pcf85063.h"
#include "shtc3.h"
#include "battery.h"
#include "weather.h"
#include "wifi_sync.h"
#include "env_history.h"
#include "overlay.h"
#include "app_state.h"
#include "lvgl_bsp.h"
#include "screens.h"
#include "ui.h"
#include "user_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/time.h>

/* ---- Scheduling ---------------------------------------------------------------------------- */
#define SYNC_HOUR_1              5
#define SYNC_HOUR_2              15
#define SYNC_WIFI_TIMEOUT_MS     30000   /* whole connect phase incl. retries */
#define SYNC_NTP_TIMEOUT_MS      10000
#define SYNC_RETRY_FIRST_MIN     10      /* after a failed sync: retry in 10, 20, 40, 60, 60... min */
#define SYNC_RETRY_MAX_MIN       60
#define BOOT_RETRY_NO_TIME_SEC   60      /* no WiFi and RTC invalid at boot */

/* ---- UI ------------------------------------------------------------------------------------ */
#define PAGE_TIMEOUT_MS          10000   /* BOOT pages return to the clock after 10 s */
#define DIAG_TIMEOUT_MS          30000
#define KEY_LONG_PRESS_MS        3000
#define BUTTON_DEBOUNCE_MS       50

#define RTC_MIN_VALID_YEAR       2025

static const char *TAG = "ClockTask";
static const char *WEEKDAY_NAMES[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

AppState g_app;

static bool s_battery_warning_active = false;
static bool s_main_screen_active = false;
static time_t s_next_retry = 0;
static int    s_retry_delay_min = SYNC_RETRY_FIRST_MIN;
static int64_t s_page_deadline_us = 0;   /* 0 = clock is shown */
static int    s_forecast_yday = -1;

/* ============================================================================================ */
/*  Time helpers                                                                                */
/* ============================================================================================ */

/* Reads the RTC (local time). Returns epoch too. */
static bool rtc_now(struct tm *out, time_t *epoch)
{
    struct tm t;
    if (Pcf85063_GetTime(&t) != ESP_OK) return false;
    t.tm_isdst = -1;                 /* let mktime decide CET/CEST */
    time_t e = mktime(&t);           /* also normalizes tm_wday / tm_yday / tm_isdst */
    if (out) *out = t;
    if (epoch) *epoch = e;
    return true;
}

static bool rtc_time_plausible(const struct tm *t)
{
    return (t->tm_year + 1900) >= RTC_MIN_VALID_YEAR;
}

static void set_system_time(time_t epoch)
{
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* Scheduled sync moments (05:00 / 15:00) - the latest one <= now, or the first one > now. */
static time_t sync_slot(time_t now, bool next)
{
    struct tm lt;
    localtime_r(&now, &lt);
    const int hours[2] = { SYNC_HOUR_1, SYNC_HOUR_2 };
    time_t best = 0;

    for (int day = -1; day <= 1; day++) {
        for (int i = 0; i < 2; i++) {
            struct tm c = lt;
            c.tm_mday += day;
            c.tm_hour = hours[i];
            c.tm_min = 0;
            c.tm_sec = 0;
            c.tm_isdst = -1;
            time_t ct = mktime(&c);
            if (!next && ct <= now && ct > best) best = ct;
            if (next && ct > now && (best == 0 || ct < best)) best = ct;
        }
    }
    return best;
}

static bool sync_due(time_t now)
{
    if (now < s_next_retry) return false;
    return g_app.last_sync_ok < sync_slot(now, false);
}

static void update_next_sync(time_t now)
{
    if (g_app.last_attempt_failed && s_next_retry > now) {
        g_app.next_sync = s_next_retry;
    } else if (sync_due(now)) {
        g_app.next_sync = now;
    } else {
        g_app.next_sync = sync_slot(now, true);
    }
}

/* ============================================================================================ */
/*  UI helpers (all take the LVGL lock)                                                         */
/* ============================================================================================ */

static void show_page(OverlayPage page);

static void show_battery_warning(void)
{
    show_page(OVERLAY_NONE);
    if (Lvgl_lock(-1)) {
        loadScreen(SCREEN_ID_INIT);
        lv_label_set_text(objects.info, "Battery low.\nPlease charge the device.");
        Lvgl_Refresh();
        Lvgl_unlock();
    }
    s_main_screen_active = false;
}

static void enter_battery_protection_shutdown(void)
{
    ESP_LOGE(TAG, "Battery critically low (<= %d mV) - putting device into permanent sleep.", APP_BATTERY_CRITICAL_MV);

    if (Lvgl_lock(-1)) {
        loadScreen(SCREEN_ID_INIT);
        lv_label_set_text(objects.info, "Battery critically low.\nCharge, then reset device.");
        Lvgl_Refresh();
        Lvgl_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_deep_sleep_start();
}

static void set_status(const char *text)
{
    if (Lvgl_lock(-1)) {
        lv_label_set_text(objects.info, text);
        Lvgl_Refresh();
        Lvgl_unlock();
    }
}

/* Progress shown while syncing: on the INIT screen as status, on the main screen
 * "Syncing..." replaces the date (top bar) and the bottom line shows the step. */
static void show_sync_progress(const char *step)
{
    if (!Lvgl_lock(-1)) return;
    if (!s_main_screen_active) {
        lv_label_set_text_fmt(objects.info, "Synchronizing...\n%s", step);
    } else {
        lv_label_set_text(objects.date, "Syncing...");
        lv_label_set_text_fmt(objects.sync, "Synchronizing: %s", step);
    }
    Lvgl_Refresh();
    Lvgl_unlock();
}

static void update_sync_label(void)
{
    if (!Lvgl_lock(-1)) return;

    if (g_app.last_attempt_failed) {
        struct tm fa, nr;
        localtime_r(&g_app.last_attempt, &fa);
        localtime_r(&s_next_retry, &nr);
        if (g_app.last_sync_ok > 0) {
            struct tm ok;
            localtime_r(&g_app.last_sync_ok, &ok);
            lv_label_set_text_fmt(objects.sync, "Sync failed %02d:%02d (%s), retry %02d:%02d, last OK %02d.%02d",
                                  fa.tm_hour, fa.tm_min, g_app.last_error, nr.tm_hour, nr.tm_min,
                                  ok.tm_mday, ok.tm_mon + 1);
        } else {
            lv_label_set_text_fmt(objects.sync, "Sync failed %02d:%02d (%s), retry at %02d:%02d",
                                  fa.tm_hour, fa.tm_min, g_app.last_error, nr.tm_hour, nr.tm_min);
        }
    } else if (g_app.last_sync_ok > 0) {
        struct tm t;
        localtime_r(&g_app.last_sync_ok, &t);
        int64_t uptime_sec = esp_timer_get_time() / 1000000LL;
        int uptime_days = (int)(uptime_sec / 86400);
        int uptime_hours = (int)((uptime_sec % 86400) / 3600);

        lv_label_set_text_fmt(objects.sync, "Last weather sync: %02d.%02d.%04d %02d:%02d  Uptime: %dd %dh",
                              t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
                              t.tm_hour, t.tm_min,
                              uptime_days, uptime_hours);
    } else {
        lv_label_set_text(objects.sync, "Weather not synced yet");
    }

    Lvgl_unlock();
}

/* Writes clock / sensor / battery labels. Caller refreshes the display. */
static void update_labels(const struct tm *t)
{
    if (!Lvgl_lock(-1)) return;

    char c[2] = { '0', '\0' };
    c[0] = (char)('0' + (t->tm_hour / 10) % 10);
    lv_label_set_text(objects.clock_hh1, c);
    c[0] = (char)('0' + t->tm_hour % 10);
    lv_label_set_text(objects.clock_hh2, c);
    c[0] = (char)('0' + (t->tm_min / 10) % 10);
    lv_label_set_text(objects.clock_mm1, c);
    c[0] = (char)('0' + t->tm_min % 10);
    lv_label_set_text(objects.clock_mm2, c);

    lv_label_set_text_fmt(objects.hum, "%d%%", (int)(g_app.humidity + 0.5f));
    lv_label_set_text_fmt(objects.date, "%02d.%02d.%04d", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);

    int mv = g_app.battery_mv;
    lv_label_set_text_fmt(objects.battery, "%d.%02d", mv / 1000, (mv % 1000) / 10);

    float temperature = g_app.temperature;
    bool temp_negative = temperature < 0.0f;
    float temp_abs = temp_negative ? -temperature : temperature;
    int temp_whole = (int)temp_abs;
    int temp_frac = (int)((temp_abs - temp_whole) * 10.0f + 0.5f);
    if (temp_frac >= 10) { temp_frac = 0; temp_whole += 1; }
    lv_label_set_text_fmt(objects.temp, "%s%d.%d°C", temp_negative ? "-" : "", temp_whole, temp_frac);

    Lvgl_unlock();
}

/* Bottom forecast row: 4 days starting from today (data has 5 days, so it stays correct
 * after midnight until the next sync). */
static void update_forecast_labels(void)
{
    if (!Lvgl_lock(-1)) return;

    lv_obj_t *date_objs[4] = { objects.day1_date, objects.day2_date, objects.day3_date, objects.day4_date };
    lv_obj_t *icon_objs[4] = { objects.day1_icon, objects.day2_icon, objects.day3_icon, objects.day4_icon };
    lv_obj_t *temp_objs[4] = { objects.day1_temp, objects.day2_temp, objects.day3_temp, objects.day4_temp };

    int first = g_app.weather_valid ? Weather_FirstDayIndex(&g_app.weather, g_app.now) : -1;

    for (int i = 0; i < 4; i++) {
        int di = (first < 0) ? -1 : first + i;
        const WeatherDay *d = (di >= 0 && di < WEATHER_DAYS && g_app.weather.days[di].date) ? &g_app.weather.days[di] : NULL;
        if (d) {
            lv_label_set_text_fmt(date_objs[i], "%d/%d %s", d->day, d->month, WEEKDAY_NAMES[d->weekday % 7]);
            lv_image_set_src(icon_objs[i], Overlay_WeatherIcon(d->weathercode));
            lv_label_set_text_fmt(temp_objs[i], "%d/%d°C", d->temp_max, d->temp_min);
            lv_obj_remove_flag(icon_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(date_objs[i], "--");
            lv_label_set_text(temp_objs[i], "--/--");
            lv_obj_add_flag(icon_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    Lvgl_unlock();
}

static void refresh_display(void)
{
    if (Lvgl_lock(-1)) {
        Lvgl_Refresh();
        Lvgl_unlock();
    }
}

/* Overlay pages in the clock area. */
static void show_page(OverlayPage page)
{
    if (Lvgl_lock(-1)) {
        Overlay_Show(page);
        Lvgl_Refresh();
        Lvgl_unlock();
    }
    if (page == OVERLAY_NONE) {
        s_page_deadline_us = 0;
    } else {
        int64_t timeout_ms = (page == OVERLAY_DIAG) ? DIAG_TIMEOUT_MS : PAGE_TIMEOUT_MS;
        s_page_deadline_us = esp_timer_get_time() + timeout_ms * 1000LL;
    }
    ESP_LOGI(TAG, "Overlay page %d", (int)page);
}

static bool page_visible(void)
{
    return s_page_deadline_us != 0;
}

/* ============================================================================================ */
/*  Sync                                                                                        */
/* ============================================================================================ */

typedef struct {
    bool wifi_ok;
    bool weather_ok;
    bool time_ok;
} SyncResult;

static WeatherData s_fresh_weather;      /* ~1.5 kB - keep it off the task stack */

static SyncResult do_sync(void)
{
    SyncResult r = { 0 };

    ESP_LOGI(TAG, "Sync start");
    show_sync_progress("connecting to WiFi...");
    r.wifi_ok = WifiSync_Connect(CLOCK_WIFI_SSID, CLOCK_WIFI_PASS, SYNC_WIFI_TIMEOUT_MS);

    if (r.wifi_ok) {
        /* NTP first: correct system time makes 'fetched_at' and the hourly window right. */
        show_sync_progress("setting time...");
        struct tm ntp_t;
        r.time_ok = WifiSync_SyncNtp(&ntp_t, SYNC_NTP_TIMEOUT_MS);
        if (r.time_ok) {
            Pcf85063_SetTime(&ntp_t);
        }

        show_sync_progress("downloading weather...");
        r.weather_ok = Weather_Fetch(&s_fresh_weather);
    }
    WifiSync_Disconnect();

    if (r.weather_ok) {
        g_app.weather = s_fresh_weather;
        g_app.weather_valid = true;
    }

    time_t now = 0;
    struct tm now_tm = {0};
    if (rtc_now(&now_tm, &now) && !r.time_ok && rtc_time_plausible(&now_tm)) {
        set_system_time(now);      /* keep system clock aligned with RTC */
    }
    g_app.now = now;
    g_app.last_attempt = now;
    if (r.wifi_ok) g_app.last_time_sync_ok = r.time_ok;

    if (r.weather_ok) {
        g_app.last_sync_ok = now;
        g_app.last_attempt_failed = false;
        g_app.last_error[0] = '\0';
        g_app.sync_ok_count++;
        g_app.consecutive_fails = 0;
        s_retry_delay_min = SYNC_RETRY_FIRST_MIN;
        s_next_retry = 0;
        ESP_LOGI(TAG, "Sync OK (time %s)", r.time_ok ? "updated" : "NOT updated");
    } else {
        const char *err = r.wifi_ok ? "weather download" : WifiSync_LastError();
        strncpy(g_app.last_error, err, sizeof(g_app.last_error) - 1);
        g_app.last_error[sizeof(g_app.last_error) - 1] = '\0';
        g_app.last_attempt_failed = true;
        g_app.sync_fail_count++;
        g_app.consecutive_fails++;
        s_next_retry = now + (time_t)s_retry_delay_min * 60;
        ESP_LOGW(TAG, "Sync FAILED (%s), next retry in %d min", g_app.last_error, s_retry_delay_min);
        s_retry_delay_min *= 2;
        if (s_retry_delay_min > SYNC_RETRY_MAX_MIN) s_retry_delay_min = SYNC_RETRY_MAX_MIN;
    }
    update_next_sync(now);

    update_forecast_labels();
    s_forecast_yday = now_tm.tm_yday;
    update_sync_label();
    return r;
}

/* ============================================================================================ */
/*  Sensors                                                                                     */
/* ============================================================================================ */

static void read_sensors(time_t now)
{
    float t, h;
    if (Shtc3_Read(&t, &h) == ESP_OK) {
        g_app.temperature = t;
        g_app.humidity = h;
        g_app.sensor_ok = true;
        EnvHistory_AddSample(now, t, h);
    } else {
        g_app.sensor_ok = false;
        g_app.sensor_errors++;
        ESP_LOGW(TAG, "Failed to read SHTC3, keeping previous values.");
    }

    int mv = 0;
    if (Battery_ReadVoltageMv(&mv) == ESP_OK && mv > 0) {
        g_app.battery_mv = mv;
        EnvHistory_AddBattery(now, mv);
    }
}

/* ============================================================================================ */
/*  Buttons                                                                                     */
/* ============================================================================================ */

static void configure_buttons_wakeup(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << KEY_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_wakeup_enable(KEY_BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BOOT_BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

static void wait_button_release(gpio_num_t pin)
{
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
}

/* KEY: short press = sync, hold 3 s = diagnostics (shown while still holding).
 * Returns true for a short press. */
static bool handle_key_press(void)
{
    int64_t t0 = esp_timer_get_time();
    bool long_press = false;

    while (gpio_get_level(KEY_BUTTON_PIN) == 0) {
        if (!long_press && (esp_timer_get_time() - t0) >= (int64_t)KEY_LONG_PRESS_MS * 1000LL) {
            long_press = true;
            ESP_LOGI(TAG, "KEY long press - diagnostics");
            if (s_main_screen_active) show_page(OVERLAY_DIAG);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    return !long_press;
}

/* ============================================================================================ */
/*  Task                                                                                        */
/* ============================================================================================ */

static void go_to_main_screen(void)
{
    if (Lvgl_lock(-1)) {
        loadScreen(SCREEN_ID_MAIN);
        Lvgl_Refresh();
        Lvgl_unlock();
    }
    s_main_screen_active = true;
}

static void clock_task(void *arg)
{
    set_status("Initializing...");

    WifiSync_Init();                 /* also sets TZ - must run before any mktime() */
    Pcf85063_Init((gpio_num_t)ESP32_I2C_SDA_PIN, (gpio_num_t)ESP32_I2C_SCL_PIN);
    Shtc3_Init(Pcf85063_GetBusHandle());
    Battery_Init();
    configure_buttons_wakeup();
    EnvHistory_Init();

    if (Lvgl_lock(-1)) {
        Overlay_Create(objects.main);
        Lvgl_unlock();
    }

    int startup_battery_mv = 0;
    Battery_ReadVoltageMv(&startup_battery_mv);
    g_app.battery_mv = startup_battery_mv;
    if (startup_battery_mv > 0 && startup_battery_mv <= APP_BATTERY_CRITICAL_MV) {
        enter_battery_protection_shutdown();
    }

    /* RTC keeps running on its backup supply - use it until NTP confirms. */
    struct tm t = {0};
    time_t now = 0;
    bool rtc_ok = rtc_now(&t, &now) && rtc_time_plausible(&t);
    if (rtc_ok) {
        set_system_time(now);
        g_app.now = now;
    }

    SyncResult sr = do_sync();

    /* Without WiFi AND without a valid RTC there is nothing sensible to show - keep retrying,
     * but never halt (e.g. router boots slower than the clock after a power outage). */
    while (!(rtc_now(&t, &now) && rtc_time_plausible(&t))) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Sync failed: %s\nRetrying in %d s...  (KEY = retry now)",
                 sr.wifi_ok ? "no time from NTP" : WifiSync_LastError(), BOOT_RETRY_NO_TIME_SEC);
        set_status(msg);
        for (int i = 0; i < BOOT_RETRY_NO_TIME_SEC * 10; i++) {
            if (gpio_get_level(KEY_BUTTON_PIN) == 0) {
                wait_button_release(KEY_BUTTON_PIN);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        sr = do_sync();
    }

    g_app.now = now;
    read_sensors(now);
    update_next_sync(now);
    update_labels(&t);
    update_forecast_labels();
    s_forecast_yday = t.tm_yday;
    update_sync_label();

    set_status("Starting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    go_to_main_screen();

    int64_t last_minute = (int64_t)now / 60;

    for (;;) {
        /* ---- Sleep until next minute boundary, page timeout or a button ---- */
        struct tm pre;
        uint64_t sleep_us = 60ULL * 1000000ULL;
        if (Pcf85063_GetTime(&pre) == ESP_OK) {
            int s = 60 - pre.tm_sec;
            if (s <= 0 || s > 60) s = 60;
            sleep_us = (uint64_t)s * 1000000ULL;
        }
        if (page_visible()) {
            int64_t left = s_page_deadline_us - esp_timer_get_time();
            if (left < 1000) left = 1000;
            if ((uint64_t)left < sleep_us) sleep_us = (uint64_t)left;
        }

        esp_sleep_enable_timer_wakeup(sleep_us);
        esp_light_sleep_start();

        if (rtc_now(&t, &now)) {
            g_app.now = now;
        }

        /* ---- Buttons ---- */
        bool boot_pressed = (gpio_get_level(BOOT_BUTTON_PIN) == 0);
        bool key_pressed  = (gpio_get_level(KEY_BUTTON_PIN) == 0);
        bool force_sync = false;

        if (boot_pressed) {
            wait_button_release(BOOT_BUTTON_PIN);
            if (s_main_screen_active) {
                OverlayPage cur = page_visible() ? Overlay_Current() : OVERLAY_NONE;
                show_page(Overlay_NextBootPage(cur));
            }
        }
        if (key_pressed) {
            force_sync = handle_key_press();
            if (force_sync) ESP_LOGI(TAG, "KEY short press - forcing sync...");
        }

        if (page_visible() && esp_timer_get_time() >= s_page_deadline_us) {
            show_page(OVERLAY_NONE);
        }

        if (!rtc_now(&t, &now)) {
            ESP_LOGW(TAG, "Failed to read RTC, skipping this cycle.");
            continue;
        }
        g_app.now = now;

        int64_t minute = (int64_t)now / 60;
        bool new_minute = (minute != last_minute);

        if (!new_minute && !force_sync) {
            continue;     /* woke only for a button / page timeout */
        }

        /* ---- Sync (manual or scheduled / retry) ---- */
        if (force_sync || sync_due(now)) {
            do_sync();
            if (!rtc_now(&t, &now)) continue;
            g_app.now = now;
            minute = (int64_t)now / 60;
        }

        if (new_minute) {
            read_sensors(now);
        }
        last_minute = minute;
        update_next_sync(now);

        int battery_mv = g_app.battery_mv;
        if (battery_mv > 0 && battery_mv <= APP_BATTERY_CRITICAL_MV) {
            enter_battery_protection_shutdown();
        }

        if (battery_mv > 0 && battery_mv <= APP_BATTERY_WARNING_MV) {
            show_battery_warning();
            s_battery_warning_active = true;
            continue;
        }

        if (s_battery_warning_active) {
            s_battery_warning_active = false;
            go_to_main_screen();
        }

        if (t.tm_yday != s_forecast_yday) {      /* midnight: shift forecast row to today */
            update_forecast_labels();
            s_forecast_yday = t.tm_yday;
        }
        update_labels(&t);
        update_sync_label();
        if (page_visible() && Lvgl_lock(-1)) {
            Overlay_Reload();
            Lvgl_unlock();
        }
        refresh_display();
    }
}

void ClockTask_Start(void)
{
    xTaskCreatePinnedToCore(clock_task, "ClockTask", 12 * 1024, NULL, 3, NULL, 1);
}
