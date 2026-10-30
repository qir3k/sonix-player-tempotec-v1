#ifndef NOISE_OVERLAY_H
#define NOISE_OVERLAY_H

#include <stdbool.h>
#include "src/gui/shell/gui.h"

// The noise control popup overlay ("bullet" / pill notification).
// Appears right below the status bar whenever noise cancellation, ambient,
// or normal/off mode is toggled (from earbud touch gestures or UI).

typedef enum {
	NOISE_OVERLAY_OFF = 1,
	NOISE_OVERLAY_ANC = 2,
	NOISE_OVERLAY_AMBIENT = 3,
} noise_overlay_mode_t;

void noise_overlay_init(gui_config_t *cfg);
void noise_overlay_show(noise_overlay_mode_t mode);

#endif /* NOISE_OVERLAY_H */
