#include "coverart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif

#include "src/system/core/json.h"
#include "src/system/net/http.h"

#ifndef COVERART_ITUNES_API
#define COVERART_ITUNES_API "https://itunes.apple.com/search"
#endif
#ifndef COVERART_MUSICBRAINZ_API
#define COVERART_MUSICBRAINZ_API "https://musicbrainz.org/ws/2/release-group"
#endif
#ifndef COVERART_ARCHIVE
#define COVERART_ARCHIVE "https://coverartarchive.org/release-group"
#endif

#define COVERART_TIMEOUT_SECS 20
#define COVERART_ATTEMPTS 2
#define COVERART_RETRY_PAUSE_SECS 2
// A page of search results is a few kilobytes; ten albums at most are kept.
#define COVERART_MAX_JSON_BYTES (256 * 1024)
#define COVERART_MAX_HITS 10
// A cover this size or larger is not taken: the decoder would refuse it on this
// device anyway, and a longer answer is cut off by http_get_ex() so it cannot
// be told apart from a whole one.
#define COVERART_MAX_IMAGE_BYTES (3 * 1024 * 1024)
// The Store hands out the picture at whatever size is in its URL. 600 is
// sharp on a 240-wide screen and small enough to decode quickly.
#define COVERART_ITUNES_SIZE "600x600bb"

static char contact[100];

void coverart_set_contact(const char *text) {
	contact[0] = '\0';
	while (text && (*text == ' ' || *text == '\t')) {
		text++;
	}
	size_t w = 0;
	for (; text && *text && w + 1 < sizeof(contact); text++) {
		// Parentheses would end the comment the contact sits in.
		if ((unsigned char)*text >= ' ' && *text != '(' && *text != ')') {
			contact[w++] = *text;
		}
	}
	while (w > 0 && contact[w - 1] == ' ') {
		w--;
	}
	contact[w] = '\0';
}

bool coverart_has_contact(void) { return contact[0] != '\0'; }
const char *coverart_contact(void) { return contact; }

// GET with the retries above. A 4xx is an answer and is not asked again.
static bool api_get(const char *url, size_t limit, char **body, size_t *len) {
	for (int attempt = 1; attempt <= COVERART_ATTEMPTS; attempt++) {
		int status = 0;
		if (http_get_ex(url, NULL, false, body, len, limit, COVERART_TIMEOUT_SECS, &status)) {
			return true;
		}
		bool transient = status <= 0 || status == 429 || status >= 500;
		const char *why = http_last_error();
		fprintf(stderr, "coverart: attempt %d/%d failed (HTTP %d%s%s)\n", attempt, COVERART_ATTEMPTS, status, why ? ": " : "", why ? why : "");
		if (!transient || attempt == COVERART_ATTEMPTS) {
			break;
		}
		sleep((unsigned)(COVERART_RETRY_PAUSE_SECS * attempt));
	}
	return false;
}

// "<album> <artist>" for the Store, which searches free text.
static bool itunes_url(char *out, size_t size, const char *album, const char *artist) {
	char words[400], q[1300];
	snprintf(words, sizeof(words), "%s %s", album, artist ? artist : "");
	http_url_encode(words, q, sizeof(q));
	int n = snprintf(out, size, COVERART_ITUNES_API "?media=music&entity=album&limit=%d&term=%s", COVERART_MAX_HITS, q);
	return n > 0 && (size_t)n < size;
}

// The Store's artwork URL names its size ("…/100x100bb.jpg"); the same path with
// another size is the picture at that size.
static bool itunes_big(const char *small, char *out, size_t size) {
	const char *tail = strstr(small, "100x100bb");
	if (!tail) {
		return false;
	}
	int n = snprintf(out, size, "%.*s%s%s", (int)(tail - small), small, COVERART_ITUNES_SIZE, tail + strlen("100x100bb"));
	return n > 0 && (size_t)n < size;
}

static void itunes_search(const char *album, const char *artist, coverart_hit_t *list, int *count, bool *answered) {
	char url[1500];
	if (!itunes_url(url, sizeof(url), album, artist)) {
		return;
	}
	char *body = NULL;
	if (!api_get(url, COVERART_MAX_JSON_BYTES, &body, NULL)) {
		return;
	}
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int results = json_get(&doc, json_root(&doc), "results");
		if (results >= 0 && doc.toks[results].type == JSON_ARRAY) {
			*answered = true;
			int total = json_len(&doc, results);
			for (int i = 0; i < total && *count < COVERART_MAX_HITS; i++) {
				int rec = json_at(&doc, results, i);
				if (rec < 0 || doc.toks[rec].type != JSON_OBJECT) {
					continue;
				}
				char art[300];
				coverart_hit_t *h = &list[*count];
				memset(h, 0, sizeof(*h));
				if (!json_obj_str(&doc, rec, "artworkUrl100", art, sizeof(art)) || !itunes_big(art, h->url, sizeof(h->url))) {
					continue;
				}
				json_obj_str(&doc, rec, "collectionName", h->album, sizeof(h->album));
				json_obj_str(&doc, rec, "artistName", h->artist, sizeof(h->artist));
				char date[32] = "";
				json_obj_str(&doc, rec, "releaseDate", date, sizeof(date));
				snprintf(h->year, sizeof(h->year), "%.4s", date);
				if (h->album[0]) {
					(*count)++;
				}
			}
		}
		json_free(&doc);
	}
	free(body);
}

// A value for a Lucene phrase: its quotes and backslashes dropped, since they
// would end the phrase.
static void phrase(const char *in, char *out, size_t size) {
	size_t w = 0;
	for (; *in && w + 1 < size; in++) {
		if (*in != '"' && *in != '\\') {
			out[w++] = *in;
		}
	}
	out[w] = '\0';
}

static bool musicbrainz_url(char *out, size_t size, const char *album, const char *artist) {
	char a[200], b[200], query[500], q[1600];
	phrase(album, a, sizeof(a));
	phrase(artist ? artist : "", b, sizeof(b));
	if (b[0]) {
		snprintf(query, sizeof(query), "releasegroup:\"%s\" AND artist:\"%s\"", a, b);
	} else {
		snprintf(query, sizeof(query), "releasegroup:\"%s\"", a);
	}
	http_url_encode(query, q, sizeof(q));
	int n = snprintf(out, size, COVERART_MUSICBRAINZ_API "?fmt=json&limit=%d&query=%s", COVERART_MAX_HITS, q);
	return n > 0 && (size_t)n < size;
}

static void musicbrainz_search(const char *album, const char *artist, coverart_hit_t *list, int *count, bool *answered) {
	char url[1900];
	if (!musicbrainz_url(url, sizeof(url), album, artist)) {
		return;
	}
	char *body = NULL;
	char agent[sizeof(contact) + 4];
	snprintf(agent, sizeof(agent), " (%s)", contact);
	http_set_user_agent_suffix(agent);
	bool got = api_get(url, COVERART_MAX_JSON_BYTES, &body, NULL);
	http_set_user_agent_suffix(NULL);
	if (!got) {
		return;
	}
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int groups = json_get(&doc, json_root(&doc), "release-groups");
		if (groups >= 0 && doc.toks[groups].type == JSON_ARRAY) {
			*answered = true;
			int total = json_len(&doc, groups);
			for (int i = 0; i < total && *count < COVERART_MAX_HITS; i++) {
				int rec = json_at(&doc, groups, i);
				if (rec < 0 || doc.toks[rec].type != JSON_OBJECT) {
					continue;
				}
				char mbid[48];
				coverart_hit_t *h = &list[*count];
				memset(h, 0, sizeof(*h));
				if (!json_obj_str(&doc, rec, "id", mbid, sizeof(mbid)) || !json_obj_str(&doc, rec, "title", h->album, sizeof(h->album))) {
					continue;
				}
				int credits = json_get(&doc, rec, "artist-credit");
				int first = credits >= 0 && doc.toks[credits].type == JSON_ARRAY ? json_at(&doc, credits, 0) : -1;
				if (first >= 0 && doc.toks[first].type == JSON_OBJECT) {
					json_obj_str(&doc, first, "name", h->artist, sizeof(h->artist));
				}
				char date[32] = "";
				json_obj_str(&doc, rec, "first-release-date", date, sizeof(date));
				snprintf(h->year, sizeof(h->year), "%.4s", date);
				// Not every release group has a picture in the Archive; the hit
				// is listed and a miss shows when it is tapped.
				snprintf(h->url, sizeof(h->url), COVERART_ARCHIVE "/%s/front-500", mbid);
				(*count)++;
			}
		}
		json_free(&doc);
	}
	free(body);
}

// How likely a hit is to be the one asked for: the same album title above all,
// then the same artist.
static int rank_of(const coverart_hit_t *h, const char *album, const char *artist) {
	int score = 0;
	if (strcasecmp(h->album, album) == 0) {
		score += 2;
	}
	if (artist && artist[0] && strcasecmp(h->artist, artist) == 0) {
		score += 1;
	}
	return score;
}

// Best first, an insertion sort so the source's own order holds between equals.
static void rank(coverart_hit_t *list, int count, const char *album, const char *artist) {
	for (int i = 1; i < count; i++) {
		coverart_hit_t moving = list[i];
		int score = rank_of(&moving, album, artist);
		int j = i;
		while (j > 0 && rank_of(&list[j - 1], album, artist) < score) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = moving;
	}
}

coverart_result_t coverart_search(const char *album, const char *artist, coverart_hit_t **hits, int *count) {
	*hits = NULL;
	*count = 0;
	if (!album || !album[0]) {
		return COVERART_NO_TAGS;
	}

	coverart_hit_t *list = calloc(COVERART_MAX_HITS, sizeof(*list));
	if (!list) {
		return COVERART_NETWORK;
	}
	int found = 0;
	bool answered = false;
	itunes_search(album, artist, list, &found, &answered);
	if (found == 0 && coverart_has_contact()) {
		musicbrainz_search(album, artist, list, &found, &answered);
	}
	if (found == 0) {
		free(list);
		fprintf(stderr, "coverart: '%s' by '%s': nothing (%s)\n", album, artist ? artist : "", answered ? "not found" : "no answer");
		return answered ? COVERART_NOT_FOUND : COVERART_NETWORK;
	}

	rank(list, found, album, artist);
	fprintf(stderr, "coverart: '%s' by '%s': %d albums\n", album, artist ? artist : "", found);
	*hits = list;
	*count = found;
	return COVERART_OK;
}

bool coverart_fetch(const char *url, uint8_t **data, size_t *len) {
	*data = NULL;
	*len = 0;
	char *body = NULL;
	size_t size = 0;
	if (!api_get(url, COVERART_MAX_IMAGE_BYTES, &body, &size)) {
		return false;
	}
	const uint8_t *b = (const uint8_t *)body;
	bool jpeg = size > 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
	bool png = size > 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0;
	if (size >= COVERART_MAX_IMAGE_BYTES || (!jpeg && !png)) {
		fprintf(stderr, "coverart: %s is not a usable picture (%zu bytes)\n", url, size);
		free(body);
		return false;
	}
	*data = (uint8_t *)body;
	*len = size;
	return true;
}

// Written to a temporary name and renamed, so a card pulled halfway leaves no
// half picture for the cover loader to find.
bool coverart_save(const char *track_path, const uint8_t *data, size_t len) {
	if (!track_path || !data || len < 8) {
		return false;
	}
	const char *slash = strrchr(track_path, '/');
	int dir_len = slash ? (int)(slash - track_path) : 0;
	const char *ext = data[0] == 0xFF ? "jpg" : "png";

	char target[1100], temp[1110];
	int n = snprintf(target, sizeof(target), "%.*s/cover.%s", dir_len, track_path, ext);
	if (n < 0 || (size_t)n >= sizeof(target)) {
		return false;
	}
	snprintf(temp, sizeof(temp), "%s.part", target);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(temp, target) != 0) {
		unlink(temp);
		fprintf(stderr, "coverart: could not write %s\n", target);
		return false;
	}
	fprintf(stderr, "coverart: saved %s (%zu bytes)\n", target, len);
	return true;
}
