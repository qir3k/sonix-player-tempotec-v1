#ifndef COVERFLOW_H
#define COVERFLOW_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// The album carousel: a horizontal row of records. The one in the middle faces
// the user; the ones on either side turn away from it and are walked left and
// right with a finger.
//
// The turn is a perspective projection, not a squeeze: a side record narrows
// towards its inner edge and its artwork crowds naturally towards that edge.
// LVGL cannot draw that projection directly, so coverflow.c works it out per
// pixel. Only the two records either side of the middle are part way through a
// turn, which keeps the work bounded while a finger is down.
//
// What gets turned is the RECORD, not the cover: a square built once per place,
// holding the card and, printed on it, the artwork -- or the album glyph when
// there is none. So the container turns with the picture, the glyph turns with
// the card, and a place whose cover has not arrived shows a blank record rather
// than somebody else's sleeve.
//
// Every cover is still decoded ONCE, at one size, and kept in the thumbnail
// database.
//
// It is not a page but a sheet, like the control centre, and it comes the other
// way: it is pulled DOWN from the top edge of the screen, over whatever page is
// showing, and goes back up the same way. The pull follows the finger. Anywhere
// the status bar is shown the bar carries the gesture (see topbar.c); the pages
// that hide the bar -- the player -- carry their own strip of it and call the
// three functions below (see player.c).

void coverflow_init(gui_config_t *cfg);

// True while the carousel is up, or on its way up or down: the sheet is over
// the page and a second pull from the top, or the control centre from the
// bottom, has no business starting underneath it.
bool coverflow_is_open(void);

// Brings the sheet down on its own, without a finger on it. Says no rather than
// opening on nothing when the setting is off, the library is closed, or one of
// the sheets that has to stay in front is up.
void coverflow_open(void);

// The setting behind it. Off by default: it is a second way to reach the albums,
// not a replacement for the list, and it costs a database of decoded covers.
bool coverflow_enabled(void);
void coverflow_set_enabled(bool on);

// Interactive open/close: the sheet follows the finger instead of jumping the
// moment a drag starts.
//   begin  -- take the sheet wherever it currently is (up, or parked above the
//             top edge). False when it cannot come up at all: the setting is
//             off, the library is shut, or something owns the device.
//   update -- dy is the finger's travel since the press. Downwards brings the
//             sheet in, upwards puts it away.
//   end    -- animate to whichever end the release is closest to
bool coverflow_drag_begin(void);
void coverflow_drag_update(int dy);
void coverflow_drag_end(void);

#endif /* COVERFLOW_H */
