#include "plugins.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/homeslot.h"
#include "src/gui/settings/langfonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/addons/addons.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *plugins_screen;

// The switch's own row callback is not used -- the switch alone decides -- so it
// is this, and the module's id rides on a second callback added below.
static void addon_switch_noop(lv_event_t *e) { (void)e; }

// One row's switch. The module is switched in the config here and now; what is
// already built stays built until the next start, which is what the toast says
// rather than pretending the memory has been handed back.
static void addon_switch_cb(lv_event_t *e) {
	const char *id = lv_event_get_user_data(e);
	if (!id) {
		return;
	}
	lv_obj_t *sw = lv_event_get_target(e);
	addons_set(id, lv_obj_has_state(sw, LV_STATE_CHECKED));
	toast_success("addon_restart_needed");
}

// The favourite mark: the heart this player has always drawn, or the star the
// older builds used. A choice rather than a switch pair, and one setting for the
// whole interface -- the player, the lists and the Favourites tiles read it
// through favmark.c.
static void fav_glyph_pill_cb(lv_event_t *e) {
	int value = (int)(intptr_t)lv_event_get_user_data(e);
	if (value == (config_get_bool("ui", "fav_star", false) ? 1 : 0)) {
		return;
	}
	config_set_bool("ui", "fav_star", value == 1);
	config_save();
	// The marks on a page already built keep the shape they were made with;
	// the player and the lists read it again on their next draw.
	toast_success("addon_restart_needed");
}

static void build_fav_mark_row(lv_obj_t *list) {
	lv_obj_t *pills = NULL;
	lv_obj_t *row = settingsrow_pills(list, "fav_mark", &pills);
	if (!pills) {
		return;
	}
	settingsrow_add_caption(row, "fav_mark_note");
	bool star = config_get_bool("ui", "fav_star", false);
	lv_obj_t *heart = settingsrow_pill(pills, "fav_mark_heart", 0, fav_glyph_pill_cb);
	lv_obj_t *star_pill = settingsrow_pill(pills, "fav_mark_star", 1, fav_glyph_pill_cb);
	settingsrow_pill_active(heart, !star);
	settingsrow_pill_active(star_pill, star);
}

void plugins_init(gui_config_t *cfg) {
	plugins_screen = lv_obj_create(NULL);
	lv_obj_add_style(plugins_screen, &theme_style_screen, 0);
	lv_obj_t *list = settingsrow_page(plugins_screen, cfg, "plugins");

	langfonts_init(cfg);
	homeslot_init(cfg);

	// One heading per group of modules, taken from the module list itself: the
	// page never has to know which id belongs where, so a module added to
	// addons.c lands under the right heading by itself.
	const char *section = NULL;
	for (int i = 0; i < addons_count(); i++) {
		const addon_t *addon = addons_at(i);
		if (!addon) {
			continue;
		}
		if (!section || strcmp(section, addon->section) != 0) {
			section = addon->section;
			settingsrow_heading(list, section);
		}
		lv_obj_t *sw = NULL;
		lv_obj_t *row = settingsrow_toggle(list, addon->label, &sw, addon_switch_noop);
		settingsrow_add_caption(row, addon->note);
		if (!sw) {
			continue;
		}
		if (addons_enabled(addon->id)) {
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		}
		lv_obj_add_event_cb(sw, addon_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)addon->id);
	}

	// What is below the modules is not a module: it is the interface itself,
	// and it costs one bitmap or one config line rather than a page built at
	// startup. A heading of its own keeps the two kinds of switch apart -- a
	// module that is off hands its pages' memory back, a choice here does not.
	settingsrow_heading(list, "plugins_section_interface");
	settingsrow_add_note(list, "plugins_interface_note");

	// The mark the whole interface uses for a favourite. A choice, not a
	// module: it costs one bitmap either way.
	build_fav_mark_row(list);

	// Which destination takes the home screen's library tile while Audiobooks
	// is off, so the grid does not keep an empty box.
	lv_obj_t *slot = settingsrow_add(list, "home_slot", NULL, switch_screen_cb, homeslot_screen);
	settingsrow_add_caption(slot, "home_slot_note");

	// Languages and fonts: the other half of what a build used to give up by
	// deleting files, as a page of its own because it is two dozen rows where
	// this one has room for two. Its row says which three kinds of thing it
	// holds, because they are easy to mistake for each other.
	lv_obj_t *langs = settingsrow_add(list, "languages_fonts", NULL, switch_screen_cb, langfonts_screen);
	settingsrow_add_caption(langs, "languages_fonts_note");

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(plugins_screen);
}
