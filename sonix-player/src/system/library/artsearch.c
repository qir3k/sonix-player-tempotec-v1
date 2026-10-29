#include "artsearch.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/json.h"
#include "src/system/library/tagwrite.h"
#include "src/system/net/http.h"

// The two catalogues. Both answer a plain GET with a JSON body and need no key
// and no account, which is the whole reason they are the two.
#ifndef ARTSEARCH_DEEZER_API
#define ARTSEARCH_DEEZER_API "https://api.deezer.com"
#endif
#ifndef ARTSEARCH_ITUNES_API
#define ARTSEARCH_ITUNES_API "https://itunes.apple.com"
#endif

// A search reply is tens of kilobytes; the ceilings are generous because a
// reply that is cut off cannot be parsed at all, and a picture is allowed to be
// a few megabytes -- a 1000px cover is one.
#define ARTSEARCH_JSON_LIMIT (768 * 1024)
#define ARTSEARCH_IMAGE_LIMIT (5 * 1024 * 1024)
#define ARTSEARCH_SEARCH_TIMEOUT 20
#define ARTSEARCH_IMAGE_TIMEOUT 30

// Catalogues are asked one after the other on the same worker, and both use
// http.c's per-thread state, so nothing else here needs a lock.
static void set_error(char *out, size_t size, const char *reason) {
	if (out && size) {
		snprintf(out, size, "%s", reason ? reason : "artwork_failed");
	}
}

// One GET, with Deezer's own preference for a shorter wait: a catalogue that
// does not answer in twenty seconds is not going to.
static bool api_get(const char *url, char **body, size_t *len, char *error, size_t error_size) {
	int status = 0;
	if (!http_get_ex(url, NULL, true, body, len, ARTSEARCH_JSON_LIMIT, ARTSEARCH_SEARCH_TIMEOUT, &status)) {
		const char *why = http_last_error();
		fprintf(stderr, "artsearch: GET %s failed (HTTP %d%s%s)\n", url, status, why ? ": " : "",
				why ? why : "");
		set_error(error, error_size, "artwork_failed");
		return false;
	}
	if (status < 200 || status >= 300) {
		// A 4xx keeps its body, which is where the catalogue says why.
		fprintf(stderr, "artsearch: GET %s answered %d\n", url, status);
		set_error(error, error_size, "artwork_failed");
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

// A string field of one JSON object, copied out. False when it is missing,
// null or empty.
static bool hit_text(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	int tok = json_get(doc, obj, key);
	return tok >= 0 && out && size && json_str(doc, tok, out, size) && out[0];
}

// The first four digits of an ISO date ("2001-05-21T07:00:00Z"), or 0.
static int year_of(const char *date) {
	if (!date || strlen(date) < 4) {
		return 0;
	}
	int year = 0;
	for (int i = 0; i < 4; i++) {
		if (date[i] < '0' || date[i] > '9') {
			return 0;
		}
		year = year * 10 + (date[i] - '0');
	}
	return year >= 1900 && year <= 2100 ? year : 0;
}

// Deezer's own sizing lives in the file name (".../1000x1000-000000-80-0-0.jpg"),
// and it publishes a named size for each step, so the step is what is asked
// for: cover_medium for the row, cover_xl for the file.
static int deezer_hits(const char *query, artsearch_hit_t *out, int limit) {
	char q[1024];
	http_url_encode(query, q, sizeof(q));
	char url[1400];
	int n = snprintf(url, sizeof(url), ARTSEARCH_DEEZER_API "/search/album?q=%s&limit=%d", q, limit);
	if (n <= 0 || (size_t)n >= sizeof(url)) {
		return 0;
	}
	char *body = NULL;
	size_t len = 0;
	if (!api_get(url, &body, &len, NULL, 0)) {
		return 0;
	}
	int count = 0;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int data = json_path(&doc, json_root(&doc), "data");
		int total = json_len(&doc, data);
		for (int i = 0; i < total && count < limit; i++) {
			int item = json_at(&doc, data, i);
			if (item < 0) {
				continue;
			}
			artsearch_hit_t *hit = &out[count];
			memset(hit, 0, sizeof(*hit));
			if (!hit_text(&doc, item, "title", hit->album, sizeof(hit->album))) {
				continue;
			}
			int artist = json_path(&doc, item, "artist.name");
			if (artist >= 0) {
				json_str(&doc, artist, hit->artist, sizeof(hit->artist));
			}
			if (!hit_text(&doc, item, "cover_xl", hit->full_url, sizeof(hit->full_url))) {
				if (!hit_text(&doc, item, "cover_big", hit->full_url, sizeof(hit->full_url))) {
					continue;
				}
			}
			hit_text(&doc, item, "cover_medium", hit->thumb_url, sizeof(hit->thumb_url));
			if (!hit->thumb_url[0]) {
				snprintf(hit->thumb_url, sizeof(hit->thumb_url), "%s", hit->full_url);
			}
			snprintf(hit->source, sizeof(hit->source), "Deezer");
			count++;
		}
		json_free(&doc);
	}
	free(body);
	return count;
}

// iTunes publishes one artwork URL with its size in it (".../100x100bb.jpg"),
// so the size is what is changed rather than a second URL being asked for.
static void itunes_artwork_url(const char *in, const char *size, char *out, size_t out_size) {
	out[0] = '\0';
	const char *at = strstr(in, "100x100bb");
	if (!at) {
		snprintf(out, out_size, "%s", in);
		return;
	}
	snprintf(out, out_size, "%.*s%s%s", (int)(at - in), in, size, at + strlen("100x100bb"));
}

static int itunes_hits(const char *query, artsearch_hit_t *out, int limit) {
	char q[1024];
	http_url_encode(query, q, sizeof(q));
	char url[1400];
	int n = snprintf(url, sizeof(url), ARTSEARCH_ITUNES_API "/search?term=%s&entity=album&limit=%d", q,
					 limit);
	if (n <= 0 || (size_t)n >= sizeof(url)) {
		return 0;
	}
	char *body = NULL;
	size_t len = 0;
	if (!api_get(url, &body, &len, NULL, 0)) {
		return 0;
	}
	int count = 0;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int results = json_path(&doc, json_root(&doc), "results");
		int total = json_len(&doc, results);
		for (int i = 0; i < total && count < limit; i++) {
			int item = json_at(&doc, results, i);
			if (item < 0) {
				continue;
			}
			artsearch_hit_t *hit = &out[count];
			memset(hit, 0, sizeof(*hit));
			char artwork[ARTSEARCH_URL_MAX];
			if (!hit_text(&doc, item, "collectionName", hit->album, sizeof(hit->album)) ||
				!hit_text(&doc, item, "artworkUrl100", artwork, sizeof(artwork))) {
				continue;
			}
			hit_text(&doc, item, "artistName", hit->artist, sizeof(hit->artist));
			char released[32];
			if (hit_text(&doc, item, "releaseDate", released, sizeof(released))) {
				hit->year = year_of(released);
			}
			itunes_artwork_url(artwork, "600x600bb", hit->full_url, sizeof(hit->full_url));
			snprintf(hit->thumb_url, sizeof(hit->thumb_url), "%s", artwork);
			snprintf(hit->source, sizeof(hit->source), "iTunes");
			count++;
		}
		json_free(&doc);
	}
	free(body);
	return count;
}

// The same record in both catalogues is one answer: the second is dropped on
// its album name, folded the way the library folds one.
static bool already_listed(const artsearch_hit_t *hits, int count, const char *album) {
	if (!album || !album[0]) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		if (strcasecmp(hits[i].album, album) == 0) {
			return true;
		}
	}
	return false;
}

int artsearch_find(const char *artist, const char *album, const char *title, artsearch_hit_t *out,
				   int limit, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!out || limit <= 0 || limit > ARTSEARCH_MAX) {
		return -1;
	}
	if ((!artist || !artist[0]) && (!title || !title[0])) {
		set_error(error, error_size, "artwork_no_tags");
		return -1;
	}
	memset(out, 0, sizeof(*out) * (size_t)limit);

	// The album is the record; the title is only the way in when nobody named
	// one, which is what a hand-tagged file looks like.
	artsearch_hit_t hits[ARTSEARCH_MAX];
	memset(hits, 0, sizeof(hits));
	int count = 0;
	char query[640];
	if (album && album[0]) {
		snprintf(query, sizeof(query), "%s %s", artist ? artist : "", album);
	} else {
		snprintf(query, sizeof(query), "%s %s", artist ? artist : "", title ? title : "");
	}

	count += deezer_hits(query, hits + count, limit - count);
	int from_itunes = itunes_hits(query, out, limit);
	int kept = 0;
	for (int i = 0; i < from_itunes && count + kept < limit; i++) {
		if (already_listed(hits, count, out[i].album) || already_listed(out, kept, out[i].album)) {
			continue;
		}
		hits[count + kept] = out[i];
		kept++;
	}
	count += kept;

	memcpy(out, hits, sizeof(hits[0]) * (size_t)count);
	fprintf(stderr, "artsearch: %d cover%s for \"%s\"\n", count, count == 1 ? "" : "s", query);
	if (count == 0) {
		set_error(error, error_size, "artwork_none");
		return 0;
	}
	return count;
}

bool artsearch_fetch(const char *url, void **data, size_t *size, char *error, size_t error_size) {
	if (data) {
		*data = NULL;
	}
	if (size) {
		*size = 0;
	}
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!url || !url[0] || !data || !size) {
		set_error(error, error_size, "artwork_failed");
		return false;
	}
	// An image URL is not JSON, and a catalogue that answers an error page
	// instead of a picture is caught by the magic-number check below.
	int status = 0;
	size_t len = 0;
	char *bytes = NULL;
	if (!http_get_ex(url, NULL, false, &bytes, &len, ARTSEARCH_IMAGE_LIMIT, ARTSEARCH_IMAGE_TIMEOUT,
					 &status) ||
		status < 200 || status >= 300 || !bytes || len == 0) {
		const char *why = http_last_error();
		fprintf(stderr, "artsearch: image %s failed (HTTP %d%s%s)\n", url, status, why ? ": " : "",
				why ? why : "");
		free(bytes);
		set_error(error, error_size, "artwork_failed");
		return false;
	}
	*data = bytes;
	*size = len;
	return true;
}

// A picture is a picture: JPEG or PNG by its own first bytes. Those two are
// what albumart.c will read back out of the folder, so those two are what is
// written -- a WebP saved as `cover.webp` would be artwork nothing looks at.
// Anything else -- an HTML error page, a truncated reply -- is refused rather
// than saved, because a file named .jpg that is not one leaves the track with
// no artwork at all.
static const char *image_extension(const uint8_t *data, size_t size) {
	if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
		return "jpg";
	}
	if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
		return "png";
	}
	return NULL;
}

// The folder's own artwork file, if it already has one. The names are the ones
// albumart.c looks for; a cover by any of them is a cover the user has.
static bool folder_has_cover(const char *dir) {
	static const char *const NAMES[] = {"cover.jpg", "cover.jpeg", "cover.png", "cover.webp",
										"folder.jpg", "folder.png", "album.jpg", "album.png",
										"front.jpg", "front.png"};
	char path[1200];
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, NAMES[i]);
		FILE *f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return true;
		}
	}
	return false;
}

bool artsearch_apply(const char *track_path, const char *url, bool replace, char *saved,
					 size_t saved_size, char *error, size_t error_size) {
	if (saved && saved_size) {
		saved[0] = '\0';
	}
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!track_path || !track_path[0] || !url || !url[0]) {
		set_error(error, error_size, "artwork_failed");
		return false;
	}

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", track_path);
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir) {
		set_error(error, error_size, "artwork_no_folder");
		return false;
	}
	*slash = '\0';
	if (!replace && folder_has_cover(dir)) {
		set_error(error, error_size, "artwork_exists");
		return false;
	}

	void *data = NULL;
	size_t size = 0;
	if (!artsearch_fetch(url, &data, &size, error, error_size)) {
		return false;
	}
	const char *ext = image_extension(data, size);
	if (!ext) {
		free(data);
		set_error(error, error_size, "artwork_bad_image");
		return false;
	}

	char path[1200];
	snprintf(path, sizeof(path), "%s/cover.%s", dir, ext);
	// Written through a temporary name and renamed: a card pulled out halfway
	// through a cover would otherwise leave a half-picture as the album art,
	// and the loader would keep showing it.
	char part[1240];
	snprintf(part, sizeof(part), "%s.part", path);
	FILE *f = fopen(part, "wb");
	if (!f) {
		free(data);
		set_error(error, error_size, "card_write_failed");
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	free(data);
	if (!ok || rename(part, path) != 0) {
		remove(part);
		set_error(error, error_size, "card_write_failed");
		return false;
	}
	// The other spellings of the same name rank identically in albumart.c's
	// lookup, and which of them wins is then a coin toss: a cover that is not
	// the one just written goes, so the picture chosen is the picture shown.
	static const char *const WHEN_PNG[] = {"cover.jpg", "cover.jpeg"};
	static const char *const WHEN_JPG[] = {"cover.png", "cover.jpeg"};
	const char *const *others = strcmp(ext, "png") == 0 ? WHEN_PNG : WHEN_JPG;
	for (int i = 0; i < 2; i++) {
		char other[1240];
		snprintf(other, sizeof(other), "%s/%s", dir, others[i]);
		remove(other);
	}
	if (saved && saved_size) {
		snprintf(saved, saved_size, "%s", path);
	}
	fprintf(stderr, "artsearch: wrote %s (%zu bytes)\n", path, size);
	return true;
}

bool artsearch_apply_tagged(const char *track_path, const char *url, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!track_path || !track_path[0] || !url || !url[0]) {
		set_error(error, error_size, "artwork_failed");
		return false;
	}
	void *data = NULL;
	size_t size = 0;
	if (!artsearch_fetch(url, &data, &size, error, error_size)) {
		return false;
	}
	// The writer decides for itself what a picture is: it refuses bytes that
	// are not a JPEG or a PNG before it touches the file, which is why nothing
	// is checked here as well.
	bool ok = tagwrite_set_cover(track_path, data, size, error, error_size);
	free(data);
	return ok;
}
