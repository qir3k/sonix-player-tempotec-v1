#ifndef LANGFONTS_H
#define LANGFONTS_H

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// Settings -> Plugins -> Languages & Fonts: the switchboard over what this
// build offers in the two places a firmware used to be made smaller by
// deleting files -- which language files the language picker lists, which
// faces the font picker lists, and which script fallbacks the text chain
// opens. Nothing is deleted now: every file stays on the card and each of
// these is one config line, so a barebones player and a fully loaded one are
// the same image with two different answers.
//
// The page is reached from a row on the Plugins page and is built once, as
// every other page here is.
extern lv_obj_t *langfonts_screen;

// Called from plugins_init, before Plugins is opened.
void langfonts_init(gui_config_t *cfg);

#endif /* LANGFONTS_H */
