#pragma once

/* Internal helpers shared by overlay.c and the page_*.c files. */

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_W   400
#define PAGE_H   151

/* Every page is a full-size white container, child of the overlay panel. */
lv_obj_t *Page_CreateContainer(lv_obj_t *panel);
lv_obj_t *Page_Label(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w, lv_text_align_t align);
void      Page_StyleChart(lv_obj_t *chart);
void      Page_FormatX10(char *buf, size_t len, int32_t v);
lv_obj_t *Page_DashedHLine(lv_obj_t *parent, lv_point_precise_t pts[2], int x1, int x2, int y);

/* Pages */
lv_obj_t *PageOutdoor_Create(lv_obj_t *panel);
void      PageOutdoor_Reload(void);

lv_obj_t *PageSun_Create(lv_obj_t *panel);
void      PageSun_Reload(void);

lv_obj_t *PageIndoor_Create(lv_obj_t *panel);
void      PageIndoor_Reload(void);

lv_obj_t *PageBattery_Create(lv_obj_t *panel);
void      PageBattery_Reload(void);

lv_obj_t *PageDiag_Create(lv_obj_t *panel);
void      PageDiag_Reload(void);

#ifdef __cplusplus
}
#endif
