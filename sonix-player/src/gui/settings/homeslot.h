#ifndef HOMESLOT_H
#define HOMESLOT_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// Settings -> Plugins -> Home tile: what fills the home screen's library slot
// while the Audiobooks module is off.
//
// The home screen is a 2x3 grid and the library is one of its six tiles. With
// Audiobooks switched off in Plugins that page was never built, so the tile has
// nothing behind it: leaving it there would open an empty screen, and dropping
// it leaves a hole in the grid where everything else then shifts. The third
// answer is this page -- any destination of the More page (the DAC, the
// calculator, the notes, Flappy Bird, a shelf) can be handed the slot instead,
// and a build that wants the hole keeps that too.
//
// The choice is stored as a slug in the config, so a module that is switched
// off keeps its name in the setting and comes back with the module: the slot
// falls back to Audiobooks (or to nothing) while it is away, exactly as the
// Books-on-home option already did for the shelf.
extern lv_obj_t *homeslot_screen;

// Called from plugins_init, before the Plugins page can open it.
void homeslot_init(gui_config_t *cfg);

// The slug of the choice, "" when nothing is chosen, or "none" when the hole is
// wanted deliberately. Never NULL, so a caller can test it without a guard.
const char *homeslot_choice(void);

// One candidate resolved for the menu that has to draw the tile: its name, its
// glyph, and where it goes -- a page to switch to, or something to open.
typedef struct {
	const char *label;        // a tag, or a literal for a name that is its own
	const lv_image_dsc_t *icon;
	lv_obj_t **screen;        // a built page...
	void (*action)(void);     // ...or an action that opens one
} homeslot_target_t;

// False for a slug that is not a candidate, and for one whose module is
// switched off -- that page was never built, so the tile would lead nowhere.
bool homeslot_resolve(const char *slug, homeslot_target_t *out);

#endif /* HOMESLOT_H */
