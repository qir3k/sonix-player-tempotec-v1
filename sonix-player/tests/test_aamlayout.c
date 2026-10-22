/*
 * The rows of Album Art Max, on their own.
 *
 * The arrangement is three rows and a picture: the status bar, the header row,
 * the sleeve, the strip. Nothing here is LVGL -- aam_layout_box() takes the
 * heights the caller measured and answers with the box -- so what is checked
 * is the arithmetic that decides how much of a 240x320 panel the artwork gets,
 * in both of its shapes.
 *
 * The numbers are the V1's: a 24 px status bar, a 40 px header row (the taller
 * of AAM_HEADER_H and the board's 36 px corner button) and a strip one 15 px
 * line of title plus its padding (2 + 15 + 2 + 2 = 21).
 */
#include <stdio.h>
#include <stdlib.h>

#include "src/gui/nowplaying/aamlayout.h"

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

#define V1_W 240
#define V1_H 320
#define V1_STATUS 24
#define V1_HEADER 40
#define V1_STRIP 21

static void test_the_panel_is_shared_out(void) {
	aam_box_t b = aam_layout_box(V1_W, V1_H, V1_STATUS, V1_HEADER, V1_STRIP, false);

	// The page starts under the status bar and the header row sits on it.
	CHECK(b.status_h == V1_STATUS);
	CHECK(b.header_h == V1_HEADER);
	CHECK(b.art_y == V1_STATUS + V1_HEADER);

	// The sleeve is the whole width and everything the strip leaves: the rows
	// and the picture add up to the panel exactly, with nothing lost between
	// them and nothing overlapping.
	CHECK(b.art_x == 0);
	CHECK(b.art_w == V1_W);
	CHECK(b.art_h == V1_H - V1_STATUS - V1_HEADER - V1_STRIP);
	CHECK(b.art_y + b.art_h + b.strip_h == V1_H);

	// Which, on the V1, is the 235 x 235 the concept draws: a square by luck
	// of the numbers rather than by design, and large enough to be the point of
	// the arrangement.
	CHECK(b.art_w == 240);
	CHECK(b.art_h == 235);
}

static void test_leftie_takes_its_width_off_the_sleeve(void) {
	aam_box_t flat = aam_layout_box(V1_W, V1_H, V1_STATUS, V1_HEADER, V1_STRIP, false);
	aam_box_t left = aam_layout_box(V1_W, V1_H, V1_STATUS, V1_HEADER, V1_STRIP, true);

	// The rail stands where the picture used to begin, so the picture starts
	// after it and is exactly that much narrower. Nothing else moves: the
	// rows above and below are the same height and the foot is in the same
	// place, so the strip does not drift when the bar changes shape.
	CHECK(left.art_x == AAM_RAIL_W);
	CHECK(left.art_w == flat.art_w - AAM_RAIL_W);
	CHECK(left.art_y == flat.art_y);
	CHECK(left.art_h == flat.art_h);
	CHECK(left.strip_h == flat.strip_h);
	CHECK(left.art_x + left.art_w == V1_W);

	// A few pixels, which is the whole claim the shape makes: the picture is
	// not the thing being traded away here.
	CHECK(AAM_RAIL_W * 100 / V1_W <= 5);
}

static void test_the_sleeve_has_a_floor(void) {
	// A panel shorter than its own rows: the picture keeps AAM_MIN_ART_H and
	// the overrun is clipped, rather than the caller being handed a negative
	// height to pass on to a widget.
	aam_box_t b = aam_layout_box(V1_W, 100, V1_STATUS, V1_HEADER, V1_STRIP, false);
	CHECK(b.art_h == AAM_MIN_ART_H);

	// Degenerate rows are held at zero rather than dragging the picture up
	// over the status bar.
	aam_box_t z = aam_layout_box(V1_W, V1_H, -5, -5, -5, false);
	CHECK(z.status_h == 0);
	CHECK(z.header_h == 0);
	CHECK(z.strip_h == 0);
	CHECK(z.art_y == 0);
	CHECK(z.art_h == V1_H);

	// And a panel too narrow for Leftie's rail still answers with a picture.
	aam_box_t n = aam_layout_box(4, V1_H, V1_STATUS, V1_HEADER, V1_STRIP, true);
	CHECK(n.art_w >= 1);
}

static void test_the_taller_header_wins(void) {
	// AAM_HEADER_H is a floor and not a fixed height: a board whose corner
	// buttons are taller than 40 px hands over its own measurement, and the
	// artwork moves down by exactly that much rather than being drawn over.
	aam_box_t a = aam_layout_box(V1_W, V1_H, V1_STATUS, 40, V1_STRIP, false);
	aam_box_t b = aam_layout_box(V1_W, V1_H, V1_STATUS, 56, V1_STRIP, false);
	CHECK(b.art_y == a.art_y + 16);
	CHECK(b.art_h == a.art_h - 16);
	CHECK(b.art_y + b.art_h + b.strip_h == V1_H);
}

static void test_the_header_marks_stand_on_a_pitch(void) {
	int slots[3];
	aam_head_slots(V1_W, slots);

	// Star, menu, mode -- three 36 px slots on one pitch, so the eye reads a
	// row and not three marks pushed about by their own widths.
	CHECK(slots[1] - slots[0] == AAM_HEAD_PITCH);
	CHECK(slots[2] - slots[1] == AAM_HEAD_PITCH);

	// The rightmost glyph keeps the end padding off the panel's edge. It is
	// the glyph that has to sit there, and the glyph is drawn smaller than the
	// slot it lives in, so the slot's centre is what the padding is measured
	// from: half a glyph and the padding in from the edge.
	int star_centre = V1_W - (slots[0] + AAM_HEAD_SLOT / 2);
	CHECK(V1_W - (star_centre + AAM_HEAD_GLYPH / 2) == AAM_HEAD_END_PAD);

	// The three marks all draw at one size: the star's canvas is 32 px and the
	// mode's 30, and at their native sizes the mode is the heavier of the two.
	CHECK(AAM_HEAD_GLYPH < 30);
	CHECK(AAM_HEAD_GLYPH > 16);

	// And they keep air between them. Six pixels of black between one glyph and
	// the next looked like three marks crowding each other on the device; the
	// pitch is what says how much of it the row is given, so it is the pitch
	// that is held to it.
	CHECK(AAM_HEAD_PITCH - AAM_HEAD_GLYPH >= 10);

	// The format line still has room beside them. On the V1 the marks begin at
	// 142 and the line starts at 46, which leaves 96 px for "16/44.1FLAC" --
	// 84 px at the size it is drawn. This is the check that would have caught
	// the row being laid out by eye, and the one that stops the air between the
	// marks from being taken out of this line.
	CHECK(aam_head_marks_left(V1_W) == 142);
	CHECK(aam_head_marks_left(V1_W) - AAM_FORMAT_X >= 92);
	(void)star_centre;
}

static void test_the_strip_shares_its_room(void) {
	int out[2];

	// Both names fit: each gets exactly its own words, so neither scrolls.
	// This is the case the arrangement was asked for -- a short artist and a
	// short title on a 240 px panel -- and the bug it fixes: a third of the
	// row each, regardless of what the words needed.
	aam_strip_shares(160, 55, 90, out);
	CHECK(out[0] == 55);
	CHECK(out[1] == 90);

	// A long title beside a short artist: the artist keeps its words whole and
	// the title takes the remainder rather than being given a third of a row.
	aam_strip_shares(160, 30, 240, out);
	CHECK(out[0] == 30);
	CHECK(out[1] == 130);

	// And the same the other way round: a long artist does not crush the title.
	aam_strip_shares(160, 240, 30, out);
	CHECK(out[1] == 30);
	CHECK(out[0] == 130);

	// Neither fits: the row is shared in proportion to the two lengths, so a
	// name twice as long gets twice the room.
	aam_strip_shares(150, 100, 200, out);
	CHECK(out[0] == 50);
	CHECK(out[1] == 100);
	CHECK(out[0] + out[1] == 150);

	// Degenerate answers stay drawable rather than negative.
	aam_strip_shares(0, 40, 40, out);
	CHECK(out[0] >= 0 && out[1] >= 0);
	CHECK(out[0] + out[1] <= 1);
	aam_strip_shares(120, -5, -5, out);
	CHECK(out[0] == 0 && out[1] == 0);
}

int main(void) {
	test_the_strip_shares_its_room();
	test_the_header_marks_stand_on_a_pitch();
	test_the_panel_is_shared_out();
	test_leftie_takes_its_width_off_the_sleeve();
	test_the_sleeve_has_a_floor();
	test_the_taller_header_wins();

	if (failures) {
		fprintf(stderr, "aam layout: %d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("PASS aam layout\n");
	return EXIT_SUCCESS;
}
