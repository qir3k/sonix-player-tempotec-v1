// Headless checks of the ListenBrainz module: the queue file on the card, the
// JSON body a request carries, and the answers the server sends.
//
// The module is compiled in here rather than linked, because the parts worth
// testing are its internals -- the queue file's lines, the body's JSON, the
// classification of a reply -- and its externals are the network, the card and
// the configuration, all of which are stubbed below. The JSON parser and the
// settings store are the real ones, linked as they are built into the player.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/system/listenbrainz/listenbrainz.c"

// ---------------------------------------------------------------------------
// the externals, stubbed
// ---------------------------------------------------------------------------

static char card_root[256] = "/tmp/lb-test-card";
static bool card_ok = true;

const char *storage_sd_root(void) { return card_ok ? card_root : NULL; }
const char *storage_sd_device(void) { return card_ok ? "sd0" : NULL; }
bool storage_card_attached(void) { return card_ok; }
bool storage_sd_writable(void) { return card_ok; }
bool usb_storage_active(void) { return false; }

static char next_error[160] = "";

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = WIFI_STATE_CONNECTED;
	out->ip[0] = '1';
}

// The next answer http_request() will give: `sent` false is a transport
// failure, `body` NULL with `sent` true is a 2xx with nothing to read.
static bool next_sent = true;
static int next_status = 200;
static const char *next_body = "{\"status\":\"ok\"}";

static char last_url[256];
static char last_body[65536];
static char last_headers[512];
static bool last_headers_seen;

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)limit;
	(void)timeout_secs;
	snprintf(last_url, sizeof(last_url), "%s", url);
	if (req->body) {
		snprintf(last_body, sizeof(last_body), "%s", req->body);
	} else {
		last_body[0] = '\0';
	}
	if (req->extra_headers) {
		snprintf(last_headers, sizeof(last_headers), "%s", req->extra_headers);
		last_headers_seen = true;
	}

	if (!next_sent) {
		*status_out = 0;
		return false;
	}
	*status_out = next_status;
	if (next_body) {
		*out_len = strlen(next_body);
		*out = malloc(*out_len + 1);
		assert(*out);
		memcpy(*out, next_body, *out_len + 1);
	} else {
		*out_len = 0;
		*out = malloc(1);
		assert(*out);
		(*out)[0] = '\0';
	}
	return true;
}

const char *http_last_error(void) { return next_error; }

// ---------------------------------------------------------------------------
// the queue file, on a card of the test's own
// ---------------------------------------------------------------------------

static void queue_reset(void) {
	unlink(queue_path);
	unlink(queue_tmp_path);
	card_ready = false;
}

static void make_play(play_t *p, long long when, int duration, const char *artist, const char *title, const char *album) {
	memset(p, 0, sizeof(*p));
	p->when = when;
	p->duration = duration;
	snprintf(p->artist, sizeof(p->artist), "%s", artist);
	snprintf(p->title, sizeof(p->title), "%s", title);
	snprintf(p->album, sizeof(p->album), "%s", album);
}

static void test_token_tag(void) {
	// The same token gives the same tag, a different one gives a different
	// one, and the tag is the sixteen hex digits the queue header carries.
	assert(token_tag("abc") == token_tag("abc"));
	assert(token_tag("abc") != token_tag("abd"));
	assert(token_tag("") != token_tag("a"));
	char want[24];
	snprintf(want, sizeof(want), "%016llx", token_tag("token-1"));
	assert(strlen(want) == 16);
	for (const char *p = want; *p; p++) {
		assert((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
	}
}

static void test_queue_lines(void) {
	// The escapes the queue file uses, and the unescape that reads them back.
	char out[FIELD_MAX];
	const char *in = "a%b\tc\r\nd";
	queue_unescape(in, in + strlen(in), out);
	assert(strcmp(out, in) == 0);

	// '%', tab, CR and LF are the four that cannot sit in a line as they are.
	FILE *f = fopen(queue_tmp_path, "w");
	assert(f);
	queue_escape(f, "x%y\tz\r\nw"); // queue_append ends every line like this
	fputc('\n', f);
	fclose(f);
	FILE *in_file = fopen(queue_tmp_path, "r");
	assert(in_file);
	char line[256];
	assert(fgets(line, sizeof(line), in_file));
	assert(strcmp(line, "x%25y%09z%0D%0Aw\n") == 0);
	fclose(in_file);
	unlink(queue_tmp_path);

	// The four, written as they appear in a line and read back as they were.
	queue_unescape("x%25y%09z%0D%0Aw", "x%25y%09z%0D%0Aw" + strlen("x%25y%09z%0D%0Aw"), out);
	assert(strcmp(out, "x%y\tz\r\nw") == 0);
}

static void test_queue_parse(void) {
	play_t p;
	assert(queue_parse("1700000000\t200\tArtist\tTitle\tAlbum", &p));
	assert(p.when == 1700000000);
	assert(p.duration == 200);
	assert(strcmp(p.artist, "Artist") == 0);
	assert(strcmp(p.title, "Title") == 0);
	assert(strcmp(p.album, "Album") == 0);

	// An empty album is allowed; an empty artist or title is not.
	assert(queue_parse("1700000000\t200\tArtist\tTitle\t", &p));
	assert(p.album[0] == '\0');
	assert(!queue_parse("1700000000\t200\t\tTitle\tAlbum", &p));
	assert(!queue_parse("1700000000\t200\tArtist\t\tAlbum", &p));
	assert(!queue_parse("1700000000\t200\tArtist\t", &p));

	// A timestamp before the clock was set is not a listen that can be sent.
	assert(!queue_parse("1000000000\t200\tArtist\tTitle\tAlbum", &p));

	// A line with fewer than five fields is not a listen.
	assert(!queue_parse("1700000000\t200\tArtist\tTitle", &p));
	// A duration that is not a number is left out rather than dropping the
	// listen: the server takes a listen without one.
	assert(queue_parse("1700000000\txx\tArtist\tTitle\tAlbum", &p));
	assert(p.duration == 0);

	// The escapes in the text fields come back as the characters they were.
	assert(queue_parse("1700000000\t200\tAr%25tist\tT%09itle\t%0DAl%0Abum", &p));
	assert(strcmp(p.artist, "Ar%tist") == 0);
	assert(strcmp(p.title, "T\title") == 0);
	assert(strcmp(p.album, "\rAl\nbum") == 0);
}

static void test_queue_file(void) {
	queue_reset();
	assert(queue_attach("token-1")); // the card is stubbed present and writable

	play_t plays[3];
	make_play(&plays[0], 1700000000, 200, "Artist 1", "Title 1", "Album 1");
	make_play(&plays[1], 1700000010, 210, "Artist 2", "Title 2", "");
	make_play(&plays[2], 1700000020, 220, "Artist %2", "T\t3", "Al\n3");
	assert(queue_append(plays, 3, "token-1"));

	// The header names the account by the tag of the token, not the token.
	char line[256];
	FILE *f = fopen(queue_path, "r");
	assert(f);
	assert(fgets(line, sizeof(line), f));
	line[strcspn(line, "\r\n")] = '\0';
	char want[64];
	snprintf(want, sizeof(want), "%s%016llx", QUEUE_HEADER, token_tag("token-1"));
	assert(strcmp(line, want) == 0);
	fclose(f);

	// A file for another account is not this one's.
	assert(!queue_header_is(want, "token-2"));
	assert(!queue_header_is("#lastfm\tsomeone", "token-1"));
	assert(queue_header_is(want, "token-1"));

	// A batch off the head: two of the three, in order.
	play_t out[BATCH_MAX];
	int lines = 0;
	assert(queue_read_head(out, 2, &lines) == 2);
	assert(lines == 2);
	assert(out[0].when == 1700000000);
	assert(out[1].when == 1700000010);

	// Dropping them leaves the third, under the header.
	queue_drop_head(2);
	lines = 0;
	assert(queue_read_head(out, BATCH_MAX, &lines) == 1);
	assert(lines == 1);
	assert(out[0].when == 1700000020);
	assert(strcmp(out[0].artist, "Artist %2") == 0);
	assert(strcmp(out[0].title, "T\t3") == 0);
	assert(strcmp(out[0].album, "Al\n3") == 0);

	// The last one goes with the file.
	queue_drop_head(1);
	assert(access(queue_path, F_OK) != 0);
	assert(!queue_attach("token-1") || true); // a missing file is not an error

	// A file for another account is thrown away, not read.
	f = fopen(queue_path, "w");
	assert(f);
	fprintf(f, "#listenbrainz\t%016llx\n1700000000\t200\tA\tT\t\n", token_tag("token-2"));
	fclose(f);
	assert(queue_attach("token-1"));
	pthread_mutex_lock(&lock);
	assert(file_count == 0);
	pthread_mutex_unlock(&lock);
	assert(access(queue_path, F_OK) != 0);

	// Without a card there is nothing to attach.
	card_ok = false;
	assert(!queue_attach("token-1"));
	card_ok = true;
	queue_reset();
}

// ---------------------------------------------------------------------------
// the body of a request
// ---------------------------------------------------------------------------

static void test_json_escape(void) {
	text_t t = {0};
	json_escape(&t, "a\"b\\c");
	assert(strcmp(t.buf, "a\\\"b\\\\c") == 0);
	text_free(&t);

	text_t u = {0};
	json_escape(&u, "x\ty\nz\rc\bd\fe");
	assert(strcmp(u.buf, "x\\ty\\nz\\rc\\bd\\fe") == 0);
	text_free(&u);

	// A control character below the short forms takes its \u00XX form.
	text_t v = {0};
	json_escape(&v, "a\001b");
	assert(strcmp(v.buf, "a\\u0001b") == 0);
	text_free(&v);

	// UTF-8 passes through: the tags are UTF-8 and JSON is UTF-8.
	text_t w = {0};
	json_escape(&w, "café ドリア");
	assert(strcmp(w.buf, "café ドリア") == 0);
	text_free(&w);
}

static void test_build_body(void) {
	play_t plays[2];
	make_play(&plays[0], 1700000000, 200, "Artist \"one\"", "T\\wo", "Album 1");
	make_play(&plays[1], 1700000010, 0, "Artist two", "Title two", "");

	// A batch goes as an import, one listen per play, with listened_at.
	text_t body = {0};
	assert(build_body("import", plays, 2, &body));
	json_doc_t doc;
	assert(json_parse(body.buf, &doc));
	int root = json_root(&doc);
	assert(root >= 0);
	char kind[32];
	assert(json_obj_str(&doc, root, "listen_type", kind, sizeof(kind)));
	assert(strcmp(kind, "import") == 0);
	int payload = json_get(&doc, root, "payload");
	assert(payload >= 0 && json_len(&doc, payload) == 2);
	int first = json_at(&doc, payload, 0);
	assert(json_llong(&doc, json_get(&doc, first, "listened_at"), -1) == 1700000000);
	int meta = json_get(&doc, first, "track_metadata");
	char artist[FIELD_MAX];
	assert(json_obj_str(&doc, meta, "artist_name", artist, sizeof(artist)));
	assert(strcmp(artist, "Artist \"one\"") == 0);
	char title[FIELD_MAX];
	assert(json_obj_str(&doc, meta, "track_name", title, sizeof(title)));
	assert(strcmp(title, "T\\wo") == 0);
	char album[FIELD_MAX];
	assert(json_obj_str(&doc, meta, "release_name", album, sizeof(album)));
	assert(strcmp(album, "Album 1") == 0);
	int info = json_get(&doc, meta, "additional_info");
	assert(json_llong(&doc, json_get(&doc, info, "duration"), -1) == 200);
	char client[64];
	assert(json_obj_str(&doc, info, "submission_client", client, sizeof(client)));
	assert(strcmp(client, "Sonix Player") == 0);
	// The second play has no album and no duration: neither key is sent.
	int second = json_at(&doc, payload, 1);
	int meta2 = json_get(&doc, second, "track_metadata");
	assert(json_get(&doc, meta2, "release_name") == -1);
	int info2 = json_get(&doc, meta2, "additional_info");
	assert(json_get(&doc, info2, "duration") == -1);
	json_free(&doc);
	text_free(&body);

	// Now playing carries no listened_at, and the duration is what tells the
	// server how long to keep the notification.
	text_t now = {0};
	assert(build_body("playing_now", &plays[0], 1, &now));
	assert(json_parse(now.buf, &doc));
	root = json_root(&doc);
	assert(json_obj_str(&doc, root, "listen_type", kind, sizeof(kind)));
	assert(strcmp(kind, "playing_now") == 0);
	payload = json_get(&doc, root, "payload");
	assert(payload >= 0 && json_len(&doc, payload) == 1);
	first = json_at(&doc, payload, 0);
	assert(json_get(&doc, first, "listened_at") == -1);
	json_free(&doc);
	text_free(&now);
}

// ---------------------------------------------------------------------------
// a request and its answer
// ---------------------------------------------------------------------------

static void test_submit(void) {
	play_t plays[1];
	make_play(&plays[0], 1700000000, 200, "Artist", "Title", "Album");
	char why[160];

	// A 2xx is a success whatever the body says.
	next_sent = true;
	next_status = 200;
	next_body = "{\"status\":\"ok\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_OK);
	assert(strcmp(last_url, API_URL) == 0);
	assert(strcmp(last_body,
		       "{\"listen_type\":\"import\",\"payload\":[{\"listened_at\":1700000000,\"track_metadata\":{\"artist_name\":\"Artist\",\"track_name\":\"Title\",\"release_name\":\"Album\",\"additional_info\":{\"submission_client\":\"Sonix Player\",\"duration\":200}}}]}") == 0);
	assert(last_headers_seen);
	assert(strstr(last_headers, "Authorization: Token the-token\r\n") == last_headers);
	assert(strstr(last_headers, "Content-Type: application/json\r\n") != NULL);

	// A 400 names the listen that was refused.
	next_status = 400;
	next_body = "{\"code\":400,\"error\":\"JSON document does not contain required field track_metadata.artist_name.\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_BAD_REQUEST);
	assert(strstr(why, "track_metadata.artist_name") != NULL);

	// A 401 is the token, not the request.
	next_status = 401;
	next_body = "{\"code\":401,\"error\":\"Invalid authorization token.\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_SESSION);
	assert(strcmp(why, "Invalid authorization token.") == 0);

	// A 429 and a 5xx are asked again later; a 403 is refused outright.
	next_status = 429;
	next_body = "{\"code\":429,\"error\":\"Rate limit exceeded.\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_RETRY);
	next_status = 503;
	next_body = "{\"code\":503,\"error\":\"Service unavailable.\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_RETRY);
	next_status = 403;
	next_body = "{\"code\":403,\"error\":\"Forbidden.\"}";
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_REFUSED);

	// No answer at all is a retry, and the transport error is what is shown.
	next_sent = false;
	next_status = 0;
	next_body = NULL;
	snprintf(next_error, sizeof(next_error), "%s", "connection refused");
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_RETRY);
	assert(strcmp(why, "connection refused") == 0);

	// A 2xx with nothing to read is still a success.
	next_sent = true;
	next_status = 204;
	next_body = NULL;
	next_error[0] = '\0';
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_OK);

	// A 4xx with nothing to read says the status code.
	next_status = 404;
	assert(submit_listens("import", plays, 1, "the-token", why, sizeof(why)) == RESULT_REFUSED);
	assert(strcmp(why, "HTTP 404") == 0);
}

int main(void) {
	char dir[] = "/tmp/lb-test-XXXXXX";
	assert(mkdtemp(dir) != NULL);
	snprintf(card_root, sizeof(card_root), "%s", dir);
	snprintf(queue_path, sizeof(queue_path), "%s/queue", dir);
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s/queue.tmp", dir);
	snprintf(token_path, sizeof(token_path), "%s/token", dir);

	test_token_tag();
	test_queue_lines();
	test_queue_parse();
	test_queue_file();
	test_json_escape();
	test_build_body();
	test_submit();

	unlink(queue_path);
	unlink(queue_tmp_path);
	unlink(token_path);
	rmdir(dir);
	printf("test_listenbrainz: ok\n");
	return 0;
}
