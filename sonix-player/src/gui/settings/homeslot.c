#include "homeslot.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/calculator/calculator.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/library/filespage.h"
#include "src/gui/metronome/metronome.h"
#include "src/gui/notes/notes.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/addons/addons.h"
#include "src/system/core/config.h"

lv_obj_t *homeslot_screen;

// The two stored answers that are not candidates: nothing chosen yet, and the
// hole chosen on purpose. Both leave the grid without a library tile.
#define HOMESLOT_NONE "none"

// One destination that can be handed the slot. The order is the page's, which
// is the More page's: the DAC and the emulator first, then the reading shelves,
// then the tools.
//
// `addon` is the module the destination belongs to, or NULL for one that is
// always built. It is asked here rather than at the click: a module that is off
// has no page, so its row is not offered at all.
typedef struct {
	const char *slug;   // the config value
	const char *label;  // the tile's name, a tag or a literal
	const char *addon;  // the module it needs, or NULL
	const lv_image_dsc_t *icon;
	lv_obj_t **screen;       // a built page...
	void (*action)(void);    // ...or an action that opens one
} homeslot_candidate_t;

static const homeslot_candidate_t CANDIDATES[] = {
	{"dac", "dac", NULL, &icon_menu_dac, &dacpage_screen, NULL},
#if GB_CORE
	{"gearboy", "gearboy", "gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
	// The shelf is an action rather than a screen for the More page's reason:
	// it reads the card every time it is opened.
	{"books", "books", "ebook", &icon_menu_books, NULL, ebookpage_open},
	{"file_explorer", "file_explorer", NULL, &icon_menu_file_explorer, NULL, filespage_open},
	{"calculator", "calculator", "calculator", &icon_menu_calculator, NULL, calculator_open},
	{"notes", "notes", "notes", &icon_menu_notes, NULL, notes_open},
	{"metronome", "metronome", "metronome", &icon_menu_metronome, NULL, metronome_open},
	{"flappy_bird", "flappy_bird", "flappy", &icon_menu_flappy_bird, NULL, flappybird_open},
};

#define CANDIDATE_COUNT ((int)(sizeof(CANDIDATES) / sizeof(CANDIDATES[0])))

static const homeslot_candidate_t *candidate_of(const char *slug) {
	if (!slug || !slug[0]) {
		return NULL;
	}
	for (int i = 0; i < CANDIDATE_COUNT; i++) {
		if (strcmp(CANDIDATES[i].slug, slug) == 0) {
			return &CANDIDATES[i];
		}
	}
	return NULL;
}

const char *homeslot_choice(void) { return config_get("ui", "home_slot", ""); }

bool homeslot_resolve(const char *slug, homeslot_target_t *out) {
	const homeslot_candidate_t *candidate = candidate_of(slug);
	if (!candidate) {
		return false;
	}
	// A destination whose module was switched off in Plugins was never built:
	// its page object is NULL and its action would open nothing.
	if (candidate->addon && !addons_enabled(candidate->addon)) {
		return false;
	}
	if (out) {
		out->label = candidate->label;
		out->icon = candidate->icon;
		out->screen = candidate->screen;
		out->action = candidate->action;
	}
	return true;
}

// The page is rebuilt rather than repainted when the choice moves: the row that
// carries it is marked with the same green chevron an enabled row wears, and
// that mark lives on the row itself.
static lv_obj_t *slot_list;

static void build_rows(void);

static void select_cb(lv_event_t *e) {
	const char *slug = lv_event_get_user_data(e);
	if (!slug) {
		return;
	}
	const char *current = homeslot_choice();
	if (current && strcmp(current, slug) == 0) {
		return;
	}
	config_set("ui", "home_slot", slug);
	config_save();
	build_rows();
}

static void add_choice_row(const char *slug, const char *label, const lv_image_dsc_t *icon) {
	// A choice between destinations is not something that is switched on, so
	// the row opens nothing: it marks itself as the answer.
	lv_obj_t *row = settingsrow_add(slot_list, label, NULL, select_cb, (void *)slug);
	const char *current = homeslot_choice();
	bool chosen = current && strcmp(current, slug) == 0;
	if (icon) {
		settingsrow_add_icon(row, icon, THEME_SEMANTIC_CYAN);
	}
	settingsrow_chevron_active(row, chosen);
}

static void build_rows(void) {
	if (!slot_list) {
		return;
	}
	lv_obj_clean(slot_list);
	settingsrow_heading(slot_list, "homeslot_group_choice");
	settingsrow_add_note(slot_list, "homeslot_note");
	// "Nothing" is a real answer, and not the same as an unanswered setting:
	// one is the hole chosen on purpose, the other is the default this page
	// offers until somebody picks.
	add_choice_row(HOMESLOT_NONE, "homeslot_none", NULL);
	for (int i = 0; i < CANDIDATE_COUNT; i++) {
		const homeslot_candidate_t *candidate = &CANDIDATES[i];
		if (candidate->addon && !addons_enabled(candidate->addon)) {
			continue;
		}
		add_choice_row(candidate->slug, candidate->label, candidate->icon);
	}
}

void homeslot_init(gui_config_t *cfg) {
	homeslot_screen = lv_obj_create(NULL);
	lv_obj_add_style(homeslot_screen, &theme_style_screen, 0);
	slot_list = settingsrow_page(homeslot_screen, cfg, "home_slot");
	build_rows();
	player_sheet_attach_drag(slot_list, true);
	switcher_attach_back_gesture(homeslot_screen);
}
