#ifndef ARTSEARCH_H
#define ARTSEARCH_H

#include <stdbool.h>
#include <stddef.h>

// Album artwork for a track that has none, or different artwork for one that
// has: the answers two free catalogues give, and the one call that puts a
// chosen picture beside the file.
//
// The same idea as the lyrics search next to the player, and the same reason
// for it: a card full of files carries whatever the people who tagged them
// bothered to put in, which on a copy of an album is often nothing at all. Two
// sources are asked, neither with a key and neither with an account -- Deezer's
// public catalogue, which serves large covers, and Apple's iTunes Search API,
// which serves them smaller. A hit carries both sizes: the small one for the
// row that offers it, the large one for the file that results.
//
// Everything here blocks on the network and must be called from a worker
// thread, never from the interface or the audio thread.

#define ARTSEARCH_MAX 10
#define ARTSEARCH_ALBUM_MAX 160
#define ARTSEARCH_ARTIST_MAX 128
#define ARTSEARCH_SOURCE_MAX 16
#define ARTSEARCH_URL_MAX 512

typedef struct {
	char album[ARTSEARCH_ALBUM_MAX];
	char artist[ARTSEARCH_ARTIST_MAX];
	// Which catalogue answered: "Deezer" or "iTunes". Shown on the row, so a
	// duplicate in both is visibly the same record twice.
	char source[ARTSEARCH_SOURCE_MAX];
	int year; // 0 when the answer did not say
	char thumb_url[ARTSEARCH_URL_MAX]; // a small copy, for the row
	char full_url[ARTSEARCH_URL_MAX];  // the largest offered, for the file
} artsearch_hit_t;

// Looks a track's record up by artist and album, falling back to artist and
// title when the album tag is empty. Fills up to `limit` hits and returns how
// many, 0 when the catalogues had nothing, or -1 with a tag in `error` -- the
// caller turns that into a message. An entry both catalogues know is listed
// once.
int artsearch_find(const char *artist, const char *album, const char *title, artsearch_hit_t *out,
				   int limit, char *error, size_t error_size);

// Fetches one picture into a freshly allocated buffer: the row's own copy, or
// the full-size one before it is written. The caller frees `*data`.
bool artsearch_fetch(const char *url, void **data, size_t *size, char *error, size_t error_size);

// The picture at `url`, written beside `track_path` as the folder's artwork --
// `cover.jpg` (or `cover.png` when that is what came back). This is what every
// track of the folder will then show, which is the point: a record has one
// sleeve. `replace` false refuses when the folder already has a cover file, so
// a track that does have artwork is never quietly overwritten.
//
// `saved` receives the path written, for the caller's message. False with a tag
// in `error` when nothing was written: "artwork_exists", "artwork_bad_image" or
// the network's own reason.
bool artsearch_apply(const char *track_path, const char *url, bool replace, char *saved,
					 size_t saved_size, char *error, size_t error_size);

// The picture at `url`, written INTO the file's own tags instead of beside it:
// the APIC frame of an MP3 or the PICTURE block of a FLAC, replacing whatever
// was there. This is the only way to change the sleeve of a file that carries
// one, because a picture in the tags is the one a reader draws and a cover file
// next to the song would never be seen.
//
// `error` carries the same tags as tagwrite_set_cover(): "artwork_tag_failed",
// "artwork_tag_unsupported", or the network's own reason.
bool artsearch_apply_tagged(const char *track_path, const char *url, char *error, size_t error_size);

#endif // ARTSEARCH_H
