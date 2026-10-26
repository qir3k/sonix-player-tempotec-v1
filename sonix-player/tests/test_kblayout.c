// test_kblayout.c -- which accented letter a held key offers first, per layout.
//
// The strip of accents on a held key is one fixed list per letter; the order it
// is shown in belongs to the layout being typed in. Holding E in Polish has to
// offer ę before é, holding A in German ä before à, and so on -- the same
// letters in every language, only the order moving. Checked here against the
// real kblayout.c with a private config file, so the [keyboard] swipe_lang
// override (a leftover of the swipe typing, but still honoured) can be set and
// cleared from the tests themselves.

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/input/kblayout.h"

// kblayout.c asks the interface language which layout is the player's own; the
// tests load no language pack, so the answer is fixed here.
const char *lang_current(void) { return "English"; }

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                        \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                     \
	} while (0)

// The first variant of `key` on `layout` equals `want`.
static void check_first(kblayout_t layout, const char *key, const char *want, int line) {
	const char *got = kblayout_variant(layout, key, 0);
	if (strcmp(got, want) != 0) {
		failures++;
		fprintf(stderr, "FAIL %s:%d: first variant of %s on layout %d is \"%s\", want \"%s\"\n", __FILE__, line,
				key, (int)layout, got, want);
	}
}

static void set_swipe(int value) { config_set_int("keyboard", "swipe_lang", value); }

// The strip of `key` on `layout` holds every variant the table has for it --
// reordering may demote, never drop. Verified against kblayout_variant_count().
static void check_nothing_lost(kblayout_t layout, const char *key) {
	int count = kblayout_variant_count(key);
	int shown = 0;
	while (shown < count && kblayout_variant(layout, key, shown)[0]) {
		shown++;
	}
	if (shown != count) {
		failures++;
		fprintf(stderr, "FAIL %s:%d: %s on layout %d shows %d of %d variants\n", __FILE__, __LINE__, key,
				(int)layout, shown, count);
	}
}

static void test_polish_nationals_come_first(void) {
	check_first(KB_LAYOUT_POLISH, "o", "\xc3\xb3", __LINE__);  // ó
	check_first(KB_LAYOUT_POLISH, "a", "\xc4\x85", __LINE__);  // ą
	check_first(KB_LAYOUT_POLISH, "e", "\xc4\x99", __LINE__);  // ę
	check_first(KB_LAYOUT_POLISH, "s", "\xc5\x9b", __LINE__);  // ś
	check_first(KB_LAYOUT_POLISH, "z", "\xc5\xbc", __LINE__); // ż before ź
	check_first(KB_LAYOUT_POLISH, "c", "\xc4\x87", __LINE__); // ć
	check_first(KB_LAYOUT_POLISH, "n", "\xc5\x84", __LINE__);  // ń
	check_first(KB_LAYOUT_POLISH, "l", "\xc5\x82", __LINE__);  // ł
	check_first(KB_LAYOUT_POLISH, "L", "\xc5\x81", __LINE__);  // Ł, the upper case too
}

static void test_other_layouts_put_their_own_first(void) {
	check_first(KB_LAYOUT_GERMAN, "a", "\xc3\xa4", __LINE__);	  // ä
	check_first(KB_LAYOUT_GERMAN, "o", "\xc3\xb6", __LINE__);	  // ö
	check_first(KB_LAYOUT_GERMAN, "u", "\xc3\xbc", __LINE__);	  // ü
	check_first(KB_LAYOUT_GERMAN, "s", "\xc3\x9f", __LINE__);	  // ß
	check_first(KB_LAYOUT_FRENCH, "e", "\xc3\xa9", __LINE__);	  // é
	check_first(KB_LAYOUT_FRENCH, "c", "\xc3\xa7", __LINE__);	  // ç
	check_first(KB_LAYOUT_SPANISH, "n", "\xc3\xb1", __LINE__);	  // ñ
	check_first(KB_LAYOUT_SPANISH, "a", "\xc3\xa1", __LINE__);	  // á
	check_first(KB_LAYOUT_ITALIAN, "e", "\xc3\xa8", __LINE__);	  // è
	check_first(KB_LAYOUT_PORTUGUESE, "a", "\xc3\xa3", __LINE__); // ã (não, mãe)
	check_first(KB_LAYOUT_PORTUGUESE, "c", "\xc3\xa7", __LINE__); // ç
}

static void test_english_keeps_the_western_order(void) {
	// English has no nationals of its own: it shares the western European
	// order, the one the accents on borrowed names follow.
	check_first(KB_LAYOUT_ENGLISH, "a", "\xc3\xa1", __LINE__); // á
	check_first(KB_LAYOUT_ENGLISH, "e", "\xc3\xa9", __LINE__); // é
	check_first(KB_LAYOUT_ENGLISH, "i", "\xc3\xad", __LINE__); // í
}

static void test_strips_stay_complete(void) {
	static const char *const keys[] = {"a", "A", "c", "C", "e", "E", "i", "I", "l", "L",
									   "n", "N", "o", "O", "s", "S", "u", "U", "y", "Y",
									   "z", "Z"};
	for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
		for (int layout = 0; layout < KB_LAYOUT_COUNT; layout++) {
			check_nothing_lost((kblayout_t)layout, keys[k]);
		}
	}
}

static void test_swipe_language_override_survives(void) {
	// Polish named in [keyboard] swipe_lang wins over the layout in hand: the
	// setting is where a choice of typed language lives, and Polish is typed on
	// the plain QWERTY rows as much as on its own layout.
	set_swipe(1);
	check_first(KB_LAYOUT_ENGLISH, "o", "\xc3\xb3", __LINE__); // ó, on the English rows
	check_first(KB_LAYOUT_GERMAN, "a", "\xc4\x85", __LINE__);  // ą, even on a German layout
	set_swipe(0);
	// English named in the old setting overrides the layout the same way: this
	// is the one explicit choice the pre-swipe code recognised, kept as it was.
	check_first(KB_LAYOUT_GERMAN, "a", "\xc3\xa1", __LINE__); // á, not ä
	set_swipe(-1); // not there: the layout decides
	check_first(KB_LAYOUT_GERMAN, "a", "\xc3\xa4", __LINE__);
	check_first(KB_LAYOUT_POLISH, "o", "\xc3\xb3", __LINE__);
}

int main(void) {
	// A private config file: the tests set [keyboard] swipe_lang themselves, and
	// nothing here may read whatever the host user happens to have. An empty
	// file is enough -- config_init() reports false for one it had to create,
	// and the in-memory defaults work all the same.
	FILE *seed = fopen(".build/test_kblayout_config.ini", "w");
	if (seed) {
		fclose(seed);
	}
	config_init(".build/test_kblayout_config.ini");
	set_swipe(-1); // no override: the layout decides, which is the case under test

	test_polish_nationals_come_first();
	test_other_layouts_put_their_own_first();
	test_english_keeps_the_western_order();
	test_strips_stay_complete();
	test_swipe_language_override_survives();

	if (failures) {
		fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_kblayout: ok\n");
	return 0;
}
