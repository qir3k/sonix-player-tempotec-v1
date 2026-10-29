#include "plugins.h"

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/langfonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/addons/addons.h"
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

void plugins_init(gui_config_t *cfg) {
	plugins_screen = lv_obj_create(NULL);
	lv_obj_add_style(plugins_screen, &theme_style_screen, 0);
	lv_obj_t *list = settingsrow_page(plugins_screen, cfg, "plugins");

	langfonts_init(cfg);

	for (int i = 0; i < addons_count(); i++) {
		const addon_t *addon = addons_at(i);
		if (!addon) {
			continue;
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

	// Languages and fonts: the other half of what a build used to give up by
	// deleting files, as a page of its own because it is two dozen rows where
	// this one has room for two.
	settingsrow_add(list, "languages_fonts", NULL, switch_screen_cb, langfonts_screen);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(plugins_screen);
}
