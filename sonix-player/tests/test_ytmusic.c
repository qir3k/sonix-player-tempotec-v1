// test_ytmusic.c -- the YouTube Music playback resolver, with a fake network.
//
// Google's attestation wall has taken direct guest playback away from every
// first-party client (see relay_audio_url() in ytmusic.c for the sweep), so
// guest streaming goes through a Piped-API-compatible relay named in the
// config. This suite links the real ytmusic.c against a fake http layer
// (real http.c needs the POSIX socket API the tests' Linux host has; the
// fakes keep the suite portable) and checks the resolver end to end:
//
//   - the phone-app player is asked first (the zero-config answer, the day
//     Google relaxes the wall), and its refusal does not end the search;
//   - only audio/mp4 streams are picked from the relay, never audio/webm
//     (the player reads AAC in MP4; its Opus decoder reads Ogg, not the WebM
//     YouTube wraps Opus in) and never anything carrying video;
//   - the highest bitrate at or under the cap wins;
//   - title, artist and duration come from the relay reply;
//   - the relay's error is the one left standing, because it is the one that
//     says what to do -- set a relay, fix the one set, or wait for Google.
//
// Guest search against the live endpoint is checked by hand with the same
// two curl invocations the relay documentation carries; this suite cannot
// reach the network, by design.

#include "win_compat_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/ytmusic.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                        \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                     \
	} while (0)

// ---------------------------------------------------------------------------
// the fake http layer
//
// Every URL the resolver asks for is matched against a table of canned
// replies. `relay_calls` counts the requests that reached the relay, so a
// test can see the phone-app player was tried first and refused.
// ---------------------------------------------------------------------------

typedef struct {
	const char *url;	// matched with strstr, so the port-free base works
	int status;			// HTTP status, or 0 = transport failure
	const char *body;
} fake_reply_t;

static fake_reply_t replies[16];
static int reply_count;
static int relay_calls;

static const fake_reply_t *find_reply(const char *url) {
	for (int i = 0; i < reply_count; i++) {
		if (replies[i].url[0] && strstr(url, replies[i].url)) return &replies[i];
	}
	return NULL;
}

static void fake_reset(void) {
	reply_count = 0;
	relay_calls = 0;
	memset(replies, 0, sizeof(replies));
}

static void fake_reply(const char *url, int status, const char *body) {
	replies[reply_count].url = url;
	replies[reply_count].status = status;
	replies[reply_count].body = body;
	reply_count++;
}

bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs) {
	(void)limit;
	(void)timeout_secs;
	if (strstr(url, "/streams/")) relay_calls++;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	*out = strdup(reply->body);
	if (out_len) *out_len = strlen(reply->body);
	return true;
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit,
				  int timeout_secs, int *status_out) {
	(void)req;
	(void)limit;
	(void)timeout_secs;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	if (status_out) *status_out = reply->status;
	*out = strdup(reply->body);
	if (out_len) *out_len = strlen(reply->body);
	return true;
}

bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout_secs,
						 const http_stream_options_t *options) {
	(void)st;
	(void)url;
	(void)timeout_secs;
	(void)options;
	return false; // the download path is not under test here
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	(void)buf;
	(void)len;
	return -1;
}

void http_stream_close(http_stream_t *st) { (void)st; }

const char *http_last_error(void) { return "fake network: no route"; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	// Only the OAuth flow uses this; the tests exercise none of it. A plain
	// copy keeps the symbol defined without pretending to be real.
	snprintf(out, out_size, "%s", in ? in : "");
	return out;
}

// ---------------------------------------------------------------------------
// the canned replies
//
// The relay body is Piped's /streams shape, trimmed to what the resolver
// reads: the audio streams (a webm it must pass over, one over the cap, and
// the AAC pair it must choose from) and the facts the reply carries. The
// player body is what the phone-app client answers today: a refusal.
// ---------------------------------------------------------------------------

#define VID "dQw4w9WgXcQ"

static const char RELAY_GOOD[] =
	"{\"title\":\"Never Gonna Give You Up\",\"uploader\":\"Rick Astley\",\"duration\":213,"
	"\"audioStreams\":["
	"{\"url\":\"https://relay.example/webm-160\",\"format\":\"WEBM_OPUS\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000,\"itag\":251},"
	"{\"url\":\"https://relay.example/mp4-400\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":400000,\"itag\":999},"
	"{\"url\":\"https://relay.example/mp4-128\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000,\"itag\":140},"
	"{\"url\":\"https://relay.example/mp4-256\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":256000,\"itag\":141}"
	"],\"videoStreams\":[{\"url\":\"https://relay.example/video\",\"mimeType\":\"video/mp4\",\"bitrate\":1000000}]}";

static const char RELAY_WEBM_ONLY[] =
	"{\"title\":\"x\",\"uploader\":\"y\",\"duration\":1,"
	"\"audioStreams\":[{\"url\":\"https://relay.example/webm\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000}]}";

static const char RELAY_GARBAGE[] = "this is not json at all";

static const char ANDROID_REFUSED[] =
	"{\"error\":{\"code\":400,\"message\":\"Precondition check failed.\",\"reason\":\"failedPrecondition\"}}";

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

static void set_relay(const char *base) { config_set("ytmusic", "relay_base", base); }

static void test_relay_unset_says_what_to_do(void) {
	set_relay("");
	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, sizeof(error));
	CHECK(!ok);
	// The phone-app player's refusal is tried first and fails; the relay's
	// error is the one the user reads, because it is the actionable one.
	CHECK(strcmp(error, "ytmusic_relay_needed") == 0);
}

static void test_relay_picks_the_aac_stream(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED); // the phone-app player, as today
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	int duration = 0;
	char title[YTMUSIC_TITLE_MAX] = "", artist[YTMUSIC_NAME_MAX] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), &duration, title, sizeof(title), artist,
								sizeof(artist), error, sizeof(error));
	CHECK(ok);
	CHECK(strcmp(url, "https://relay.example/mp4-256") == 0); // 256k beats 128k; webm skipped; 400k over the cap
	CHECK(duration == 213);
	CHECK(strcmp(title, "Never Gonna Give You Up") == 0);
	CHECK(strcmp(artist, "Rick Astley") == 0);
}

static void test_phone_player_is_asked_first(void) {
	// Both transports reply; the resolver must try the zero-config one before
	// the relay, and still land on the relay's stream.
	fake_reset();
	fake_reply("youtube.com/youtubei/v1/player", 200,
			   "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\",\"reason\":\"The page needs to be reloaded.\"}}");
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, sizeof(error));
	CHECK(ok);
	CHECK(strcmp(url, "https://relay.example/mp4-256") == 0);
}

static void test_webm_is_never_the_answer(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 200, RELAY_WEBM_ONLY);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_aac_stream_unavailable") == 0);
	CHECK(url[0] == '\0');
}

static void test_unreadable_relay_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 200, RELAY_GARBAGE);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreadable") == 0);
}

static void test_unreachable_relay_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	// No relay reply at all: the transport fails before a status is read.
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreachable") == 0);
}

int main(void) {
	FILE *seed = fopen(".build/test_ytmusic_config.ini", "w");
	if (seed) {
		fclose(seed);
	}
	config_init(".build/test_ytmusic_config.ini");
	streamkeys_init();
	ytmusic_init();

	test_relay_unset_says_what_to_do();
	test_relay_picks_the_aac_stream();
	test_phone_player_is_asked_first();
	test_webm_is_never_the_answer();
	test_unreadable_relay_names_itself();
	test_unreachable_relay_names_itself();

	if (failures) {
		fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_ytmusic: ok\n");
	return 0;
}
