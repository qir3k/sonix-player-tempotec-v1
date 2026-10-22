#ifndef AAMLAYOUT_H
#define AAMLAYOUT_H

#include <stdbool.h>

// The geometry of Album Art Max, on its own.
//
// Three rows and a picture: the shell's status bar, the header row under it
// (format, shuffle mode, menu, star), the sleeve, and the strip at the foot
// with the title and the place in the queue. Nothing in here knows about LVGL,
// fonts or widgets -- the caller passes in the heights it has measured, which
// is what lets the arrangement be worked out before anything has been laid out
// (the artwork is asked for at the size this gives, see reload_cover) and what
// lets the arithmetic be tested without a display.

// Leftie's rail, bar and head included: what the sleeve gives up in width when
// the scrub bar stands up along its left edge.
#define AAM_RAIL_W 12

// The sleeve never gets smaller than this, whatever the rows around it take:
// below it there is nothing left to look at, and the picture is the point.
#define AAM_MIN_ART_H 64

// The header row's three marks, measured in from the right edge.
//
// They are drawn in the corner buttons the deck already uses (AAM_HEAD_SLOT,
// 36 px), and what decides where a mark *looks* like it sits is the glyph, not
// the slot: AAM_HEAD_GLYPH is the size every one of the three is drawn at, so
// that the star and the shuffle, which come from canvases of 32 and 30 px, read
// as the same weight. The rightmost glyph then keeps AAM_HEAD_END_PAD off the
// panel's edge and the three stand on one pitch, which is what makes the row a
// row rather than three things pinned to the corner.
#define AAM_HEAD_SLOT 36
#define AAM_HEAD_GLYPH 20
#define AAM_HEAD_END_PAD 10
// And the pitch is where the air between them comes from: 26 px left 6 px of
// black between one glyph and the next, which on the device read as three marks
// crowding each other rather than as a row. At 30 px each of them keeps 10 px --
// half a glyph's width -- and the row is still short enough to leave the format
// line its 84 px for "16/44.1FLAC".
#define AAM_HEAD_PITCH 30

// Where the format line begins: past the chevron's own slot on the left.
#define AAM_FORMAT_X 46

// The strip at the foot shares its width between the artist's name, the track's
// and the place in the queue. When both names fit they get exactly the width
// their own words take and neither scrolls; when they do not, the shorter one
// keeps its words and the longer one takes the rest -- down to this share of
// the row, under which a name is no longer a name.
#define AAM_STRIP_MIN_SHARE 34

typedef struct {
	int status_h; // the shell's status bar, where the page begins
	int header_h; // the row of chrome under it
	int art_x, art_y, art_w, art_h;
	int strip_h; // the title and the queue position, at the foot
} aam_box_t;

// `screen_*` is the player's own size in pixels. `status_h`, `header_h` and
// `strip_h` are measured by the caller -- the board profile, the button size
// and the line height of the face in force. `leftie` stands the scrub bar up
// the left edge, which takes AAM_RAIL_W off the sleeve's width.
aam_box_t aam_layout_box(int screen_w, int screen_h, int status_h, int header_h, int strip_h, bool leftie);

// The alignment offsets of the three header buttons, as LV_ALIGN_RIGHT_MID sees
// them: out[0] the star (rightmost), out[1] the menu, out[2] the mode -- the
// order the concept draws them in. Negative when applied, as every alignment
// offset is.
void aam_head_slots(int screen_w, int out[3]);

// The x where the marks' slots begin: what the format line has to its right,
// and the number that says whether "16/44.1FLAC" still fits beside them.
int aam_head_marks_left(int screen_w);

// The strip's two names: how wide the artist's and the track's get, given how
// wide their own words are (`artist_px`, `title_px`) and the room the row has
// to share (`avail`, already less the padding, the queue position and the two
// gaps). out[0] is the artist -- which stands first -- and out[1] the track.
void aam_strip_shares(int avail, int artist_px, int title_px, int out[2]);

#endif /* AAMLAYOUT_H */
