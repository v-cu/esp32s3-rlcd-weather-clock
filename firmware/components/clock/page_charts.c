#include "pages_internal.h"
#include "env_history.h"
#include "app_state.h"
#include "battery.h"

#include <stdio.h>
#include <limits.h>

/* Layout shared by both chart pages (page-relative coordinates, page = 400 x 151). */
#define LEFT_W     40
#define CHART_X    44
#define CHART_W    354
#define AXIS_Y     134

/* ============================================================================================ */
/*  Common                                                                                      */
/* ============================================================================================ */

typedef struct {
    lv_obj_t *chart;
    lv_chart_series_t *ser;
    lv_obj_t *lbl_max;
    lv_obj_t *lbl_min;
    lv_obj_t *lbl_unit;
} ChartRow;

static void make_row(lv_obj_t *page, ChartRow *row, int y, int h, int vdiv, const char *unit, uint32_t points)
{
    lv_obj_t *c = lv_chart_create(page);
    row->chart = c;
    lv_obj_set_pos(c, CHART_X, y);
    lv_obj_set_size(c, CHART_W, h);
    Page_StyleChart(c);
    lv_chart_set_point_count(c, points);
    lv_chart_set_div_line_count(c, 3, vdiv);

    row->ser = lv_chart_add_series(c, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(c, row->ser, LV_CHART_POINT_NONE);

    row->lbl_max  = Page_Label(page, &lv_font_montserrat_16, 0, y - 1, LEFT_W, LV_TEXT_ALIGN_RIGHT);
    row->lbl_unit = Page_Label(page, &lv_font_montserrat_14, 0, y + (h - 16) / 2, LEFT_W, LV_TEXT_ALIGN_RIGHT);
    row->lbl_min  = Page_Label(page, &lv_font_montserrat_16, 0, y + h - 18, LEFT_W, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(row->lbl_unit, unit);
}

static void make_axis(lv_obj_t *page, const char *const *txt, int n)
{
    for (int i = 0; i < n; i++) {
        int x_line = CHART_X + (CHART_W - 1) * i / (n - 1);
        int w = 36;
        int x = x_line - w / 2;
        lv_text_align_t align = LV_TEXT_ALIGN_CENTER;
        if (i == 0)     { x = CHART_X; align = LV_TEXT_ALIGN_LEFT; }
        if (i == n - 1) { x = CHART_X + CHART_W - w; align = LV_TEXT_ALIGN_RIGHT; }
        lv_obj_t *l = Page_Label(page, &lv_font_montserrat_12, x, AXIS_Y, w, align);
        lv_label_set_text(l, txt[i]);
    }
}

typedef enum { FMT_X10, FMT_X10_INT, FMT_MV_VOLT } ValueFmt;

static void format_value(char *buf, size_t len, int32_t v, ValueFmt fmt)
{
    switch (fmt) {
        case FMT_X10:     Page_FormatX10(buf, len, v); break;
        case FMT_X10_INT: snprintf(buf, len, "%d", (int)((v + 5) / 10)); break;
        case FMT_MV_VOLT: snprintf(buf, len, "%d.%02d", (int)(v / 1000), (int)((v % 1000) / 10)); break;
    }
}

/* Axis range around data min/max with a minimum span so sensor noise is not magnified. */
static void fill_row(ChartRow *row, int32_t *data, uint32_t n, int32_t min_span, ValueFmt fmt)
{
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    for (uint32_t i = 0; i < n; i++) {
        if (data[i] == LV_CHART_POINT_NONE) continue;
        if (data[i] < mn) mn = data[i];
        if (data[i] > mx) mx = data[i];
    }

    if (mn == INT32_MAX) {
        lv_label_set_text(row->lbl_max, "--");
        lv_label_set_text(row->lbl_min, "--");
        lv_chart_set_all_values(row->chart, row->ser, LV_CHART_POINT_NONE);
        return;
    }

    int32_t lo = mn, hi = mx;
    int32_t span = hi - lo;
    if (span < min_span) {
        int32_t extra = min_span - span;
        lo -= extra / 2;
        hi += extra - extra / 2;
        span = min_span;
    }
    int32_t pad = span / 12 + 1;   /* keep the 2 px line inside the frame */
    lv_chart_set_axis_range(row->chart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
    lv_chart_set_series_values(row->chart, row->ser, data, n);

    char buf[16];
    format_value(buf, sizeof(buf), mx, fmt);
    lv_label_set_text(row->lbl_max, buf);
    format_value(buf, sizeof(buf), mn, fmt);
    lv_label_set_text(row->lbl_min, buf);
    lv_chart_refresh(row->chart);
}

/* ============================================================================================ */
/*  Indoor temperature / humidity, 24 h                                                         */
/* ============================================================================================ */

#define IN_ROW0_Y   2
#define IN_ROW1_Y   70
#define IN_ROW_H    62

static ChartRow s_temp, s_hum;
static lv_obj_t *s_in_no_data;
static int32_t s_temp_buf[ENV_HISTORY_POINTS];
static int32_t s_hum_buf[ENV_HISTORY_POINTS];

lv_obj_t *PageIndoor_Create(lv_obj_t *panel)
{
    lv_obj_t *page = Page_CreateContainer(panel);

    make_row(page, &s_temp, IN_ROW0_Y, IN_ROW_H, 5, "\xC2\xB0" "C", ENV_HISTORY_POINTS);
    make_row(page, &s_hum, IN_ROW1_Y, IN_ROW_H, 5, "%RH", ENV_HISTORY_POINTS);

    static const char *const axis[5] = { "-24h", "-18h", "-12h", "-6h", "now" };
    make_axis(page, axis, 5);

    /* Shown in the still empty left part of the upper chart during the first hours. */
    s_in_no_data = Page_Label(page, &lv_font_montserrat_14, CHART_X + 8, IN_ROW0_Y + (IN_ROW_H - 18) / 2, 190,
                              LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_in_no_data, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_in_no_data, LV_OPA_COVER, 0);
    lv_label_set_text(s_in_no_data, "Collecting data...");
    return page;
}

void PageIndoor_Reload(void)
{
    int valid = EnvHistory_GetSeries(s_temp_buf, s_hum_buf, LV_CHART_POINT_NONE);
    fill_row(&s_temp, s_temp_buf, ENV_HISTORY_POINTS, 20, FMT_X10);      /* min. 2.0 C span */
    fill_row(&s_hum, s_hum_buf, ENV_HISTORY_POINTS, 100, FMT_X10_INT);   /* min. 10 % span */

    if (valid < 18) lv_obj_remove_flag(s_in_no_data, LV_OBJ_FLAG_HIDDEN);   /* < 3 h of data */
    else            lv_obj_add_flag(s_in_no_data, LV_OBJ_FLAG_HIDDEN);
}

/* ============================================================================================ */
/*  Battery voltage, 7 days                                                                     */
/* ============================================================================================ */

#define BAT_INFO_Y   0
#define BAT_CHART_Y  21
#define BAT_CHART_H  108

static ChartRow s_bat;
static lv_obj_t *s_bat_info;
static lv_obj_t *s_bat_no_data;
static int32_t s_bat_buf[BAT_HISTORY_POINTS];

lv_obj_t *PageBattery_Create(lv_obj_t *panel)
{
    lv_obj_t *page = Page_CreateContainer(panel);

    s_bat_info = Page_Label(page, &lv_font_montserrat_14, 4, BAT_INFO_Y, PAGE_W - 8, LV_TEXT_ALIGN_CENTER);
    make_row(page, &s_bat, BAT_CHART_Y, BAT_CHART_H, 8, "V", BAT_HISTORY_POINTS);   /* grid line per day */

    static const char *const axis[8] = { "-7d", "-6d", "-5d", "-4d", "-3d", "-2d", "-1d", "now" };
    make_axis(page, axis, 8);

    s_bat_no_data = Page_Label(page, &lv_font_montserrat_14, CHART_X + 8, BAT_CHART_Y + (BAT_CHART_H - 18) / 2, 200,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_bat_no_data, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bat_no_data, LV_OPA_COVER, 0);
    lv_label_set_text(s_bat_no_data, "Collecting data (7 days)...");
    return page;
}

/* Trend over the discharge segment after the last charge (last hourly rise > 15 mV).
 * Returns mV per day; *ok false when there is not enough data (< 12 h). */
static float battery_trend(const int32_t *mv, int n, bool *ok)
{
    *ok = false;
    int start = 0;
    int32_t prev = LV_CHART_POINT_NONE;
    for (int i = 0; i < n; i++) {
        if (mv[i] == LV_CHART_POINT_NONE) continue;
        if (prev != LV_CHART_POINT_NONE && mv[i] - prev > 15) start = i;   /* still charging here */
        prev = mv[i];
    }

    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0, first = -1, last = -1;
    for (int i = start; i < n; i++) {
        if (mv[i] == LV_CHART_POINT_NONE) continue;
        double x = (double)i, y = (double)mv[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
        cnt++;
        if (first < 0) first = i;
        last = i;
    }
    if (cnt < 12 || (last - first) < 12) return 0.0f;

    double den = cnt * sxx - sx * sx;
    if (den == 0.0) return 0.0f;
    double slope_per_h = (cnt * sxy - sx * sy) / den;    /* one point = 1 h */
    *ok = true;
    return (float)(slope_per_h * 24.0);
}

static bool battery_charging(const int32_t *mv, int n)
{
    /* Rising by > 20 mV over the last 3 hours. */
    int32_t now_v = mv[n - 1];
    if (now_v == LV_CHART_POINT_NONE || n < 4 || mv[n - 4] == LV_CHART_POINT_NONE) return false;
    return (now_v - mv[n - 4]) > 20;
}

void PageBattery_Reload(void)
{
    int valid = EnvHistory_GetBattery(s_bat_buf, LV_CHART_POINT_NONE);
    fill_row(&s_bat, s_bat_buf, BAT_HISTORY_POINTS, 100, FMT_MV_VOLT);     /* min. 0.1 V span */

    int mv = g_app.battery_mv;
    int pct = Battery_VoltageToPercent(mv);
    char trend[56];
    bool ok = false;
    float per_day = battery_trend(s_bat_buf, BAT_HISTORY_POINTS, &ok);

    if (battery_charging(s_bat_buf, BAT_HISTORY_POINTS)) {
        snprintf(trend, sizeof(trend), "Charging");
    } else if (!ok) {
        snprintf(trend, sizeof(trend), "Trend: need 12 h of data");
    } else if (per_day > -3.0f) {
        snprintf(trend, sizeof(trend), "Trend: stable (%+d mV/day)", (int)per_day);
    } else if (mv <= APP_BATTERY_WARNING_MV) {
        snprintf(trend, sizeof(trend), "%d mV/day - charge now", (int)per_day);
    } else {
        float days = (float)(mv - APP_BATTERY_WARNING_MV) / -per_day;
        if (days > 99.0f) days = 99.0f;
        snprintf(trend, sizeof(trend), "%d mV/day, ~%d days left", (int)per_day, (int)(days + 0.5f));
    }

    lv_label_set_text_fmt(s_bat_info, "Now %d.%02d V (%d%%)   %s", mv / 1000, (mv % 1000) / 10, pct, trend);

    if (valid < 12) lv_obj_remove_flag(s_bat_no_data, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_bat_no_data, LV_OBJ_FLAG_HIDDEN);
}
