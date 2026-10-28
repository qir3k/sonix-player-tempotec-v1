#ifndef GUI_METRONOME_METRONOME_H
#define GUI_METRONOME_METRONOME_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The metronome, from the More page.
//
// A tempo from 30 to 250 BPM, a bar of one to seven beats, and a tap button
// that takes the tempo from the finger rather than from the slider. The bar's
// first beat is accented, which is the only way a click in a bar of four is
// distinguishable from a click in a bar of three.
//
// The click is metronomeclick.c, on a thread of its own writing into the PCM;
// this page is the controls and the flash of the beat. The tempo and the bar
// are kept in the device config, so the page comes back as it was left.
extern lv_obj_t *metronome_screen;

void metronome_init(gui_config_t *cfg);

// Opens the page. An action rather than a screen to switch to, for the reason
// the calculator's is: the page reads the settings again on the way in.
void metronome_open(void);

#endif /* GUI_METRONOME_METRONOME_H */
