#include "aamlayout.h"

// The rows, top to bottom, and the picture between them.
//
// Everything that can be negative is held at something drawable rather than
// left to fall through: a panel smaller than the rows it is asked for is a
// mistake in the board profile, not a reason to hand a widget a negative size.
aam_box_t aam_layout_box(int screen_w, int screen_h, int status_h, int header_h, int strip_h, bool leftie) {
	aam_box_t b;
	if (status_h < 0) {
		status_h = 0;
	}
	if (header_h < 0) {
		header_h = 0;
	}
	if (strip_h < 0) {
		strip_h = 0;
	}

	b.status_h = status_h;
	b.header_h = header_h;
	b.strip_h = strip_h;
	b.art_x = leftie ? AAM_RAIL_W : 0;
	b.art_y = status_h + header_h;
	b.art_w = screen_w - b.art_x;
	if (b.art_w < 1) {
		b.art_w = 1;
	}
	// The picture takes what the two rows above it and the strip below leave.
	// A sleeve that would come back smaller than AAM_MIN_ART_H keeps that much
	// anyway: what it overruns is clipped by the panel, which reads as a full
	// picture rather than as a letterbox with nothing in it.
	b.art_h = screen_h - b.art_y - strip_h;
	if (b.art_h < AAM_MIN_ART_H) {
		b.art_h = AAM_MIN_ART_H;
	}
	return b;
}

// The three marks of the header row. The slot's centre has to sit half a glyph
// plus the end padding in from the edge for the *glyph* to keep that padding,
// and the slot is wider than the glyph, so the slot starts nearer the edge than
// the padding suggests.
void aam_head_slots(int screen_w, int out[3]) {
	(void)screen_w; // the row is measured from its right end, whatever its width
	int half = AAM_HEAD_SLOT / 2;
	for (int i = 0; i < 3; i++) {
		out[i] = AAM_HEAD_END_PAD + AAM_HEAD_GLYPH / 2 - half + i * AAM_HEAD_PITCH;
	}
}

int aam_head_marks_left(int screen_w) {
	int slots[3];
	aam_head_slots(screen_w, slots);
	return screen_w - (slots[2] + AAM_HEAD_SLOT);
}

// The two names in the strip. The rule the arrangement was asked for: a name
// that fits is not made to scroll, and the artist -- which stands first -- is
// not squeezed to a sliver by a long track name beside it.
void aam_strip_shares(int avail, int artist_px, int title_px, int out[2]) {
	if (avail < 1) {
		avail = 1;
	}
	if (artist_px < 0) {
		artist_px = 0;
	}
	if (title_px < 0) {
		title_px = 0;
	}

	int artist;
	int title;
	if (artist_px + title_px <= avail) {
		// Both fit, so both take their own words and neither moves.
		artist = artist_px;
		title = title_px;
	} else {
		// One of them short enough to keep whole: it does, and the other has
		// what is left. Otherwise they share the row in proportion to their
		// own lengths, which is the fairest split when neither fits.
		int floor_px = (avail * AAM_STRIP_MIN_SHARE) / 100;
		if (artist_px <= floor_px) {
			artist = artist_px;
			title = avail - artist_px;
		} else if (title_px <= floor_px) {
			title = title_px;
			artist = avail - title_px;
		} else {
			artist = (int)(((long long)avail * artist_px) / (artist_px + title_px));
			title = avail - artist;
		}
	}
	out[0] = artist;
	out[1] = title;
}
