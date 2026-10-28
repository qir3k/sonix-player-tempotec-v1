#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/menuorder.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/streaming/ytmusicpage.h"
#include "src/system/addons/addons.h"
#include "src/system/streaming/ytmusic.h"

lv_obj_t *streaming_screen;

static lv_obj_t *streaming_grid;
static lv_obj_t *streaming_list;
static gui_config_t *streaming_cfg;
static lv_obj_t *streaming_rows[6];

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;
	theme_semantic_t tone;
	lv_obj_t **target;
	// The addon this service belongs to: the one setting that both hides its
	// tile and keeps its pages from being built (see addons.h).
	const char *addon;
} streaming_item_t;

static const streaming_item_t tokyo_items[] = {
	{"radio", &icon_radio_player, THEME_SEMANTIC_PURPLE, &radiopage_screen, "radio"},
	{"podcasts", &icon_podcast_list, THEME_SEMANTIC_CYAN, &podcast_screen, "podcasts"},
	{"tidal", &icon_tidal_mark, THEME_SEMANTIC_CYAN, &tidal_screen, "tidal"},
	{"qobuz", &icon_qobuz_mark, THEME_SEMANTIC_PURPLE, &qobuz_screen, "qobuz"},
	{"subsonic", &icon_music_note, THEME_SEMANTIC_BLUE, &subsonic_screen, "subsonic"},
	{"YouTube Music", &icon_music_note, THEME_SEMANTIC_RED, &ytmusic_screen, "ytmusic"},
};

static bool streaming_shown(int id, bool visible) {
	return visible && addons_enabled(tokyo_items[id].addon);
}

#define STREAMING_COUNT ((int)(sizeof(tokyo_items) / sizeof(tokyo_items[0])))

static void make_streaming_entries(grid_entry_t *entries, int *order, bool *visible) {
	int count = menuorder_load(MENUORDER_STREAMING, order, STREAMING_COUNT, visible);
	for (int i = 0; i < count; i++) {
		int id = order[i];
		const streaming_item_t *item = &tokyo_items[id];
		entries[i] = (grid_entry_t){item->label, item->icon, item->target, NULL};
	}
}

static void streaming_list_build(gui_config_t *cfg) {
	streaming_list = settingsrow_page(streaming_screen, cfg, "streaming");
	player_sheet_attach_drag(streaming_list, true);

	for (int i = 0; i < STREAMING_COUNT; i++) {
		const streaming_item_t *item = &tokyo_items[i];
		streaming_rows[i] = settingsrow_add(streaming_list, item->label, NULL, switch_screen_cb, *item->target);
		settingsrow_add_icon(streaming_rows[i], item->icon, item->tone);
		if (!addons_enabled(item->addon)) {
			lv_obj_set_hidden(streaming_rows[i], true);
		}
	}
}

void streaming_refresh_layout(void) {
	if (!streaming_cfg) {
		return;
	}
	int order[STREAMING_COUNT];
	bool visible[STREAMING_COUNT];
	if (menuorder_load(MENUORDER_STREAMING, order, STREAMING_COUNT, visible) != STREAMING_COUNT) {
		// Nothing arranged yet, or an order that no longer fits: the built-in
		// order with everything shown, still subject to the addon switches.
		for (int i = 0; i < STREAMING_COUNT; i++) {
			order[i] = i;
			visible[i] = true;
		}
	}
	if (streaming_grid) {
		grid_entry_t entries[STREAMING_COUNT];
		make_streaming_entries(entries, order, visible);
		gridpage_replace_entries(streaming_grid, streaming_cfg, entries, STREAMING_COUNT, 2, 3);
		for (int i = 0; i < STREAMING_COUNT; i++) {
			lv_obj_set_hidden(lv_obj_get_child(streaming_grid, i), !streaming_shown(order[i], visible[order[i]]));
		}
	}
	if (streaming_list) {
		for (int i = 0; i < STREAMING_COUNT; i++) {
			lv_obj_t *row = streaming_rows[order[i]];
			lv_obj_set_hidden(row, !streaming_shown(order[i], visible[order[i]]));
			lv_obj_move_to_index(row, i);
		}
	}
}

static void streaming_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_uses_function_glyphs();
	if (streaming_grid) {
		lv_obj_set_hidden(streaming_grid, tokyo_v1);
	}
	if (streaming_list) {
		lv_obj_set_hidden(streaming_list, !tokyo_v1);
	}
}

void streaming_init(gui_config_t *cfg) {
	streaming_cfg = cfg;
	ytmusic_init();
	// Where an empty page keeps its response body for a pull: the same card
	// the player's own logs land on.
	ytmusic_set_card_root(cfg && cfg->sd_root_path ? cfg->sd_root_path : NULL);
	// Only the services that are switched on are built: these pages hold the
	// lists, the rows and the artwork of a whole catalogue, and an unused one is
	// the largest thing in the heap. The system init above (cookies, keys) stays
	// unconditional and cheap.
	if (addons_enabled("qobuz")) qobuzpage_init(cfg);
	if (addons_enabled("tidal")) tidalpage_init(cfg);
	if (addons_enabled("podcasts")) podcastpage_init(cfg);
	if (addons_enabled("subsonic")) subsonicpage_init(cfg);
	if (addons_enabled("ytmusic")) ytmusicpage_init(cfg);

	grid_entry_t entries[STREAMING_COUNT];
	int order[STREAMING_COUNT];
	bool visible[STREAMING_COUNT];
	make_streaming_entries(entries, order, visible);

	// Dark and Light keep their established two-column grid. Tokyo Night on the
	// V1 gets the same destinations as a compact, icon-led list.
	streaming_grid = gridpage_build(streaming_screen, cfg, entries, STREAMING_COUNT, 2, 3, true);
	if (bp_is_tempotec_v1()) {
		streaming_list_build(cfg);
	} else {
		settingsrow_title(streaming_screen, cfg, "streaming");
	}
	streaming_refresh_theme();
	streaming_refresh_layout();
	theme_register_refresh(streaming_refresh_theme);
}
