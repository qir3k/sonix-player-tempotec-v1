#include "swipedict.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/device/system.h"

// ---------------------------------------------------------------------------
// The lists
//
//   <card>/Dictionaries/en.txt   one word per line, UTF-8, nothing else
//   <card>/Dictionaries/pl.txt
//
// Read whole and split, rather than line by line: the file is small enough to
// hold (capped below) and a whole-file read is one pass over the card instead
// of one per line. Words keep the spelling the file gave them -- the Polish
// list's diacritics are what get typed -- and are matched against the keys with
// those folded away, because the keys are Latin letters.
//
// The cap is what a swipe may cost in memory: 384 kB a language, three thousand
// words at the least. A list past it is read up to the cap and the rest
// ignored, which is better than a card read that fills the heap.
// ---------------------------------------------------------------------------

#define SWIPE_LIST_MAX_BYTES (384 * 1024)
#define SWIPE_WORD_MAX 40
#define SWIPE_WORDS_MAX 65535

typedef struct {
	uint32_t offset;
	uint8_t length;
} word_t;

typedef struct {
	char *pool;		// the file's text, NUL at each word's end
	uint32_t pool_used;
	word_t *words;
	int count;
	int capacity;
	bool tried;		// a load was attempted (found or not)
	bool found;
} list_t;

static list_t lists[2]; // 0 = English, 1 = Polish
static char root[512];
static char status[96];

void swipedict_forget(void) {
	for (int i = 0; i < 2; i++) {
		free(lists[i].pool);
		free(lists[i].words);
		memset(&lists[i], 0, sizeof(lists[i]));
	}
	status[0] = '\0';
}

void swipedict_set_root(const char *sd_root) {
	if (!sd_root || !*sd_root) {
		root[0] = '\0';
		swipedict_forget();
		return;
	}
	if (strcmp(root, sd_root) == 0) {
		return; // the same card: whatever was loaded is still good
	}
	snprintf(root, sizeof(root), "%s", sd_root);
	swipedict_forget();
}

// 0 = English, 1 = Polish, 2 = both. Kept in [keyboard] beside the switch, so
// the keyboard and the settings page read the same answer.
static int language_choice(void) {
	int choice = (int)config_get_int("keyboard", "swipe_lang", 0);
	return choice < 0 ? 0 : (choice > 2 ? 2 : choice);
}

// The letters a word is matched by: the Latin letters the keys carry. A Polish
// word keeps its own spelling in the list and is matched as its unaccented
// self, so a path over l-a-k-a finds "łąka" and types it with the tail.
static char fold_codepoint(const unsigned char *s, int *consumed) {
	*consumed = 1;
	if (s[0] < 0x80) {
		char c = (char)s[0];
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		return (c >= 'a' && c <= 'z') ? c : 0;
	}

	// Two-byte Latin letters: the C3/C4/C5 block, which is where Polish and the
	// other European diacritics live.
	if (s[0] == 0xC3 && s[1]) {
		*consumed = 2;
		switch (s[1]) {
		case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA5: case 0xA6: return 'a'; // à á â ã å æ
		case 0xA7: return 'c';														  // ç
		case 0xA8: case 0xA9: case 0xAA: case 0xAB: return 'e';						  // è é ê ë
		case 0xAC: case 0xAD: case 0xAE: case 0xAF: return 'i';						  // ì í î ï		case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB8: return 'o'; // ò ó ô õ ø
		case 0xB0: return 'd'; // ð
		case 0xB1: return 'n'; // ñ
		case 0xB6: case 0xB9: case 0xBA: case 0xBB: case 0xBC: return 'u'; // ö ù ú û ü
		case 0xBD: return 'y'; // ý
		default: return 0;
		}
	}
	if (s[0] == 0xC4 && s[1]) {
		*consumed = 2;
		if (s[1] == 0x85 || s[1] == 0x84) { // ą / Ą
			return 'a';
		}
		if (s[1] == 0x87 || s[1] == 0x86) { // ć / Ć
			return 'c';
		}
		if (s[1] == 0x99 || s[1] == 0x98) { // ę / Ę
			return 'e';
		}
		return 0;
	}
	if (s[0] == 0xC5 && s[1]) {
		*consumed = 2;
		if (s[1] == 0x82 || s[1] == 0x81) { // ł / Ł
			return 'l';
		}
		if (s[1] == 0x84 || s[1] == 0x83) { // ń / Ń
			return 'n';
		}
		if (s[1] == 0x9B || s[1] == 0x9A) { // ś / Ś
			return 's';
		}
		if (s[1] == 0xBA || s[1] == 0xB9) { // ź / Ź
			return 'z';
		}
		if (s[1] == 0xBC || s[1] == 0xBB) { // ż / Ż
			return 'z';
		}
		return 0;
	}
	// Anything else (Cyrillic, CJK) is not typed by a swipe: this keyboard's
	// swipe is Latin only, and a list with other letters in it is read past.
	*consumed = 1;
	while ((s[*consumed] & 0xC0) == 0x80) {
		(*consumed)++;
	}
	return 0;
}

// A word folded to the letters the keys carry, or 0 characters when it holds
// anything that is not a Latin letter.
static int fold_word(const char *word, int length, char *out, int out_size) {
	int used = 0;
	int at = 0;
	while (at < length) {
		int taken = 1;
		char c = fold_codepoint((const unsigned char *)word + at, &taken);
		if (!c) {
			return 0;
		}
		if (used + 1 >= out_size) {
			return 0; // longer than any word this will store, so not one of them
		}
		out[used++] = c;
		at += taken;
	}
	out[used] = '\0';
	return used;
}

static bool ensure_words(list_t *list, int need) {
	if (list->capacity >= need) {
		return true;
	}
	int wanted = list->capacity ? list->capacity : 4096;
	while (wanted < need) {
		wanted *= 2;
	}
	word_t *grown = realloc(list->words, (size_t)wanted * sizeof(*grown));
	if (!grown) {
		return false;
	}
	list->words = grown;
	list->capacity = wanted;
	return true;
}

// Reads one list. A file that is not there is not a failure worth logging:
// the settings row says which lists are in memory.
static void load_list(int index, const char *name) {
	list_t *list = &lists[index];
	if (list->tried) {
		return;
	}
	list->tried = true;
	if (!root[0]) {
		return;
	}

	char path[600];
	snprintf(path, sizeof(path), "%s/Dictionaries/%s.txt", root, name);
	FILE *file = fopen(path, "rb");
	if (!file) {
		return;
	}

	list->pool = malloc(SWIPE_LIST_MAX_BYTES + 1);
	if (!list->pool) {
		fclose(file);
		return;
	}
	size_t got = fread(list->pool, 1, SWIPE_LIST_MAX_BYTES, file);
	fclose(file);
	list->pool[got] = '\0';

	char folded[SWIPE_WORD_MAX + 4];
	char *at = list->pool;
	char *end = list->pool + got;
	uint32_t used = 0; // where the kept words are packed, at the front of the same buffer
	while (at < end && list->count < SWIPE_WORDS_MAX) {
		char *line = at;
		char *newline = memchr(at, '\n', (size_t)(end - at));
		at = newline ? newline + 1 : end;
		int length = (int)((newline ? newline : end) - line);
		if (length > 0 && line[length - 1] == '\r') {
			length--;
		}
		if (length <= 1 || length > SWIPE_WORD_MAX) {
			continue;
		}
		// The first field only: a list with counts or parts of speech beside the
		// word ("word 1234") still reads as its word.
		for (int i = 0; i < length; i++) {
			if (line[i] == ' ' || line[i] == '\t' || line[i] == ';') {
				length = i;
				break;
			}
		}
		if (length <= 1 || !fold_word(line, length, folded, (int)sizeof(folded))) {
			continue;
		}
		if (!ensure_words(list, list->count + 1)) {
			break;
		}
		// The kept words are packed at the front of the file's own buffer, and
		// each one gets a NUL where its newline was. That is safe forwards only
		// because nothing is skipped without bytes being read: the write never
		// gets past the read, so a copy only ever moves a word back over text
		// that has already been scanned -- hence the byte loop and not memcpy.
		if (used + (uint32_t)length + 1 > got) {
			break;
		}
		for (int i = 0; i < length; i++) {
			list->pool[used + (uint32_t)i] = line[i];
		}
		list->pool[used + (uint32_t)length] = '\0';
		list->words[list->count].offset = used;
		list->words[list->count].length = (uint8_t)length;
		list->count++;
		used += (uint32_t)length + 1;
	}
	list->pool_used = used;
	list->found = list->count > 0;
}

bool swipedict_load(void) {
	if (!root[0]) {
		// First use: the card is mounted by now, whatever boot order this
		// happened in.
		swipedict_set_root(storage_sd_root());
	}

	int choice = language_choice();
	bool need_en = (choice == 0 || choice == 2);
	bool need_pl = (choice == 1 || choice == 2);

	if (need_en) {
		load_list(0, "en");
	}
	if (need_pl) {
		load_list(1, "pl");
	}

	int total = 0;
	status[0] = '\0';
	if (need_en && lists[0].count > 0) {
		total += lists[0].count;
		snprintf(status + strlen(status), sizeof(status) - strlen(status), "en %d", lists[0].count);
	}
	if (need_pl && lists[1].count > 0) {
		total += lists[1].count;
		snprintf(status + strlen(status), sizeof(status) - strlen(status), "%spl %d", status[0] ? ", " : "",
				 lists[1].count);
	}
	if (total == 0) {
		// Named, so the settings row can say what to put on the card rather than
		// just "off".
		snprintf(status, sizeof(status), "%s", need_pl ? (need_en ? "en, pl -- none" : "pl -- none") : "en -- none");
	}
	return total > 0;
}

const char *swipedict_status(void) { return status; }

// ---------------------------------------------------------------------------
// The decoder
//
// A path is sampled while the finger moves; what has to be decided is which
// word the keys under it spell. Three cheap passes, in order of what throws the
// most away for the least work:
//
//   1. every key's distance to the nearest point of the path, once, and the
//      sequence of keys the path passes over (repeats collapsed);
//   2. a word must spell itself out in that sequence, in order -- which throws
//      away nearly the whole list without measuring anything;
//   3. what is left is scored on how close the path came to each of its letters
//      and on how long it is against how many keys were visited.
//
// That keeps the cost at a few milliseconds for a list of tens of thousands of
// words, on the release of the swipe, which is the only time it runs.
// ---------------------------------------------------------------------------

#define SWIPE_KEY_MAX 40

typedef struct {
	int index[SWIPE_KEY_MAX]; // key index per folded letter, -1 when there is no such key
	uint32_t near_d2[SWIPE_KEY_MAX];
} key_map_t;

static int key_for_letter(const swipe_key_t *keys, int key_count, char letter) {
	for (int i = 0; i < key_count; i++) {
		int taken = 1;
		char folded = fold_codepoint((const unsigned char *)keys[i].cap, &taken);
		if (folded && folded == letter) {
			return i;
		}
	}
	return -1;
}

bool swipedict_decode(const swipe_key_t *keys, int key_count, const swipe_point_t *path, int path_count, char *out,
					  size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!keys || key_count <= 0 || !path || path_count < 2 || !swipedict_load()) {
		return false;
	}

	static key_map_t map;
	for (int i = 0; i < SWIPE_KEY_MAX; i++) {
		map.index[i] = -1;
		map.near_d2[i] = 0xFFFFFFFFu;
	}

	// 1a. How near the path came to each key.
	for (int k = 0; k < key_count && k < SWIPE_KEY_MAX; k++) {
		uint32_t best = 0xFFFFFFFFu;
		for (int p = 0; p < path_count; p++) {
			int dx = path[p].x - keys[k].cx;
			int dy = path[p].y - keys[k].cy;
			uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
			if (d2 < best) {
				best = d2;
			}
		}
		int taken = 1;
		char letter = fold_codepoint((const unsigned char *)keys[k].cap, &taken);
		if (!letter) {
			continue;
		}
		int slot = letter - 'a';
		if (slot >= 0 && slot < SWIPE_KEY_MAX) {
			map.index[slot] = k;
			map.near_d2[slot] = best;
		}
	}

	// 1b. The keys the path runs over, in order, without repeats.
	int visit[96];
	int visits = 0;
	for (int p = 0; p < path_count; p++) {
		int nearest = -1;
		uint32_t best = 0xFFFFFFFFu;
		for (int k = 0; k < key_count; k++) {
			int dx = path[p].x - keys[k].cx;
			int dy = path[p].y - keys[k].cy;
			uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
			if (d2 < best) {
				best = d2;
				nearest = k;
			}
		}
		if (nearest >= 0 && (visits == 0 || visit[visits - 1] != nearest) && visits < (int)(sizeof(visit) / sizeof(visit[0]))) {
			visit[visits++] = nearest;
		}
	}
	if (visits < 2) {
		return false;
	}

	int choice = language_choice();
	int first_list = (choice == 1) ? 1 : 0;
	int last_list = (choice == 2) ? 1 : first_list;

	const word_t *best_word = NULL;
	const list_t *best_list = NULL;
	uint32_t best_score = 0xFFFFFFFFu;

	char folded[SWIPE_WORD_MAX + 4];
	for (int which = first_list; which <= last_list; which++) {
		const list_t *list = &lists[which];
		if (!list->found) {
			continue;
		}
		for (int w = 0; w < list->count; w++) {
			const word_t *word = &list->words[w];
			int length = fold_word(list->pool + word->offset, word->length, folded, (int)sizeof(folded));
			if (length == 0) {
				continue;
			}

			// 2. The word has to be spelled out by the keys the path crossed.
			int at = 0;
			int used = 0;
			for (int i = 0; i < length && at < visits; i++) {
				int slot = folded[i] - 'a';
				if (slot < 0 || slot >= SWIPE_KEY_MAX || map.index[slot] < 0) {
					at = visits; // a letter no key carries: not this word
					break;
				}
				while (at < visits && visit[at] != map.index[slot]) {
					at++;
				}
				if (at == visits) {
					break;
				}
				at++;
				used++;
			}
			if (used != length) {
				continue;
			}

			// 3. Score what is left: how close the path came to every letter of
			// the word, plus what it costs to be a different length from the
			// number of keys the path crossed.
			uint32_t total = 0;
			for (int i = 0; i < length; i++) {
				total += map.near_d2[folded[i] - 'a'];
			}
			uint32_t average = total / (uint32_t)length;
			int difference = length > visits ? length - visits : visits - length;
			uint32_t score = average + (uint32_t)difference * 220u;
			if (score < best_score) {
				best_score = score;
				best_word = word;
				best_list = list;
			}
		}
	}

	// A path that came nowhere near the letters it would have to have crossed
	// spells nothing. 50 px on average is roughly two key gaps on the 240 px
	// panel, so anything past it is a drag, not a word.
	if (!best_word || best_score > 2500u) {
		return false;
	}

	snprintf(out, out_size, "%.*s", (int)best_word->length, best_list->pool + best_word->offset);
	return true;
}
