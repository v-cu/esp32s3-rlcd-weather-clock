#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pages drawn over the big clock area of the main screen (y = 36..186).
 * Top bar and bottom forecast stay visible. All functions need the LVGL lock. */
typedef enum {
    OVERLAY_NONE = 0,       /* big clock */
    OVERLAY_OUTDOOR,        /* current outdoor weather   - BOOT #1 */
    OVERLAY_SUN,            /* sunrise / sunset          - BOOT #2 */
    OVERLAY_INDOOR,         /* indoor 24 h chart         - BOOT #3 */
    OVERLAY_BATTERY,        /* battery 7 days chart      - BOOT #4 */
    OVERLAY_DIAG,           /* diagnostics               - KEY held 3 s */
} OverlayPage;

void        Overlay_Create(lv_obj_t *main_screen);
void        Overlay_Show(OverlayPage page);       /* OVERLAY_NONE hides; data is reloaded */
OverlayPage Overlay_Current(void);
void        Overlay_Reload(void);                 /* refresh data of the visible page */

/* BOOT button sequence: clock -> outdoor -> sun -> indoor -> battery -> clock.
 * From diagnostics BOOT returns to the clock. */
OverlayPage Overlay_NextBootPage(OverlayPage current);

/* Weather icon for a WMO code (shared with the forecast row). */
const lv_image_dsc_t *Overlay_WeatherIcon(int code);

#ifdef __cplusplus
}
#endif
