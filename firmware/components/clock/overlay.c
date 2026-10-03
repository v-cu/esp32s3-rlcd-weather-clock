#include "overlay.h"
#include "pages_internal.h"
#include "images.h"

#include <stdio.h>

#define PANEL_X  0
#define PANEL_Y  36

/* Pages are created when shown and deleted when hidden, so only one page lives in the
 * LVGL heap at a time (LV_MEM_SIZE is only 64 kB). */
typedef struct {
    lv_obj_t *(*create)(lv_obj_t *panel);
    void (*reload)(void);
} PageEntry;

static const PageEntry s_pages[OVERLAY_DIAG + 1] = {
    [OVERLAY_NONE]    = { NULL, NULL },
    [OVERLAY_OUTDOOR] = { PageOutdoor_Create, PageOutdoor_Reload },
    [OVERLAY_SUN]     = { PageSun_Create,     PageSun_Reload },
    [OVERLAY_INDOOR]  = { PageIndoor_Create,  PageIndoor_Reload },
    [OVERLAY_BATTERY] = { PageBattery_Create, PageBattery_Reload },
    [OVERLAY_DIAG]    = { PageDiag_Create,    PageDiag_Reload },
};

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_page_obj = NULL;
static OverlayPage s_current = OVERLAY_NONE;

/* ---- shared helpers ------------------------------------------------------------------------ */

lv_obj_t *Page_CreateContainer(lv_obj_t *panel)
{
    lv_obj_t *c = lv_obj_create(panel);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_size(c, PAGE_W, PAGE_H);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    return c;
}

lv_obj_t *Page_Label(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, "");
    return l;
}

/* Monochrome panel: only pure black / white (flush thresholds RGB565 at 0x7FFF). */
void Page_StyleChart(lv_obj_t *c)
{
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);

    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_line_color(c, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(c, 2, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(c, 4, LV_PART_MAIN);

    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(c, false, LV_PART_ITEMS);
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
}

void Page_FormatX10(char *buf, size_t len, int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    snprintf(buf, len, "%s%d.%d", v < 0 ? "-" : "", (int)(a / 10), (int)(a % 10));
}

lv_obj_t *Page_DashedHLine(lv_obj_t *parent, lv_point_precise_t pts[2], int x1, int x2, int y)
{
    pts[0].x = x1; pts[0].y = y;
    pts[1].x = x2; pts[1].y = y;
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, 2);
    lv_obj_set_style_line_color(l, lv_color_black(), 0);
    lv_obj_set_style_line_width(l, 1, 0);
    lv_obj_set_style_line_dash_width(l, 2, 0);
    lv_obj_set_style_line_dash_gap(l, 4, 0);
    return l;
}

const lv_image_dsc_t *Overlay_WeatherIcon(int code)
{
    if (code == 95 || code == 96 || code == 99) return &img_icon_storm;
    if (code == 0) return &img_icon_sun;
    if (code == 1 || code == 2) return &img_icon_partly_cloudy;
    if (code == 3 || (code >= 45 && code <= 48)) return &img_icon_cloud;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return &img_icon_rain;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return &img_icon_snow;
    return &img_icon_cloud;
}

/* ---- overlay ------------------------------------------------------------------------------- */

void Overlay_Create(lv_obj_t *main_screen)
{
    if (s_panel) return;

    s_panel = lv_obj_create(main_screen);
    lv_obj_set_pos(s_panel, PANEL_X, PANEL_Y);
    lv_obj_set_size(s_panel, PAGE_W, PAGE_H);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void Overlay_Show(OverlayPage page)
{
    if (!s_panel) return;

    if (s_page_obj && page != s_current) {
        lv_obj_delete(s_page_obj);
        s_page_obj = NULL;
    }

    s_current = page;
    if (page == OVERLAY_NONE) {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (!s_page_obj) {
        s_page_obj = s_pages[page].create(s_panel);
    }
    if (s_pages[page].reload) s_pages[page].reload();
    lv_obj_move_foreground(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

OverlayPage Overlay_Current(void)
{
    return s_current;
}

void Overlay_Reload(void)
{
    if (s_current != OVERLAY_NONE && s_page_obj && s_pages[s_current].reload) {
        s_pages[s_current].reload();
    }
}

OverlayPage Overlay_NextBootPage(OverlayPage current)
{
    switch (current) {
        case OVERLAY_NONE:    return OVERLAY_OUTDOOR;
        case OVERLAY_OUTDOOR: return OVERLAY_SUN;
        case OVERLAY_SUN:     return OVERLAY_INDOOR;
        case OVERLAY_INDOOR:  return OVERLAY_BATTERY;
        case OVERLAY_BATTERY: return OVERLAY_NONE;
        case OVERLAY_DIAG:    return OVERLAY_NONE;
    }
    return OVERLAY_NONE;
}
