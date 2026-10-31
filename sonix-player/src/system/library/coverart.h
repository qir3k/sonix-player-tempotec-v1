#ifndef COVERART_H
#define COVERART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cover art for an album that has none: a search by album and artist, and the
// picture picked saved as cover.jpg in the album's folder, which is where the
// cover loader looks when a track carries no artwork of its own.
//
// The iTunes Store search comes first -- one request, square JPEGs of a size
// this player can decode -- and MusicBrainz with the Cover Art Archive behind
// it, for what the Store does not sell. Neither needs a key.
//
// All of it blocks on the network or the card: call it on a thread of its own,
// never on the interface thread.

typedef enum {
	COVERART_OK,		// at least one album
	COVERART_NOT_FOUND, // nothing for these tags
	COVERART_NO_TAGS,	// no album to search with
	COVERART_NETWORK,	// no answer, or not one that could be read
} coverart_result_t;

typedef struct {
	char album[160];
	char artist[160];
	char year[8]; // "" when the record does not say
	char url[320]; // the picture, to hand to coverart_fetch()
} coverart_hit_t;

// Who to tell MusicBrainz is asking, an e-mail address or a web page: it asks
// every program that uses it to say, and may refuse those that do not. Without
// one the search stays with the Store. Leading and trailing blanks are dropped.
void coverart_set_contact(const char *contact);
bool coverart_has_contact(void);
const char *coverart_contact(void); // as kept: trimmed, without parentheses

// Searches by album and artist (the artist may be empty). On COVERART_OK *hits
// holds *count albums, the likeliest first: the same album title, then the same
// artist. Free them with free().
coverart_result_t coverart_search(const char *album, const char *artist, coverart_hit_t **hits, int *count);

// Downloads the picture at `url`. On true *data holds *len bytes of a JPEG or
// a PNG, to free(); false for a failed transfer, a truncated one, or an answer
// that is not a picture.
bool coverart_fetch(const char *url, uint8_t **data, size_t *len);

// Writes the picture as "cover.jpg" (or "cover.png") in the folder of the file
// at `track_path`, replacing one already there. False when the card would not
// take it.
bool coverart_save(const char *track_path, const uint8_t *data, size_t len);

#endif /* COVERART_H */
