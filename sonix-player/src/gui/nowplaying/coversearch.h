#ifndef COVERSEARCH_H
#define COVERSEARCH_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Cover art for the album playing, found online (coverart.h) when its files
// carry none. The track menu starts the search from the tags of the playing
// track; the albums found are listed to pick from, a tap shows the picture, and
// Apply saves it as cover.jpg in the album's folder and has the player pick it
// up. Back from the picture returns to the list, and back from the list to the
// player.

extern lv_obj_t *coversearch_screen;

void coversearch_init(gui_config_t *cfg);

// Looks the playing track's album up, on a thread of its own, and opens the
// list over the player when something is found; otherwise says why not.
void coversearch_start(void);

// The page where the contact MusicBrainz is told is typed, and its row for
// Music settings: MusicBrainz asks every program that uses it to say how to
// reach whoever is calling, and the search keeps to the Store until it is set.
// The row goes on the page `parent`, after coversearch_init().
void coversearch_contact_row(lv_obj_t *parent);
lv_obj_t *coversearch_contact_screen(void);

#endif /* COVERSEARCH_H */
