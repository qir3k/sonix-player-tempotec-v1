#ifndef BOOTPICPAGE_H
#define BOOTPICPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The "Boot picture" page under Settings > Appearance: the pictures in the
// Bootlogo folder on the card, and which of them the next power-on draws.
//
// A page rather than a row of pills because the folder's contents are not
// known until the card is: the list is read on the way in, so a card that has
// been through a computer since the page was last open shows what is on it
// now.
//
// Picking a row makes the picture into the boot screen there and then -- it is
// decoded, fitted to the panel and copied into the writable partition (see
// bootpic.h) -- which is a moment rather than a tap, so it happens on a
// thread of its own behind a busy card rather than on the interface's.
extern lv_obj_t *bootpicpage_screen;

void bootpicpage_init(gui_config_t *cfg);

#endif /* BOOTPICPAGE_H */
