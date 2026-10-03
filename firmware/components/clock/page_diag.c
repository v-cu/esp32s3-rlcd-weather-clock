#include "pages_internal.h"
#include "app_state.h"
#include "wifi_sync.h"
#include "battery.h"
#include "user_config.h"

#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_FW_VERSION
#define APP_FW_VERSION "dev"
#endif

#define DIAG_ROWS    9
#define DIAG_ROW_H   16
#define DIAG_COL_W   198

static lv_obj_t *s_left[DIAG_ROWS];
static lv_obj_t *s_right[DIAG_ROWS];

static const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "ext. pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "int. WDT";
        case ESP_RST_TASK_WDT:  return "task WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_USB:       return "USB";
        default:                return "other";
    }
}

static const char *rssi_text(int rssi)
{
    if (rssi >= -55) return "excellent";
    if (rssi >= -67) return "good";
    if (rssi >= -75) return "fair";
    if (rssi >= -82) return "weak";
    return "very weak";
}

lv_obj_t *PageDiag_Create(lv_obj_t *panel)
{
    lv_obj_t *page = Page_CreateContainer(panel);

    for (int i = 0; i < DIAG_ROWS; i++) {
        s_left[i]  = Page_Label(page, &lv_font_montserrat_14, 4, 2 + i * DIAG_ROW_H, DIAG_COL_W - 6, LV_TEXT_ALIGN_LEFT);
        s_right[i] = Page_Label(page, &lv_font_montserrat_14, DIAG_COL_W + 6, 2 + i * DIAG_ROW_H, PAGE_W - DIAG_COL_W - 8,
                                LV_TEXT_ALIGN_LEFT);
    }

    /* vertical separator */
    static lv_point_precise_t sep[2] = { { DIAG_COL_W, 2 }, { DIAG_COL_W, PAGE_H - 3 } };
    lv_obj_t *l = lv_line_create(page);
    lv_line_set_points(l, sep, 2);
    lv_obj_set_style_line_color(l, lv_color_black(), 0);
    lv_obj_set_style_line_width(l, 1, 0);
    lv_obj_set_style_line_dash_width(l, 2, 0);
    lv_obj_set_style_line_dash_gap(l, 3, 0);
    return page;
}

static void fmt_hm(char *buf, size_t len, time_t t)
{
    if (t == 0) { snprintf(buf, len, "--:--"); return; }
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void PageDiag_Reload(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int64_t up = esp_timer_get_time() / 1000000LL;
    int mv = g_app.battery_mv;
    char t1[16];

    /* ---- left: device ---- */
    lv_label_set_text_fmt(s_left[0], "FW %s", APP_FW_VERSION);
    lv_label_set_text_fmt(s_left[1], "Built %.11s", app ? app->date : "?");
    lv_label_set_text_fmt(s_left[2], "Uptime %dd %02dh %02dm",
                          (int)(up / 86400), (int)((up % 86400) / 3600), (int)((up % 3600) / 60));
    lv_label_set_text_fmt(s_left[3], "Reset: %s", reset_reason_text(esp_reset_reason()));
    lv_label_set_text_fmt(s_left[4], "RAM %uk (min %uk)",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text_fmt(s_left[5], "PSRAM %uk free",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text_fmt(s_left[6], "Battery %d.%02d V (%d%%)", mv / 1000, (mv % 1000) / 10,
                          Battery_VoltageToPercent(mv));
    lv_label_set_text_fmt(s_left[7], "Sensor %s, %u err", g_app.sensor_ok ? "OK" : "FAIL",
                          (unsigned)g_app.sensor_errors);
    fmt_hm(t1, sizeof(t1), g_app.last_sync_ok);
    lv_label_set_text_fmt(s_left[8], "Last OK %s, NTP %s", t1, g_app.last_time_sync_ok ? "OK" : "fail");

    /* ---- right: Wi-Fi / sync ---- */
    WifiSyncInfo wi;
    WifiSync_GetInfo(&wi);

    if (wi.valid) {
        lv_label_set_text_fmt(s_right[0], "WiFi %s", wi.ssid);
        lv_label_set_text_fmt(s_right[1], "RSSI %d dBm (%s)", wi.rssi, rssi_text(wi.rssi));
        lv_label_set_text_fmt(s_right[2], "Channel %u", (unsigned)wi.channel);
        lv_label_set_text_fmt(s_right[3], "%02X:%02X:%02X:%02X:%02X:%02X",
                              wi.bssid[0], wi.bssid[1], wi.bssid[2], wi.bssid[3], wi.bssid[4], wi.bssid[5]);
        lv_label_set_text_fmt(s_right[4], "IP %u.%u.%u.%u",
                              (unsigned)(wi.ip & 0xFF), (unsigned)((wi.ip >> 8) & 0xFF),
                              (unsigned)((wi.ip >> 16) & 0xFF), (unsigned)((wi.ip >> 24) & 0xFF));
        lv_label_set_text_fmt(s_right[5], "Connect %u.%u s, %d %s", (unsigned)(wi.connect_ms / 1000),
                              (unsigned)((wi.connect_ms % 1000) / 100), wi.attempts, wi.attempts == 1 ? "try" : "tries");
    } else {
        lv_label_set_text(s_right[0], "WiFi: never connected");
        for (int i = 1; i <= 5; i++) lv_label_set_text(s_right[i], "");
    }

    lv_label_set_text_fmt(s_right[6], "Sync OK %u / fail %u", (unsigned)g_app.sync_ok_count,
                          (unsigned)g_app.sync_fail_count);
    if (g_app.last_attempt_failed) {
        lv_label_set_text_fmt(s_right[7], "Err: %s", g_app.last_error);
    } else {
        lv_label_set_text_fmt(s_right[7], "WiFi conn %u ok / %u fail", (unsigned)wi.ok_count, (unsigned)wi.fail_count);
    }
    fmt_hm(t1, sizeof(t1), g_app.next_sync);
    lv_label_set_text_fmt(s_right[8], "Next sync %s", t1);
}
