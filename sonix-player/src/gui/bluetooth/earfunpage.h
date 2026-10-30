#ifndef EARFUNPAGE_H
#define EARFUNPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The EarFun page: what each earbud and the case have left, and the switches the
// earbuds themselves carry -- noise control, its strength, the transparency
// variant, Game Mode and in-ear detection.
//
// It is reached from Bluetooth audio, and its row exists only while a pair is
// actually connected: the page draws what the protocol session in earfun.h
// reports, and without one there is nothing to draw.

extern lv_obj_t *earfunpage_screen;

void earfunpage_init(gui_config_t *cfg);

#endif /* EARFUNPAGE_H */
