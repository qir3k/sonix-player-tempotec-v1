#ifndef SWIPEDICT_H
#define SWIPEDICT_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Swipe typing
//
// The keyboard's own dictionaries are the word lists on the card -- plain text,
// one word per line, in <card>/Dictionaries/en.txt and pl.txt -- and this is
// what reads them and what turns a finger's path across the keys into a word.
//
// Nothing here knows about LVGL: the keyboard hands over the keys it has on
// screen (their letters and their centres, in the same coordinates it sampled
// the finger in) and gets a word back. That keeps the decoder testable and keeps
// the list of languages out of the interface.
//
// The lists are not shipped: they are somebody else's file. A missing list is
// not an error -- swipe typing then simply types nothing, and the settings row
// says which lists it found.
// ---------------------------------------------------------------------------

// A key as the decoder sees it: the letter it types, lower case, and where it
// is. `cap` holds a UTF-8 string; a key whose letter is not a Latin letter is
// left out by the caller.
#define SWIPE_KEY_CAP 8

typedef struct {
	char cap[SWIPE_KEY_CAP];
	int cx, cy;
} swipe_key_t;

typedef struct {
	int x, y;
} swipe_point_t;

// Where the lists live. Safe to call more than once; a different root drops
// whatever was loaded.
void swipedict_set_root(const char *sd_root);

// Reads the lists the chosen language needs, if they are not in memory already.
// Returns true when at least one list is ready. Called from the first swipe and
// from the settings page, so the cost (a few hundred kilobytes off the card) is
// paid once and not while a finger is on the glass.
bool swipedict_load(void);

// Drops the lists: the language choice changed, or the card did.
void swipedict_forget(void);

// What the settings row shows: "" when nothing is loaded, otherwise which lists
// are in memory and how many words they hold. Never NULL, never freed by the
// caller.
const char *swipedict_status(void);

// The word the path most likely spells, written into `out`. False when nothing
// plausible was found -- a stray drag across the keyboard must type nothing
// rather than the nearest word in the list.
bool swipedict_decode(const swipe_key_t *keys, int key_count, const swipe_point_t *path, int path_count, char *out,
					  size_t out_size);

#endif /* SWIPEDICT_H */
