#ifndef ARTWORKPAGE_H
#define ARTWORKPAGE_H

#include "src/gui/shell/gui.h"

// The page's screen, built empty here and filled by artworkpage_init().
extern lv_obj_t *artworkpage_screen;

// The page that offers artwork found online for one track: a row per answer,
// with the picture in it, and a tap that writes the chosen one beside the file.
// Built only when the "artwork" module is on (see src/system/addons/addons.c).
void artworkpage_init(gui_config_t *cfg);

// Searches for the record `title`, `artist`, `album` of the file at `path` and
// shows what came back. The search runs on a worker; the page says so while it
// does. Nothing is written until a row is tapped.
void artworkpage_open(const char *path, const char *title, const char *artist, const char *album);

#endif // ARTWORKPAGE_H
