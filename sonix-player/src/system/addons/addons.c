#include "addons.h"

#include <stddef.h>
#include <string.h>

#include "src/system/core/config.h"

// The list, and nothing else in this file knows what any of them do: the gate
// is asked by the page that would build the module, and by the menu that would
// offer it. Adding a module is a row here, a switch that never leaves the
// player, and the two questions -- "may I build?" -- at the places that build.
//
// Order is the Plugins page's: the services first, because they are the ones
// people actually turn off, then what belongs beside the music, then the tools.
// The section tag is on every row so the page never has to know which id is
// what -- a module added here lands under the right heading by itself.
static const addon_t ADDONS[] = {
	{"tidal", "tidal", "addon_tidal_note", "plugins_section_services"},
	{"qobuz", "qobuz", "addon_qobuz_note", "plugins_section_services"},
	{"ytmusic", "YouTube Music", "addon_ytmusic_note", "plugins_section_services"},
	{"podcasts", "podcasts", "addon_podcasts_note", "plugins_section_services"},
	{"radio", "radio", "addon_radio_note", "plugins_section_services"},
	{"subsonic", "subsonic", "addon_subsonic_note", "plugins_section_services"},
	// The shelves are a place of their own: a book is not a music feature, and
	// nobody would look for it under one.
	{"audiobooks", "audiobooks", "addon_audiobooks_note", "plugins_section_library"},
	{"ebook", "books", "addon_ebook_note", "plugins_section_library"},
	// What the player does WITH the music: words on screen, a curve for a pair
	// of headphones, a line to two scrobblers. Four features, not four places,
	// which is why the heading says so.
	{"lyrics", "lyrics_search", "addon_lyrics_note", "plugins_section_features"},
	{"autoeq", "autoeq", "addon_autoeq_note", "plugins_section_features"},
	{"lastfm", "last_fm", "addon_lastfm_note", "plugins_section_features"},
	{"listenbrainz", "listenbrainz", "addon_listenbrainz_note", "plugins_section_features"},
	{"calculator", "calculator", "addon_calculator_note", "plugins_section_tools"},
	{"notes", "notes", "addon_notes_note", "plugins_section_tools"},
	{"flappy", "flappy_bird", "addon_flappy_note", "plugins_section_tools"},
	{"gearboy", "gearboy", "addon_gearboy_note", "plugins_section_tools"},
	{"metronome", "metronome", "addon_metronome_note", "plugins_section_tools"},
};

int addons_count(void) { return (int)(sizeof(ADDONS) / sizeof(ADDONS[0])); }

const addon_t *addons_at(int index) {
	if (index < 0 || index >= addons_count()) {
		return NULL;
	}
	return &ADDONS[index];
}

const addon_t *addons_find(const char *id) {
	if (!id) {
		return NULL;
	}
	for (int i = 0; i < addons_count(); i++) {
		if (strcmp(ADDONS[i].id, id) == 0) {
			return &ADDONS[i];
		}
	}
	return NULL;
}

bool addons_enabled(const char *id) {
	if (!id) {
		return true;
	}
	// Everything starts on: an update that adds a module must not take the
	// interface somebody had away, and the player that was shipped before this
	// existed behaves exactly as it did.
	return config_get_bool("addons", id, true);
}

void addons_set(const char *id, bool on) {
	if (!id || !addons_find(id)) {
		return;
	}
	config_set_bool("addons", id, on);
	config_save();
}

void addons_apply_defaults(bool (*is_default)(const char *id)) {
	if (!is_default) {
		return;
	}
	for (int i = 0; i < addons_count(); i++) {
		config_set_bool("addons", ADDONS[i].id, is_default(ADDONS[i].id));
	}
	config_save();
}
