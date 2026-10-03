#include "pages_internal.h"
#include "overlay.h"
#include "app_state.h"
#include "weather.h"

#include <stdio.h>
#include <math.h>

/* ============================================================================================ */
/*  Outdoor weather "now"                                                                       */
/*  Weather is downloaded twice a day, so "now" is taken from the hourly forecast for the       */
/*  current hour (48 h stored), not from the possibly hours-old "current" block.                */
/* ============================================================================================ */

#define OUT_STRIP_Y    95
#define OUT_CELLS      4
#define OUT_CELL_STEP  3      /* hours between cells: +3h, +6h, +9h, +12h */

static lv_obj_t *s_out_icon;
static lv_obj_t *s_out_temp;
static lv_obj_t *s_out_cond;
static lv_obj_t *s_out_feels;
static lv_obj_t *s_out_info[5];
static lv_obj_t *s_out_nodata;
static lv_obj_t *s_cell_icon[OUT_CELLS];
static lv_obj_t *s_cell_time[OUT_CELLS];
static lv_obj_t *s_cell_temp[OUT_CELLS];
static lv_point_precise_t s_out_line_pts[2];

static const char *wind_dir_text(int deg)
{
    static const char *const dirs[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    deg %= 360;
    if (deg < 0) deg += 360;
    return dirs[((deg * 2 + 45) / 90) % 8];
}

lv_obj_t *PageOutdoor_Create(lv_obj_t *panel)
{
    lv_obj_t *page = Page_CreateContainer(panel);

    s_out_icon = lv_image_create(page);
    lv_obj_set_pos(s_out_icon, 2, 2);
    lv_image_set_src(s_out_icon, Overlay_WeatherIcon(0));

    s_out_temp  = Page_Label(page, &lv_font_montserrat_48, 96, -4, 162, LV_TEXT_ALIGN_LEFT);
    s_out_cond  = Page_Label(page, &lv_font_montserrat_18, 98, 50, 160, LV_TEXT_ALIGN_LEFT);
    s_out_feels = Page_Label(page, &lv_font_montserrat_14, 98, 73, 160, LV_TEXT_ALIGN_LEFT);

    for (int i = 0; i < 5; i++) {
        s_out_info[i] = Page_Label(page, &lv_font_montserrat_14, 260, 2 + i * 18, 140, LV_TEXT_ALIGN_LEFT);
    }

    Page_DashedHLine(page, s_out_line_pts, 0, PAGE_W - 1, OUT_STRIP_Y - 2);

    for (int i = 0; i < OUT_CELLS; i++) {
        int x = i * 100;
        s_cell_icon[i] = lv_image_create(page);
        lv_obj_set_pos(s_cell_icon[i], x - 22, OUT_STRIP_Y - 19);   /* 90x90 scaled to ~46 px around centre */
        lv_image_set_scale(s_cell_icon[i], 120);
        lv_image_set_src(s_cell_icon[i], Overlay_WeatherIcon(0));
        s_cell_time[i] = Page_Label(page, &lv_font_montserrat_14, x + 50, OUT_STRIP_Y + 2, 50, LV_TEXT_ALIGN_LEFT);
        s_cell_temp[i] = Page_Label(page, &lv_font_montserrat_20, x + 50, OUT_STRIP_Y + 22, 50, LV_TEXT_ALIGN_LEFT);
    }

    s_out_nodata = Page_Label(page, &lv_font_montserrat_20, 0, 50, PAGE_W, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_out_nodata, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_out_nodata, LV_OPA_COVER, 0);
    lv_obj_set_height(s_out_nodata, 60);
    lv_label_set_text(s_out_nodata, "No outdoor data\nPress KEY to sync");
    return page;
}

void PageOutdoor_Reload(void)
{
    const WeatherData *w = &g_app.weather;
    const WeatherHour *h = g_app.weather_valid ? Weather_FindHour(w, g_app.now) : NULL;

    if (!h) {
        lv_obj_remove_flag(s_out_nodata, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_out_nodata, LV_OBJ_FLAG_HIDDEN);

    char buf[24];
    lv_image_set_src(s_out_icon, Overlay_WeatherIcon(h->code));
    Page_FormatX10(buf, sizeof(buf), h->temp_x10);
    lv_label_set_text_fmt(s_out_temp, "%s\xC2\xB0", buf);
    lv_label_set_text(s_out_cond, Weather_CodeText(h->code));
    Page_FormatX10(buf, sizeof(buf), h->feels_x10);
    lv_label_set_text_fmt(s_out_feels, "Feels like %s\xC2\xB0" "C", buf);

    lv_label_set_text_fmt(s_out_info[0], "Humidity %u%%", (unsigned)h->humidity);
    lv_label_set_text_fmt(s_out_info[1], "Wind %d km/h %s", (h->wind_x10 + 5) / 10, wind_dir_text(h->wind_dir));
    if (h->precip_prob != WEATHER_UNKNOWN_U8) {
        lv_label_set_text_fmt(s_out_info[2], "Rain %u%%", (unsigned)h->precip_prob);
    } else {
        lv_label_set_text(s_out_info[2], "Rain --");
    }
    if (h->pressure) lv_label_set_text_fmt(s_out_info[3], "%u hPa", (unsigned)h->pressure);
    else             lv_label_set_text(s_out_info[3], "-- hPa");

    struct tm ft;
    localtime_r(&w->fetched_at, &ft);
    lv_label_set_text_fmt(s_out_info[4], "Updated %02d:%02d", ft.tm_hour, ft.tm_min);

    /* Next hours */
    for (int i = 0; i < OUT_CELLS; i++) {
        time_t target = h->time + (time_t)(i + 1) * OUT_CELL_STEP * 3600;
        const WeatherHour *c = Weather_FindHour(w, target);
        struct tm tt;
        localtime_r(&target, &tt);
        lv_label_set_text_fmt(s_cell_time[i], "%02d:00", tt.tm_hour);
        if (c && c != &w->current) {
            lv_image_set_src(s_cell_icon[i], Overlay_WeatherIcon(c->code));
            lv_obj_remove_flag(s_cell_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_cell_temp[i], "%d\xC2\xB0", (c->temp_x10 >= 0 ? c->temp_x10 + 5 : c->temp_x10 - 5) / 10);
        } else {
            lv_obj_add_flag(s_cell_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_cell_temp[i], "--");
        }
    }
}

/* ============================================================================================ */
/*  Sunrise / sunset                                                                            */
/* ============================================================================================ */

#define SUN_CX        200
#define SUN_HORIZON   102
#define SUN_RX        100
#define SUN_RY        84
#define SUN_ARC_PTS   41
#define SUN_DOT       18

static lv_point_precise_t s_arc_pts[SUN_ARC_PTS];
static lv_point_precise_t s_horizon_pts[2];
static lv_obj_t *s_arc_done;
static lv_obj_t *s_arc_rest;
static lv_obj_t *s_sun_dot;
static lv_obj_t *s_rise_time;
static lv_obj_t *s_set_time;
static lv_obj_t *s_day_len;
static lv_obj_t *s_sun_next;
static lv_obj_t *s_sun_nodata;

static lv_obj_t *make_arc_line(lv_obj_t *page, int width, bool dashed)
{
    lv_obj_t *l = lv_line_create(page);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_color(l, lv_color_black(), 0);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    if (dashed) {
        lv_obj_set_style_line_dash_width(l, 3, 0);
        lv_obj_set_style_line_dash_gap(l, 3, 0);
    }
    return l;
}

lv_obj_t *PageSun_Create(lv_obj_t *panel)
{
    lv_obj_t *page = Page_CreateContainer(panel);

    for (int i = 0; i < SUN_ARC_PTS; i++) {
        double a = M_PI * (1.0 - (double)i / (SUN_ARC_PTS - 1));     /* 180 deg (east) -> 0 deg (west) */
        s_arc_pts[i].x = (lv_value_precise_t)(SUN_CX + SUN_RX * cos(a));
        s_arc_pts[i].y = (lv_value_precise_t)(SUN_HORIZON - SUN_RY * sin(a));
    }

    Page_DashedHLine(page, s_horizon_pts, SUN_CX - SUN_RX - 6, SUN_CX + SUN_RX + 6, SUN_HORIZON);

    s_arc_rest = make_arc_line(page, 1, false);
    s_arc_done = make_arc_line(page, 4, false);

    s_sun_dot = lv_obj_create(page);
    lv_obj_set_size(s_sun_dot, SUN_DOT, SUN_DOT);
    lv_obj_remove_flag(s_sun_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_sun_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_sun_dot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sun_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_sun_dot, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_sun_dot, 3, 0);
    lv_obj_set_style_pad_all(s_sun_dot, 0, 0);
    lv_obj_set_style_shadow_width(s_sun_dot, 0, 0);

    lv_obj_t *l = Page_Label(page, &lv_font_montserrat_16, 0, 44, 96, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(l, "Sunrise");
    s_rise_time = Page_Label(page, &lv_font_montserrat_30, 0, 64, 96, LV_TEXT_ALIGN_CENTER);

    l = Page_Label(page, &lv_font_montserrat_16, 304, 44, 96, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(l, "Sunset");
    s_set_time = Page_Label(page, &lv_font_montserrat_30, 304, 64, 96, LV_TEXT_ALIGN_CENTER);

    s_day_len  = Page_Label(page, &lv_font_montserrat_16, 0, 108, PAGE_W, LV_TEXT_ALIGN_CENTER);
    s_sun_next = Page_Label(page, &lv_font_montserrat_16, 0, 130, PAGE_W, LV_TEXT_ALIGN_CENTER);

    s_sun_nodata = Page_Label(page, &lv_font_montserrat_20, 0, 40, PAGE_W, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_sun_nodata, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_sun_nodata, LV_OPA_COVER, 0);
    lv_obj_set_height(s_sun_nodata, 111);
    lv_label_set_text(s_sun_nodata, "\nNo sunrise/sunset data\nPress KEY to sync");
    return page;
}

static void fmt_hm(char *buf, size_t len, time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

static void fmt_duration(char *buf, size_t len, long sec)
{
    if (sec < 0) sec = 0;
    long m = (sec + 30) / 60;
    snprintf(buf, len, "%ldh %02ldm", m / 60, m % 60);
}

void PageSun_Reload(void)
{
    const WeatherData *w = &g_app.weather;
    time_t now = g_app.now;
    int di = g_app.weather_valid ? Weather_FirstDayIndex(w, now) : -1;

    if (di < 0 || w->days[di].sunrise == 0 || w->days[di].sunset <= w->days[di].sunrise) {
        lv_obj_remove_flag(s_sun_nodata, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_sun_nodata, LV_OBJ_FLAG_HIDDEN);

    const WeatherDay *d = &w->days[di];
    const WeatherDay *tm_day = (di + 1 < WEATHER_DAYS && w->days[di + 1].sunrise) ? &w->days[di + 1] : NULL;
    char a[16], b[24];

    fmt_hm(a, sizeof(a), d->sunrise);
    lv_label_set_text(s_rise_time, a);
    fmt_hm(a, sizeof(a), d->sunset);
    lv_label_set_text(s_set_time, a);

    long len_today = (long)(d->sunset - d->sunrise);
    fmt_duration(b, sizeof(b), len_today);
    if (tm_day) {
        long diff_min = ((long)(tm_day->sunset - tm_day->sunrise) - len_today) / 60;
        lv_label_set_text_fmt(s_day_len, "Day length %s  (tomorrow %+ld min)", b, diff_min);
    } else {
        lv_label_set_text_fmt(s_day_len, "Day length %s", b);
    }

    /* Sun position along the arc */
    int k;
    if (now <= d->sunrise) {
        k = 0;
    } else if (now >= d->sunset) {
        k = SUN_ARC_PTS - 1;
    } else {
        double f = (double)(now - d->sunrise) / (double)(d->sunset - d->sunrise);
        k = (int)(f * (SUN_ARC_PTS - 1) + 0.5);
        double ang = M_PI * (1.0 - f);
        int sx = (int)(SUN_CX + SUN_RX * cos(ang));
        int sy = (int)(SUN_HORIZON - SUN_RY * sin(ang));
        lv_obj_set_pos(s_sun_dot, sx - SUN_DOT / 2, sy - SUN_DOT / 2);
    }
    bool daytime = (now > d->sunrise && now < d->sunset);

    if (daytime) {
        lv_obj_remove_flag(s_sun_dot, LV_OBJ_FLAG_HIDDEN);
        if (k < 1) k = 1;
        lv_line_set_points(s_arc_done, s_arc_pts, (uint32_t)(k + 1));
        lv_obj_remove_flag(s_arc_done, LV_OBJ_FLAG_HIDDEN);
        lv_line_set_points(s_arc_rest, &s_arc_pts[k], (uint32_t)(SUN_ARC_PTS - k));
        fmt_duration(b, sizeof(b), (long)(d->sunset - now));
        lv_label_set_text_fmt(s_sun_next, "Sunset in %s", b);
    } else {
        lv_obj_add_flag(s_sun_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_arc_done, LV_OBJ_FLAG_HIDDEN);
        lv_line_set_points(s_arc_rest, s_arc_pts, SUN_ARC_PTS);

        time_t next_rise = (now < d->sunrise) ? d->sunrise : (tm_day ? tm_day->sunrise : 0);
        if (next_rise) {
            fmt_hm(a, sizeof(a), next_rise);
            fmt_duration(b, sizeof(b), (long)(next_rise - now));
            lv_label_set_text_fmt(s_sun_next, "Night - next sunrise %s (in %s)", a, b);
        } else {
            lv_label_set_text(s_sun_next, "Night");
        }
    }
}
