#define _GNU_SOURCE 1

#include "listenbrainz.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define API_URL "https://api.listenbrainz.org/1/submit-listens"
#define API_TIMEOUT_SECS 15
#define BODY_LIMIT (64 * 1024)

// submit-listens takes up to a thousand listens per request, and ListenBrainz
// asks clients to keep submissions small; the queue drains in batches of fifty
// so that one refusal costs a handful of plays rather than a thousand.
#define BATCH_MAX 50
#define QUEUE_MAX 5000
#define PENDING_MAX 256
#define FIELD_MAX 256

// 2020-01-01 UTC. A clock before it has not been set since the last power-off.
#define CLOCK_VALID_AFTER 1577836800LL

#define RETRY_FIRST_SECS 30.0
#define RETRY_MAX_SECS 600.0
// How often the worker looks at the Wi-Fi and the clock while something is
// waiting on them.
#define WAIT_POLL_SECS 10

// ListenBrainz's "invalid token" this many times in a row, with the usual
// retry pauses between, before the token counts as lost. One refusal can come
// from ListenBrainz having a bad moment rather than from the token.
#define TOKEN_REFUSALS_MAX 3

// ListenBrainz's rule: a track of at least 30 seconds, listened to for half
// its length or for four minutes.
#define SCROBBLE_MIN_LENGTH 30.0
#define SCROBBLE_ENOUGH 240.0

typedef struct {
	long long when;	   // UTC seconds of the start of the play, 0 while not known
	double boot_start; // CLOCK_BOOTTIME of the start, to work `when` out later
	int duration;
	char artist[FIELD_MAX];
	char title[FIELD_MAX];
	char album[FIELD_MAX];
} play_t;

// ---------------------------------------------------------------------------
// state shared with the worker, under `lock`
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool started;

static bool enabled;
static char token[64];
static bool token_expired;
static int token_refusals;
static bool refused;
static char detail[160];

static bool signin_requested;
static bool signout_requested;
static bool signing_in;
static char signin_token[64];

static play_t pending[PENDING_MAX];
static int pending_count;
static int file_count; // listens in the queue file; 0 while there is no card

static bool now_playing_waiting;
static play_t now_playing;
static unsigned now_playing_generation;
static unsigned current_generation;
static bool current_playing;

static int requests_in_flight;

static char token_path[512];

// Worker thread only. `card_ready` is true once the queue on the card now in
// the slot has been checked and counted.
static bool card_ready;
static char queue_path[600];
static char queue_tmp_path[610];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static double boot_seconds(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool clock_valid(void) { return (long long)time(NULL) >= CLOCK_VALID_AFTER; }

static void wipe(void *p, size_t n) {
	volatile unsigned char *b = p;
	while (n--) {
		*b++ = 0;
	}
}

static void copy_field(char *dst, const char *src) { snprintf(dst, FIELD_MAX, "%s", src ? src : ""); }

// Under `lock`. Listens are counted for an account that is signed in.
static bool counting_locked(void) { return enabled && token[0]; }

static bool online(void) {
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0];
}

// The token lives in the folder that holds device_config.ini: /usr/data on the
// device, there with or without a card.
static void build_paths(void) {
	char dir[400] = "/tmp";
	const char *cfg = config_path();
	if (cfg && cfg[0]) {
		const char *slash = strrchr(cfg, '/');
		if (slash == cfg) {
			snprintf(dir, sizeof(dir), "/");
		} else if (slash) {
			snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cfg), cfg);
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}
	snprintf(token_path, sizeof(token_path), "%s/listenbrainz.token", dir);
}

// ---------------------------------------------------------------------------
// the token file: the token on the first line
// ---------------------------------------------------------------------------

static void load_token(void) {
	FILE *f = fopen(token_path, "r");
	if (!f) {
		return;
	}
	char line[256];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(token, sizeof(token), "%.*s", (int)sizeof(token) - 1, line);
	}
	fclose(f);
	wipe(line, sizeof(line));
}

static void save_token(const char *tok) {
	if (!tok[0]) {
		unlink(token_path);
		return;
	}
	char tmp[530];
	snprintf(tmp, sizeof(tmp), "%s.tmp", token_path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "listenbrainz: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}
	chmod(tmp, 0600);
	fprintf(f, "%s\n", tok);
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, token_path) != 0) {
		fprintf(stderr, "listenbrainz: cannot write %s\n", token_path);
		unlink(tmp);
	}
}

// ---------------------------------------------------------------------------
// the queue file
//
// .local/listenbrainz.queue on the card. The first line names the account the
// listens belong to, `#listenbrainz <tag>`; a file for another account, or
// without that line, is thrown away. Then one listen per line:
//
//     when \t duration \t artist \t title \t album
//
// In the text fields '%', tab, CR and LF are written as %25, %09, %0D and %0A;
// the album may be empty. Only the worker touches the file.
// ---------------------------------------------------------------------------

#define QUEUE_HEADER "#listenbrainz\t"

// The tag is a hash of the token rather than the token itself: the queue on
// the card is not secret, but a card read by another player should not carry
// a copy of the token with it. Two accounts' queues stay apart either way.
static unsigned long long token_tag(const char *tok) {
	unsigned long long h = 1469598103934665603ULL; // FNV-1a, 64 bits
	for (const unsigned char *p = (const unsigned char *)tok; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

static bool card_writable(void) { return storage_sd_root() && storage_sd_device() && storage_card_attached() && storage_sd_writable() && !usb_storage_active(); }

static void queue_escape(FILE *f, const char *s) {
	for (; *s; s++) {
		if (*s == '%' || *s == '\t' || *s == '\r' || *s == '\n') {
			fprintf(f, "%%%02X", (unsigned char)*s);
		} else {
			fputc(*s, f);
		}
	}
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

// Unescapes [from, to) into a FIELD_MAX buffer, cutting what does not fit.
static void queue_unescape(const char *from, const char *to, char *out) {
	size_t n = 0;
	while (from < to && n + 1 < FIELD_MAX) {
		int hi, lo;
		if (*from == '%' && to - from >= 3 && (hi = hex_digit(from[1])) >= 0 && (lo = hex_digit(from[2])) >= 0) {
			out[n++] = (char)(hi * 16 + lo);
			from += 3;
		} else {
			out[n++] = *from++;
		}
	}
	out[n] = '\0';
}

static bool queue_parse(const char *line, play_t *out) {
	const char *field[5];
	const char *end[5];
	const char *p = line;
	for (int i = 0; i < 5; i++) {
		field[i] = p;
		const char *stop = i < 4 ? strchr(p, '\t') : p + strcspn(p, "\r\n");
		if (!stop) {
			return false;
		}
		end[i] = stop;
		p = stop + 1;
	}

	memset(out, 0, sizeof(*out));
	char *num_end = NULL;
	out->when = strtoll(field[0], &num_end, 10);
	if (num_end != end[0] || out->when < CLOCK_VALID_AFTER) {
		return false;
	}
	out->duration = (int)strtol(field[1], &num_end, 10);
	if (num_end != end[1] || out->duration < 0) {
		out->duration = 0;
	}
	queue_unescape(field[2], end[2], out->artist);
	queue_unescape(field[3], end[3], out->title);
	queue_unescape(field[4], end[4], out->album);
	return out->artist[0] && out->title[0];
}

// Whether `line` is the header for `tok`.
static bool queue_header_is(const char *line, const char *tok) {
	size_t len = strlen(QUEUE_HEADER);
	if (strncmp(line, QUEUE_HEADER, len) != 0) {
		return false;
	}
	const char *tag = line + len;
	size_t tag_len = strcspn(tag, "\r\n");
	char want[24];
	snprintf(want, sizeof(want), "%016llx", token_tag(tok));
	return tag_len == strlen(want) && strncasecmp(tag, want, tag_len) == 0;
}

// Checks the queue on the card in the slot against the token signed in and
// counts its listens. False when there is no card to write to.
static bool queue_attach(const char *tok) {
	if (!card_writable()) {
		return false;
	}
	char dir[520];
	snprintf(dir, sizeof(dir), "%s/.local", storage_sd_root());
	mkdir(dir, 0755);
	snprintf(queue_path, sizeof(queue_path), "%s/listenbrainz.queue", dir);
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s.tmp", queue_path);

	int plays = 0;
	FILE *f = fopen(queue_path, "r");
	if (f) {
		char *line = NULL;
		size_t cap = 0;
		bool mine = getline(&line, &cap, f) >= 0 && queue_header_is(line, tok);
		while (mine && getline(&line, &cap, f) >= 0) {
			plays++;
		}
		free(line);
		fclose(f);
		if (!mine) {
			printf("listenbrainz: %s is not this account's; discarding it\n", queue_path);
			unlink(queue_path);
			plays = 0;
		}
	}
	pthread_mutex_lock(&lock);
	file_count = plays;
	pthread_mutex_unlock(&lock);
	return true;
}

static bool queue_append(const play_t *plays, int count, const char *tok) {
	FILE *f = fopen(queue_path, "a");
	if (!f) {
		fprintf(stderr, "listenbrainz: cannot open %s: %s\n", queue_path, strerror(errno));
		return false;
	}
	if (ftell(f) == 0) {
		fprintf(f, QUEUE_HEADER "%016llx\n", token_tag(tok));
	}
	for (int i = 0; i < count; i++) {
		fprintf(f, "%lld\t%d\t", plays[i].when, plays[i].duration);
		queue_escape(f, plays[i].artist);
		fputc('\t', f);
		queue_escape(f, plays[i].title);
		fputc('\t', f);
		queue_escape(f, plays[i].album);
		fputc('\n', f);
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	return ok;
}

// Up to `max` plays from the head of the queue. `*lines_used` counts every play
// line read, unreadable ones included, so that they go with the batch.
static int queue_read_head(play_t *out, int max, int *lines_used) {
	*lines_used = 0;
	FILE *f = fopen(queue_path, "r");
	if (!f) {
		return 0;
	}
	char *line = NULL;
	size_t cap = 0;
	int count = 0;
	if (getline(&line, &cap, f) >= 0) { // the header
		while (count < max && getline(&line, &cap, f) >= 0) {
			(*lines_used)++;
			if (queue_parse(line, &out[count])) {
				count++;
			}
		}
	}
	free(line);
	fclose(f);
	return count;
}

// Removes the first `lines` plays, keeping the header; the file goes when no
// play is left.
static void queue_drop_head(int lines) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return;
	}
	char *line = NULL;
	size_t cap = 0;
	int n = -1; // the header is line -1
	bool any = false;
	while (getline(&line, &cap, in) >= 0) {
		if (n < 0) {
			fputs(line, out);
		} else if (n >= lines) {
			fputs(line, out);
			any = true;
		}
		n++;
	}
	free(line);
	fclose(in);
	bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
	ok = fclose(out) == 0 && ok;
	if (!ok || !any) {
		unlink(queue_tmp_path);
		if (ok) {
			unlink(queue_path);
		}
		return;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		unlink(queue_tmp_path);
	}
}

// ---------------------------------------------------------------------------
// the body of a request
//
// A growable text buffer, copied and wiped rather than realloc'd as it grows:
// it holds the tags and the timestamps, and the rules about secrets apply to
// the whole request while it is being built.
// ---------------------------------------------------------------------------

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
	bool failed;
} text_t;

static void text_add(text_t *t, const char *s, size_t n) {
	if (t->failed) {
		return;
	}
	if (t->len + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 1024;
		while (cap < t->len + n + 1) {
			cap *= 2;
		}
		char *grown = malloc(cap);
		if (!grown) {
			t->failed = true;
			return;
		}
		if (t->buf) {
			memcpy(grown, t->buf, t->len);
			wipe(t->buf, t->cap);
			free(t->buf);
		}
		t->buf = grown;
		t->cap = cap;
	}
	memcpy(t->buf + t->len, s, n);
	t->len += n;
	t->buf[t->len] = '\0';
}

static void text_str(text_t *t, const char *s) { text_add(t, s, strlen(s)); }

// Escapes `s` for a JSON string: the quote and the backslash become \" and \\,
// the control characters JSON spells specially take their short forms, and the
// rest of those below 0x20 take \u00XX. Bytes above 0x7f pass through: the
// tags are UTF-8 and so is JSON.
static void json_escape(text_t *t, const char *s) {
	static const char hex[] = "0123456789abcdef";
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			text_add(t, "\\\"", 2);
			break;
		case '\\':
			text_add(t, "\\\\", 2);
			break;
		case '\b':
			text_add(t, "\\b", 2);
			break;
		case '\f':
			text_add(t, "\\f", 2);
			break;
		case '\n':
			text_add(t, "\\n", 2);
			break;
		case '\r':
			text_add(t, "\\r", 2);
			break;
		case '\t':
			text_add(t, "\\t", 2);
			break;
		default:
			if (*p < 0x20) {
				char esc[6] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15]};
				text_add(t, esc, 6);
			} else {
				text_add(t, (const char *)p, 1);
			}
		}
	}
}

static void text_free(text_t *t) {
	if (t->buf) {
		wipe(t->buf, t->cap);
	}
	free(t->buf);
	memset(t, 0, sizeof(*t));
}

// The body of one submit-listens request:
//
//     {"listen_type":"<import|playing_now>","payload":[
//         {"listened_at":<unix seconds>,"track_metadata":{
//             "artist_name":"...","track_name":"...","release_name":"...",
//             "additional_info":{"submission_client":"Sonix Player",
//                               "duration":<seconds>}}},
//         ...]}
//
// playing_now leaves out listened_at. ListenBrainz takes exactly one listen
// for single and for playing_now, so a batch always goes as an import, and a
// listen over thirty seconds of tags is the only kind the queue ever holds.
// release_name and duration are left out when there is nothing to send.
static bool build_body(const char *listen_type, const play_t *plays, int count, text_t *out) {
	text_str(out, "{\"listen_type\":\"");
	text_str(out, listen_type);
	text_str(out, "\",\"payload\":[");
	for (int i = 0; i < count; i++) {
		if (i) {
			text_str(out, ",");
		}
		if (strcmp(listen_type, "playing_now") != 0) {
			char number[32];
			snprintf(number, sizeof(number), "{\"listened_at\":%lld,", (long long)plays[i].when);
			text_str(out, number);
		} else {
			text_str(out, "{");
		}
		text_str(out, "\"track_metadata\":{\"artist_name\":\"");
		json_escape(out, plays[i].artist);
		text_str(out, "\",\"track_name\":\"");
		json_escape(out, plays[i].title);
		if (plays[i].album[0]) {
			text_str(out, "\",\"release_name\":\"");
			json_escape(out, plays[i].album);
		}
		text_str(out, "\",\"additional_info\":{\"submission_client\":\"Sonix Player\"");
		if (plays[i].duration > 0) {
			char number[32];
			snprintf(number, sizeof(number), ",\"duration\":%d", plays[i].duration);
			text_str(out, number);
		}
		text_str(out, "}}}");
	}
	text_str(out, "]}");
	return !out->failed;
}

typedef enum {
	RESULT_OK,
	RESULT_RETRY,		// no answer, or ListenBrainz busy: try again later
	RESULT_SESSION,	// the token is no longer valid
	RESULT_BAD_REQUEST, // the parameters were refused
	RESULT_REFUSED,	// anything else ListenBrainz said no to
} result_t;

// Submits one request and sorts the answer. A 2xx is a success whatever the
// body says; a refusal carries {"code":<status>,"error":"<message>"}, where
// the message is what ListenBrainz says went wrong and the status code is
// what decides what to do about it. `why` gets one or the other, in English.
static result_t submit_listens(const char *listen_type, const play_t *plays, int count, const char *tk,
				   char *why, size_t why_size) {
	why[0] = '\0';
	text_t body = {0};
	if (!build_body(listen_type, plays, count, &body)) {
		text_free(&body);
		snprintf(why, why_size, "out of memory");
		return RESULT_RETRY;
	}

	char header[256];
	snprintf(header, sizeof(header), "Authorization: Token %s\r\nContent-Type: application/json\r\n", tk);

	http_req_t req = {
		.method = "POST",
		.extra_headers = header,
		.body = body.buf,
		.body_len = body.len,
		.content_type = "application/json",
		.want_error_body = true,
		.allow_empty_body = true,
		.no_https_downgrade = true,
	};

	pthread_mutex_lock(&lock);
	requests_in_flight++;
	pthread_mutex_unlock(&lock);

	char *response = NULL;
	size_t response_len = 0;
	int status = 0;
	http_request(API_URL, &req, &response, &response_len, BODY_LIMIT, API_TIMEOUT_SECS, &status);

	pthread_mutex_lock(&lock);
	requests_in_flight--;
	pthread_mutex_unlock(&lock);
	text_free(&body);

	result_t result;
	if (status >= 200 && status < 300) {
		result = RESULT_OK;
	} else {
		char error[200] = "";
		if (response && response_len) {
			json_doc_t doc;
			if (json_parse(response, &doc)) {
				json_obj_str(&doc, json_root(&doc), "error", error, sizeof(error));
				json_free(&doc);
			}
		}
		if (!error[0]) {
			const char *err = http_last_error();
			if (err && err[0]) {
				snprintf(why, why_size, "%s", err);
			} else if (status) {
				snprintf(why, why_size, "HTTP %d", status);
			} else {
				snprintf(why, why_size, "no reply");
			}
		} else {
			snprintf(why, why_size, "%s", error);
		}
		switch (status) {
		case 400: // the request itself: a listen the server will not take
			result = RESULT_BAD_REQUEST;
			break;
		case 401: // the token is not valid
			result = RESULT_SESSION;
			break;
		case 429: // too many requests
		case 500:
		case 502:
		case 503:
		case 504:
			result = RESULT_RETRY;
			break;
		default:
			result = status >= 400 && status < 500 ? RESULT_REFUSED : RESULT_RETRY;
			break;
		}
	}
	free(response);
	return result;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static void do_signin(const char *tok) {
	save_token(tok);
	card_ready = false; // checked again against the token now signed in

	pthread_mutex_lock(&lock);
	snprintf(token, sizeof(token), "%s", tok);
	token_expired = false;
	token_refusals = 0;
	refused = false;
	detail[0] = '\0';
	pthread_mutex_unlock(&lock);
	printf("listenbrainz: signed in\n");
}

static void do_signout(void) {
	save_token("");
	if (card_ready) {
		unlink(queue_path);
	}
	card_ready = false;
	pthread_mutex_lock(&lock);
	token[0] = '\0';
	token_expired = false;
	refused = false;
	detail[0] = '\0';
	file_count = 0;
	pending_count = 0;
	now_playing_waiting = false;
	pthread_mutex_unlock(&lock);
	printf("listenbrainz: signed out\n");
}

static void token_lost(const char *why) {
	fprintf(stderr, "listenbrainz: the token was refused (%s); sign in again\n", why);
	save_token("");
	pthread_mutex_lock(&lock);
	token[0] = '\0';
	token_expired = true;
	token_refusals = 0;
	pthread_mutex_unlock(&lock);
}

// A 401 answer. True when the token is now lost; until then the token stays
// and the caller tries again later.
static bool token_refused(const char *why) {
	pthread_mutex_lock(&lock);
	int count = ++token_refusals;
	pthread_mutex_unlock(&lock);
	if (count >= TOKEN_REFUSALS_MAX) {
		token_lost(why);
		return true;
	}
	fprintf(stderr, "listenbrainz: the token was refused (%s), %d of %d; keeping it for now\n", why, count, TOKEN_REFUSALS_MAX);
	return false;
}

static void token_accepted(void) {
	pthread_mutex_lock(&lock);
	token_refusals = 0;
	pthread_mutex_unlock(&lock);
}

// Puts the listens whose time is known into the file on the card. A listen
// that started before the clock was set gets its time once the clock is right,
// counted back on CLOCK_BOOTTIME, which keeps running through suspend. Listens
// stay here while there is no card to write them to.
static void flush_pending(const char *tok) {
	bool valid = clock_valid();
	long long now_real = (long long)time(NULL);
	double now_boot = boot_seconds();

	pthread_mutex_lock(&lock);
	for (int i = 0; i < pending_count; i++) {
		if (!pending[i].when && valid) {
			pending[i].when = now_real - (long long)(now_boot - pending[i].boot_start);
		}
	}
	pthread_mutex_unlock(&lock);
	if (!card_ready) {
		return;
	}

	play_t *ready = malloc(sizeof(play_t) * PENDING_MAX);
	if (!ready) {
		return;
	}
	pthread_mutex_lock(&lock);
	int ready_count = 0;
	for (int i = 0; i < pending_count; i++) {
		if (pending[i].when) {
			ready[ready_count++] = pending[i];
		}
	}
	int room = QUEUE_MAX - file_count;
	pthread_mutex_unlock(&lock);
	if (!ready_count) {
		free(ready);
		return;
	}

	if (room < 0) {
		room = 0;
	}
	int kept = ready_count > room ? room : ready_count;
	if (kept < ready_count) {
		fprintf(stderr, "listenbrainz: the queue is full; %d listens not kept\n", ready_count - kept);
	}
	bool written = kept == 0 || queue_append(ready, kept, tok);
	free(ready);
	if (!written) {
		return; // still in `pending`, for the next pass
	}

	// Out of `pending` go the listens just handled; the GUI thread may have added
	// more behind them in the meantime.
	pthread_mutex_lock(&lock);
	file_count += kept;
	int keep = 0;
	for (int i = 0; i < pending_count; i++) {
		if (!pending[i].when) {
			pending[keep++] = pending[i];
		}
	}
	pending_count = keep;
	pthread_mutex_unlock(&lock);
}

static void send_now_playing(const play_t *track, const char *tk) {
	char why[160];
	result_t result = submit_listens("playing_now", track, 1, tk, why, sizeof(why));
	// A refused token is left to the listens to count: they are retried,
	// "now playing" is not.
	if (result == RESULT_OK) {
		token_accepted();
	} else {
		fprintf(stderr, "listenbrainz: now playing not sent: %s\n", why);
	}
}

// Sends up to `max` listens from the head of the queue. `*lines` is how many
// lines of the file the batch took up.
static result_t send_batch(int max, const char *tk, char *why, size_t why_size, int *lines) {
	*lines = 0;
	why[0] = '\0';
	play_t *plays = malloc(sizeof(play_t) * (size_t)max);
	if (!plays) {
		snprintf(why, why_size, "out of memory");
		return RESULT_RETRY;
	}

	int count = queue_read_head(plays, max, lines);
	result_t result = RESULT_OK;
	if (count > 0) {
		result = submit_listens("import", plays, count, tk, why, why_size);
		if (result == RESULT_OK) {
			printf("listenbrainz: %d listens sent\n", count);
		}
	}

	free(plays);
	return result;
}

typedef struct {
	int batch;
	double backoff;
	double retry_at; // boot seconds
	bool was_online;
} sender_t;

// One batch off the queue, and what to do after it.
static void send_queue(sender_t *s, const char *tk) {
	char why[160];
	int lines = 0;
	result_t result = send_batch(s->batch, tk, why, sizeof(why), &lines);
	bool drop = false;

	switch (result) {
	case RESULT_OK:
		drop = true;
		token_accepted();
		s->backoff = RETRY_FIRST_SECS;
		pthread_mutex_lock(&lock);
		refused = false;
		detail[0] = '\0';
		pthread_mutex_unlock(&lock);
		break;
	case RESULT_BAD_REQUEST:
		// One listen of the batch is the problem: they go one at a time until
		// it is the one being sent, and then it is let go.
		if (s->batch > 1) {
			s->batch = 1;
		} else {
			fprintf(stderr, "listenbrainz: a queued listen was refused (%s); dropping it\n", why);
			drop = true;
		}
		break;
	case RESULT_SESSION:
		if (!token_refused(why)) {
			s->retry_at = boot_seconds() + s->backoff;
			s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		}
		break;
	case RESULT_REFUSED:
		fprintf(stderr, "listenbrainz: listens refused: %s\n", why);
		pthread_mutex_lock(&lock);
		refused = true;
		snprintf(detail, sizeof(detail), "%s", why);
		pthread_mutex_unlock(&lock);
		s->retry_at = boot_seconds() + RETRY_MAX_SECS;
		break;
	case RESULT_RETRY:
		fprintf(stderr, "listenbrainz: listens not sent (%s); again in %.0f s\n", why, s->backoff);
		s->retry_at = boot_seconds() + s->backoff;
		s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		break;
	}

	pthread_mutex_lock(&lock);
	if (lines == 0) {
		// Nothing readable where the count says there are lines: the file is
		// gone or empty.
		file_count = 0;
	} else if (drop) {
		file_count = file_count > lines ? file_count - lines : 0;
	}
	// One at a time lasts until the queue is empty.
	if (file_count == 0) {
		s->batch = BATCH_MAX;
	}
	pthread_mutex_unlock(&lock);

	if (drop && lines > 0) {
		queue_drop_head(lines);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	sender_t sender = {.batch = BATCH_MAX, .backoff = RETRY_FIRST_SECS};

	for (;;) {
		pthread_mutex_lock(&lock);
		bool out = signout_requested;
		bool in = signin_requested;
		char tok[64] = "";
		if (in) {
			snprintf(tok, sizeof(tok), "%s", signin_token);
			wipe(signin_token, sizeof(signin_token));
		}
		signout_requested = false;
		signin_requested = false;
		pthread_mutex_unlock(&lock);

		if (out) {
			do_signout();
		}
		if (in) {
			do_signin(tok);
			wipe(tok, sizeof(tok));
			sender.retry_at = 0;
			sender.backoff = RETRY_FIRST_SECS;
		}

		pthread_mutex_lock(&lock);
		bool active = enabled && token[0];
		bool counting = counting_locked();
		char tk[64];
		snprintf(tk, sizeof(tk), "%s", token);
		pthread_mutex_unlock(&lock);

		// Listens go to the card while there is an account to keep them for;
		// they go to ListenBrainz only with a token.
		bool up = false;
		if (counting) {
			// The card comes and goes: pulled out, shared over USB, read-only.
			if (!card_writable()) {
				if (card_ready) {
					card_ready = false;
					pthread_mutex_lock(&lock);
					file_count = 0;
					pthread_mutex_unlock(&lock);
				}
			} else if (!card_ready) {
				card_ready = queue_attach(tk);
			}
			flush_pending(tk);
		}
		if (active) {
			up = online();
			if (up && !sender.was_online) {
				sender.retry_at = 0;
				sender.backoff = RETRY_FIRST_SECS;
			}
			sender.was_online = up;

			if (up && boot_seconds() >= sender.retry_at) {
				// "Now playing" once per track, for the track still playing,
				// and not retried.
				pthread_mutex_lock(&lock);
				bool announce = false;
				play_t track;
				if (now_playing_waiting && (now_playing_generation != current_generation || !current_playing)) {
					now_playing_waiting = false;
				} else if (now_playing_waiting) {
					now_playing_waiting = false;
					announce = true;
					track = now_playing;
				}
				bool queued = file_count > 0;
				pthread_mutex_unlock(&lock);

				if (announce) {
					send_now_playing(&track, tk);
				}
				if (queued) {
					send_queue(&sender, tk);
				}
			}
		}

		// Asleep until there is something to do. While listens wait for the
		// network, the clock or a card, awake now and then to look again; with
		// more to send and nothing in the way, straight on.
		pthread_mutex_lock(&lock);
		active = enabled && token[0];
		counting = counting_locked();
		bool more_now = active && up && file_count > 0 && boot_seconds() >= sender.retry_at;
		bool waiting = (counting && (!card_ready || pending_count > 0)) || (active && (file_count > 0 || now_playing_waiting));
		if (!signout_requested && !signin_requested && !more_now) {
			if (waiting) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				ts.tv_sec += WAIT_POLL_SECS;
				pthread_cond_timedwait(&wake, &lock, &ts);
			} else {
				pthread_cond_wait(&wake, &lock);
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the public side
// ---------------------------------------------------------------------------

void listenbrainz_init(void) {
	if (started) {
		return;
	}
	build_paths();

	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&wake, &attr);
	pthread_condattr_destroy(&attr);

	pthread_mutex_lock(&lock);
	enabled = config_get_bool("listenbrainz", "enabled", false);
	load_token();
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
		fprintf(stderr, "listenbrainz: cannot start the worker\n");
		return;
	}
	pthread_detach(thread);
	started = true;
	printf("listenbrainz: %s, %s\n", enabled ? "on" : "off", token[0] ? "signed in" : "signed out");
}

void listenbrainz_set_enabled(bool on) {
	config_set_bool("listenbrainz", "enabled", on);
	config_save();
	pthread_mutex_lock(&lock);
	enabled = on;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

bool listenbrainz_active(void) {
	if (!started) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool counting = counting_locked();
	pthread_mutex_unlock(&lock);
	return counting;
}

void listenbrainz_signin(const char *tok) {
	if (!started || !tok || !tok[0]) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (!signing_in) {
		snprintf(signin_token, sizeof(signin_token), "%s", tok);
		signing_in = true;
		signin_requested = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void listenbrainz_signout(void) {
	if (!started) {
		return;
	}
	pthread_mutex_lock(&lock);
	signout_requested = true;
	token[0] = '\0';
	token_expired = false;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void listenbrainz_get_status(listenbrainz_status_t *out) {
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	out->enabled = enabled;
	snprintf(out->detail, sizeof(out->detail), "%s", detail);
	out->queued = file_count + pending_count;
	if (!started) {
		// The worker never came up: nothing is sent or counted.
		out->state = LISTENBRAINZ_STATE_OFF;
	} else if (!enabled) {
		out->state = LISTENBRAINZ_STATE_OFF;
	} else if (token_expired) {
		out->state = LISTENBRAINZ_STATE_TOKEN_EXPIRED;
	} else if (!token[0]) {
		out->state = LISTENBRAINZ_STATE_SIGNED_OUT;
	} else if (refused) {
		out->state = LISTENBRAINZ_STATE_REFUSED;
	} else {
		out->state = LISTENBRAINZ_STATE_CONNECTED;
	}
	pthread_mutex_unlock(&lock);
}

bool listenbrainz_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = requests_in_flight > 0;
	pthread_mutex_unlock(&lock);
	return wanted;
}

// ---------------------------------------------------------------------------
// counting plays -- GUI thread only
// ---------------------------------------------------------------------------

static struct {
	char file[512];
	unsigned generation;
	bool skip;
	bool scrobbled;
	bool was_playing;
	double listened;
	double last_tick;
	double last_position;
	double boot_start;
	long long real_start;
	play_t track;
} play;

static void take_tags(const listenbrainz_playback_t *now) {
	copy_field(play.track.artist, now->artist);
	copy_field(play.track.title, now->title);
	copy_field(play.track.album, now->album);
}

static void start_play(const listenbrainz_playback_t *now, double t) {
	snprintf(play.file, sizeof(play.file), "%s", now->file);
	play.generation++;
	play.skip = now->skip;
	play.scrobbled = false;
	play.was_playing = true;
	play.listened = 0;
	play.last_tick = t;
	play.last_position = now->position;
	play.boot_start = t;
	play.real_start = clock_valid() ? (long long)time(NULL) : 0;
	memset(&play.track, 0, sizeof(play.track));
	take_tags(now);
}

static void qualify(void) {
	double length = play.track.duration;
	if (play.scrobbled || play.skip || !play.track.artist[0] || !play.track.title[0] || length < SCROBBLE_MIN_LENGTH) {
		return;
	}
	double enough = length / 2 < SCROBBLE_ENOUGH ? length / 2 : SCROBBLE_ENOUGH;
	if (play.listened < enough) {
		return;
	}
	play.scrobbled = true;

	play_t p = play.track;
	p.when = play.real_start;
	p.boot_start = play.boot_start;
	pthread_mutex_lock(&lock);
	if (pending_count < PENDING_MAX) {
		pending[pending_count++] = p;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void listenbrainz_note_playback(const listenbrainz_playback_t *now) {
	if (!listenbrainz_active()) {
		play.file[0] = '\0';
		return;
	}

	double t = boot_seconds();
	const char *file = now->file ? now->file : "";
	bool same = play.file[0] && strcmp(play.file, file) == 0;
	// Back at the start of the same file after being well into it: played
	// again, by repeat-one or by the user.
	bool again = same && now->position < 5.0 && now->position + 10.0 < play.last_position;

	bool announce = false;
	if (now->playing && file[0] && (!same || again)) {
		start_play(now, t);
		announce = true;
	} else if (!same) {
		play.file[0] = '\0';
		return;
	} else if (!play.track.title[0] && now->title && now->title[0]) {
		// The tags came in after the file started.
		take_tags(now);
		announce = true;
	}

	if (now->playing && play.was_playing) {
		double step = t - play.last_tick;
		// A longer gap is the device having been asleep, not listening.
		if (step > 0 && step < 5.0) {
			play.listened += step;
		}
	}
	play.was_playing = now->playing;
	play.last_tick = t;
	play.last_position = now->position;
	if (now->duration > 0) {
		play.track.duration = (int)(now->duration + 0.5);
	}

	pthread_mutex_lock(&lock);
	current_generation = play.generation;
	current_playing = now->playing;
	if (announce && token[0] && !play.skip && play.track.artist[0] && play.track.title[0]) {
		now_playing = play.track;
		now_playing_generation = play.generation;
		now_playing_waiting = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);

	qualify();
}
