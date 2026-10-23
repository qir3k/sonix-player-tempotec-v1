// board_profile.h -- the geometry of the TempoTec Variations V1's 240x320 panel.
//
// This fork runs on the V1 and nothing else, so the profile is not a choice any
// more: every number below is the V1's, and every function is inline and
// constant. That is what lets the compiler throw away the 480-pixel layouts the
// pages used to carry beside the compact ones -- `bp_pick(a, b)` folds to `a`,
// so the `b` branch is never emitted, and `bp_is_tempotec_v1()` is always true,
// so the regular-panel code behind it goes with it.
//
// The names are kept: they are read from fonts.c, main.c and a hundred pages,
// and renaming them would be a change with no behaviour in it. The host
// simulator builds through the same profile -- there is no 480-pixel mode to
// run any more, which is why `SONIX_PANEL=240x320` is the only useful value and
// where nothing at all is equivalent to it.

#ifndef SRC_GUI_BOARD_PROFILE_H_
#define SRC_GUI_BOARD_PROFILE_H_

#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "src/gui/fonts/fonts.h"

// The panel, in pixels, and the page padding every layout is written against.
#define BP_SCREEN_W 240
#define BP_SCREEN_H 320
#define BP_PADDING 6
#define BP_STATUS_BAR_H 24
#define BP_TILE_RADIUS 10
// The black quarter-circles matching the glass: about 2 mm of the 2" IPS.
#define BP_SCREEN_CORNER_RADIUS 16
#define BP_HEADER_BUTTON_SIZE 36
#define BP_HEADER_BUTTON_GAP 4

static inline bool bp_is_tempotec_v1(void) { return true; }

static inline int bp_screen_w(void) { return BP_SCREEN_W; }
static inline int bp_screen_h(void) { return BP_SCREEN_H; }
static inline int bp_padding(void) { return BP_PADDING; }
static inline int bp_status_bar_h(void) { return BP_STATUS_BAR_H; }
static inline int bp_tile_radius(void) { return BP_TILE_RADIUS; }
static inline int bp_screen_corner_radius(void) { return BP_SCREEN_CORNER_RADIUS; }
static inline int bp_header_button_size(void) { return BP_HEADER_BUTTON_SIZE; }
static inline int bp_header_button_gap(void) { return BP_HEADER_BUTTON_GAP; }

// One geometry number written once: the compact value. The second argument is
// the 480-pixel layout this fork no longer draws, kept in the call sites
// because it is what documents the number (`bp_pick(56, 88)` says what the V1's
// 56 was chosen against); the compiler folds the call to the first.
static inline int bp_pick(int compact, int regular) {
	(void)regular;
	return compact;
}

// Fonts used by the most crowded player/list labels. fonts.c maps the named UI
// faces to compact raster sizes too, so pages which use font_ui_24 directly do
// not keep 24 physical pixels on a 240-pixel display.
static inline const lv_font_t *bp_title_font(void) { return &font_ui_16; }
static inline const lv_font_t *bp_artist_font(void) { return &font_ui_14; }
static inline const lv_font_t *bp_time_font(void) { return &font_ui_14; }
static inline const lv_font_t *bp_tile_label_font(void) { return &font_ui_14; }
static inline const lv_font_t *bp_queue_label_font(void) { return &font_ui_14; }
static inline const lv_font_t *bp_format_label_font(void) { return &font_ui_14; }

#endif // SRC_GUI_BOARD_PROFILE_H_
