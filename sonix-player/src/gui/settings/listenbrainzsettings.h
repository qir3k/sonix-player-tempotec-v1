#ifndef LISTENBRAINZSETTINGS_H
#define LISTENBRAINZSETTINGS_H

#include "src/gui/shell/gui.h"

// The ListenBrainz page of the Music settings: the switch, the account, the
// state and the queue, and the screen the token is typed into. Built once at
// startup, like every other page.

lv_obj_t *listenbrainzsettings_screen(void);
void listenbrainzsettings_init(gui_config_t *cfg);

#endif /* LISTENBRAINZSETTINGS_H */
