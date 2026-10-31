/*
 * Cover art search: what coverart.c makes of the Store's and MusicBrainz's
 * answers, how it ranks them, what it accepts as a picture, and the
 * cover.jpg it writes beside a track. The network is substituted: http_get_ex()
 * answers from the canned bodies written here.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/library/coverart.h"
#include "src/system/net/http.h"

/* ---- the network, substituted ------------------------------------------- */

static const char *itunes_body;		// NULL: the Store does not answer
static const char *musicbrainz_body; // NULL: MusicBrainz does not answer
static const char *image_body;		// the picture, NULL for a failed transfer
static size_t image_len;
static int requests;
static char last_url[4096];
/* What the next MusicBrainz request is sent as, and what it was sent as. */
static char agent[200];
static char agent_at_musicbrainz[200];
void http_set_user_agent_suffix(const char *suffix) { snprintf(agent, sizeof(agent), "%s", suffix ? suffix : ""); }

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	requests++;
	snprintf(last_url, sizeof(last_url), "%s", url);
	const char *body = NULL;
	size_t len = 0;
	if (strstr(url, "itunes.apple.com")) {
		body = itunes_body;
		len = body ? strlen(body) : 0;
	} else if (strstr(url, "musicbrainz.org")) {
		snprintf(agent_at_musicbrainz, sizeof(agent_at_musicbrainz), "%s", agent);
		body = musicbrainz_body;
		len = body ? strlen(body) : 0;
	} else {
		body = image_body;
		len = image_len;
	}
	if (!body) {
		*status_out = 404; // not transient: not asked twice
		return false;
	}
	*status_out = 200;
	*out = malloc(len + 1);
	memcpy(*out, body, len);
	(*out)[len] = '\0';
	if (out_len) {
		*out_len = len;
	}
	return true;
}

const char *http_last_error(void) { return NULL; }


char *http_url_encode(const char *in, char *out, size_t out_size) {
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && w + 4 < out_size; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
			*p == '_' || *p == '.' || *p == '~') {
			out[w++] = (char)*p;
		} else {
			w += (size_t)snprintf(out + w, out_size - w, "%%%02X", *p);
		}
	}
	out[w] = '\0';
	return out;
}

/* Three albums: a compilation of the artist's, then the album itself, then
 * the album by someone else. The Store's own order has to give way to the
 * tags. One has no picture and is left out. */
static const char ITUNES[] =
	"{\"resultCount\":4,\"results\":["
	"{\"collectionName\":\"Greatest Hits\",\"artistName\":\"The Band\",\"releaseDate\":\"2011-01-01T08:00:00Z\","
	"\"artworkUrl100\":\"https://is1.mzstatic.com/image/a/100x100bb.jpg\"},"
	"{\"collectionName\":\"Album\",\"artistName\":\"The Band\",\"releaseDate\":\"2005-05-05T07:00:00Z\","
	"\"artworkUrl100\":\"https://is1.mzstatic.com/image/b/100x100bb.jpg\"},"
	"{\"collectionName\":\"Album\",\"artistName\":\"Cover Band\",\"releaseDate\":\"2015-02-02T08:00:00Z\","
	"\"artworkUrl100\":\"https://is1.mzstatic.com/image/c/100x100bb.jpg\"},"
	"{\"collectionName\":\"No Picture\",\"artistName\":\"The Band\"}"
	"]}";

static const char MUSICBRAINZ[] =
	"{\"release-groups\":["
	"{\"id\":\"11111111-2222-3333-4444-555555555555\",\"title\":\"Rare Album\",\"first-release-date\":\"1977-06\","
	"\"artist-credit\":[{\"name\":\"Obscure Artist\"}]}"
	"]}";

static void check_search(void) {
	coverart_hit_t *hits;
	int count;

	/* No album in the tags: nothing is asked. */
	requests = 0;
	assert(coverart_search("", "The Band", &hits, &count) == COVERART_NO_TAGS);
	assert(requests == 0 && hits == NULL && count == 0);

	/* The Store's answer: ranked by the tags, sized up to 600 px. */
	itunes_body = ITUNES;
	musicbrainz_body = NULL;
	requests = 0;
	assert(coverart_search("Album", "The Band", &hits, &count) == COVERART_OK);
	assert(requests == 1);
	assert(count == 3);
	assert(strcmp(hits[0].album, "Album") == 0 && strcmp(hits[0].artist, "The Band") == 0);
	assert(strcmp(hits[0].year, "2005") == 0);
	assert(strcmp(hits[0].url, "https://is1.mzstatic.com/image/b/600x600bb.jpg") == 0);
	assert(strcmp(hits[1].artist, "Cover Band") == 0);
	assert(strcmp(hits[2].album, "Greatest Hits") == 0);
	free(hits);

	/* Nothing from the Store and no contact to give: MusicBrainz is not asked. */
	itunes_body = "{\"resultCount\":0,\"results\":[]}";
	musicbrainz_body = MUSICBRAINZ;
	coverart_set_contact("");
	assert(!coverart_has_contact());
	requests = 0;
	assert(coverart_search("Rare Album", "Obscure Artist", &hits, &count) == COVERART_NOT_FOUND);
	assert(requests == 1 && hits == NULL);

	/* The contact is trimmed and loses what would end the agent's comment. */
	coverart_set_contact("  me(at)example.org ");
	assert(strcmp(coverart_contact(), "meatexample.org") == 0);
	coverart_set_contact(" me@example.org ");
	assert(strcmp(coverart_contact(), "me@example.org") == 0);

	/* With one: MusicBrainz is asked, as the program that has that contact and
	 * for that request alone, and its hit points at the Cover Art Archive. */
	requests = 0;
	assert(coverart_search("Rare \"Album\"", "Obscure Artist", &hits, &count) == COVERART_OK);
	assert(requests == 2 && count == 1);
	assert(strcmp(agent_at_musicbrainz, " (me@example.org)") == 0);
	assert(agent[0] == '\0');
	assert(strstr(last_url, "query=releasegroup%3A%22Rare%20Album%22%20AND%20artist%3A%22Obscure%20Artist%22"));
	assert(strcmp(hits[0].album, "Rare Album") == 0 && strcmp(hits[0].year, "1977") == 0);
	assert(strcmp(hits[0].artist, "Obscure Artist") == 0);
	assert(strcmp(hits[0].url, "https://coverartarchive.org/release-group/11111111-2222-3333-4444-555555555555/front-500") == 0);
	free(hits);

	/* Both answer, neither knows the album. */
	itunes_body = "{\"resultCount\":0,\"results\":[]}";
	musicbrainz_body = "{\"release-groups\":[]}";
	assert(coverart_search("Nothing", "Nobody", &hits, &count) == COVERART_NOT_FOUND);
	assert(hits == NULL && count == 0);

	/* Neither answers. */
	itunes_body = NULL;
	musicbrainz_body = NULL;
	assert(coverart_search("Album", "The Band", &hits, &count) == COVERART_NETWORK);
}

static void check_fetch(void) {
	uint8_t *data;
	size_t len;
	static const char JPEG[] = "\xFF\xD8\xFF\xE0 jpeg bytes";
	static const char PNG[] = "\x89PNG\r\n\x1a\n png bytes";

	image_body = JPEG;
	image_len = sizeof(JPEG) - 1;
	assert(coverart_fetch("https://example.test/a.jpg", &data, &len));
	assert(len == image_len && memcmp(data, JPEG, len) == 0);
	free(data);

	image_body = PNG;
	image_len = sizeof(PNG) - 1;
	assert(coverart_fetch("https://example.test/a.png", &data, &len));
	free(data);

	/* An error page that came back as a 200 is not a picture. */
	image_body = "<html>nope</html>";
	image_len = strlen(image_body);
	assert(!coverart_fetch("https://example.test/a", &data, &len));
	assert(data == NULL && len == 0);

	image_body = NULL;
	assert(!coverart_fetch("https://example.test/a", &data, &len));
}

static void check_save(void) {
	char dir[] = "/tmp/coverart-test-XXXXXX";
	assert(mkdtemp(dir));
	char track[256], cover[256], part[256];
	snprintf(track, sizeof(track), "%s/01 - Song.flac", dir);
	snprintf(cover, sizeof(cover), "%s/cover.jpg", dir);
	snprintf(part, sizeof(part), "%s/cover.jpg.part", dir);

	static const uint8_t JPEG[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6};
	assert(coverart_save(track, JPEG, sizeof(JPEG)));
	struct stat st;
	assert(stat(cover, &st) == 0 && (size_t)st.st_size == sizeof(JPEG));
	assert(stat(part, &st) != 0); /* no half file left behind */

	/* A second save replaces the first. */
	static const uint8_t JPEG2[] = {0xFF, 0xD8, 0xFF, 0xE1, 9, 9, 9, 9, 9, 9, 9, 9};
	assert(coverart_save(track, JPEG2, sizeof(JPEG2)));
	assert(stat(cover, &st) == 0 && (size_t)st.st_size == sizeof(JPEG2));

	/* A folder that is not there. */
	assert(!coverart_save("/nonexistent/dir/song.flac", JPEG, sizeof(JPEG)));

	unlink(cover);
	rmdir(dir);
}

/* ---- the strings, in every language -------------------------------------- */

/* "key = text" with some text, as a line of a language file. */
static bool has_string(const char *path, const char *key) {
	FILE *f = fopen(path, "r");
	assert(f);
	size_t klen = strlen(key);
	char line[1024];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		found = strncmp(line, key, klen) == 0 && strncmp(line + klen, " = ", 3) == 0 && (unsigned char)line[klen + 3] > ' ';
	}
	fclose(f);
	return found;
}

static void check_strings(const char *language_dir) {
	static const char *const KEYS[] = {
		"trackmenu_find_cover", "coverart_results", "coverart_apply", "coverart_searching", "coverart_loading",
		"coverart_not_found", "coverart_no_tags", "coverart_search_failed", "coverart_download_failed",
		"coverart_not_saved", "coverart_saved", "coverart_no_wifi", "coverart_has_embedded",
		"coverart_find_title", "coverart_wifi_off_note", "coverart_enable_wifi", "coverart_wifi_connecting",
		"coverart_wifi_failed", "coverart_contact", "coverart_contact_title", "coverart_contact_hint", "coverart_contact_note",
		"coverart_contact_saved",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		size_t n = strlen(entry->d_name);
		if (n <= 4 || strcmp(entry->d_name + n - 4, ".ini") != 0) {
			continue;
		}
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", language_dir, entry->d_name);
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			if (!has_string(path, KEYS[i])) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", entry->d_name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
		}
		languages++;
	}
	closedir(dir);
	assert(languages == 10);
}

int main(int argc, char **argv) {
	check_search();
	check_fetch();
	check_save();
	if (argc > 1) {
		check_strings(argv[1]);
	}
	printf("coverart: ok\n");
	return 0;
}
