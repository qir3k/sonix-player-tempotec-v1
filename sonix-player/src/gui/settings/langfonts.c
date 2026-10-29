#include "langfonts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *langfonts_screen;

// How many of each the page can show: lang.c reads sixteen languages at most,
// the built-in faces are three and a card may add more of its own.
#define LANGFONTS_LANG_MAX 16
#define LANGFONTS_FACE_MAX 24
#define LANGFONTS_SCRIPT_MAX 8

// Every switch on this page says the same thing when it is thrown: what is
// already built stays built until the next start, which is what the toast
// says rather than pretending the memory has been handed back.
static void language_switch_cb(lv_event_t *e) {
	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}
	lv_obj_t *sw = lv_event_get_target(e);
	lang_set_enabled(name, lv_obj_has_state(sw, LV_STATE_CHECKED));
	toast_success("addon_restart_needed");
}

static void face_switch_cb(lv_event_t *e) {
	const char *id = lv_event_get_user_data(e);
	if (!id) {
		return;
	}
	lv_obj_t *sw = lv_event_get_target(e);
	fonts_set_enabled(id, lv_obj_has_state(sw, LV_STATE_CHECKED));
	toast_success("addon_restart_needed");
}

static void script_switch_cb(lv_event_t *e) {
	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}
	lv_obj_t *sw = lv_event_get_target(e);
	fonts_set_script_enabled(name, lv_obj_has_state(sw, LV_STATE_CHECKED));
	toast_success("addon_restart_needed");
}

// The switch's own row callback is not used anywhere on this page -- the
// switch alone decides -- so it is this, and the id rides on a second
// callback added below, exactly as on the Plugins page above it.
static void switch_noop(lv_event_t *e) { (void)e; }

// One row with its switch already carrying what is stored. A row that cannot
// be switched off is drawn checked and dead -- the language on screen, English,
// the default face, the face in force -- rather than as a switch that would
// snap back the next time the page is opened.
static void add_switch_row(lv_obj_t *list, const char *label, bool on, bool locked, lv_event_cb_t cb,
						   const char *key) {
	lv_obj_t *sw = NULL;
	lv_obj_t *row = settingsrow_toggle(list, label, &sw, switch_noop);
	if (!sw || !row) {
		return;
	}
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	}
	if (locked) {
		lv_obj_add_state(sw, LV_STATE_DISABLED);
		return;
	}
	lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

// A group of switches: its heading, the sentence saying what the group is,
// then the rows themselves. Every group on this page is a different kind of
// thing -- a translated interface, a typeface, a set of letters for a script
// the main face has none of -- and three bare lists of names in a row is what
// made the old page unreadable: "Japanese" under "Polski" looks like another
// language rather than the face that stops a Japanese title drawing as boxes.
static void add_group(lv_obj_t *list, const char *heading, const char *note) {
	settingsrow_heading(list, heading);
	if (note) {
		settingsrow_add_note(list, note);
	}
}

void langfonts_init(gui_config_t *cfg) {
	langfonts_screen = lv_obj_create(NULL);
	lv_obj_add_style(langfonts_screen, &theme_style_screen, 0);
	lv_obj_t *list = settingsrow_page(langfonts_screen, cfg, "languages_fonts");

	// The languages, from the whole directory rather than from what is on:
	// this is the page that decides what is on, so it has to be able to bring
	// a language back. The rows say the languages' own names, as the settings
	// page spells them.
	add_group(list, "langfonts_group_languages", "langfonts_languages_note");
	const char *languages[LANGFONTS_LANG_MAX];
	int lang_count = lang_all(languages, LANGFONTS_LANG_MAX);
	for (int i = 0; i < lang_count; i++) {
		char label[64];
		lang_display_name(languages[i], label, sizeof(label));
		bool on = lang_enabled(languages[i]);
		add_switch_row(list, label, on, lang_locked(languages[i]), language_switch_cb, languages[i]);
	}

	// The faces the font picker will offer: the built-in ones and whatever
	// the card's Fonts folder carries. MiSans is the default and answers on
	// whatever the switch says, and so does the face in force.
	add_group(list, "langfonts_group_fonts", "langfonts_fonts_note");
	const char *face_ids[LANGFONTS_FACE_MAX];
	const char *face_labels[LANGFONTS_FACE_MAX];
	int face_count = fonts_all_choices(face_ids, face_labels, LANGFONTS_FACE_MAX);
	for (int i = 0; i < face_count; i++) {
		bool on = fonts_enabled(face_ids[i]);
		add_switch_row(list, face_labels[i], on, fonts_locked(face_ids[i]), face_switch_cb, face_ids[i]);
	}

	// The scripts chained behind the main face, so a title the main face has
	// no letters for draws letters rather than squares. These are letters and
	// nothing else: switching one on does not translate a word of the
	// interface, which is the distinction the heading and the note carry.
	add_group(list, "langfonts_group_scripts", "langfonts_scripts_note");
	const char *scripts[LANGFONTS_SCRIPT_MAX];
	int script_count = fonts_script_names(scripts, LANGFONTS_SCRIPT_MAX);
	for (int i = 0; i < script_count; i++) {
		bool on = fonts_script_enabled(scripts[i]);
		add_switch_row(list, scripts[i], on, false, script_switch_cb, scripts[i]);
	}

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(langfonts_screen);
}
