#ifndef YTMUSICPAGE_H
#define YTMUSICPAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"
#include "lvgl/lvgl.h"

extern lv_obj_t *ytmusic_screen;
void ytmusicpage_init(gui_config_t *cfg);

// "Can this queue entry be played right now?", asked by device_state before it
// opens one. A song the page queued but nobody has played has no cache file
// yet: this answers no, fetches it, and the queue is restarted on it when it is
// there. Registered by ytmusicpage_init(); declared because it is defined
// elsewhere in the file than where it is registered.
bool ytmusicpage_prepare_track(char *path, size_t path_size);

#endif /* YTMUSICPAGE_H */
