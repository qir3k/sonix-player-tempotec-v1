#ifndef GUI_FAVMARK_H
#define GUI_FAVMARK_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The mark a favourite wears, one shape everywhere.
//
// The heart is what this player draws: the player's button, the lists'
// selection marks and the Favourites tiles all use it, and a mark that changes
// shape from page to page reads as two different features. The star is the glyph
// older builds used, and some listeners are used to it -- so which one the whole
// interface draws is a setting (Plugins -> Favourite mark), read here and
// nowhere else.
//
// The two stars live in favmark.c rather than in icons.c: that file is generated
// from assets/icons, and the star SVGs are not part of its table any more. They
// are the same 32 px A8 format as the hearts, so a caller can swap one for the
// other without touching its size or its scale.
bool favmark_is_star(void);

// The unfilled and filled marks, as the icon tables hand theirs out. Re-read
// them on every draw: the setting can have changed since the last one.
const lv_image_dsc_t *favmark_outline(void);
const lv_image_dsc_t *favmark_filled(void);

#endif /* GUI_FAVMARK_H */
