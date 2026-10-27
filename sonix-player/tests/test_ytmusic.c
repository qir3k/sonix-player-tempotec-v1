// test_ytmusic.c -- the YouTube Music playback resolver, with a fake network.
//
// Three answers to "where are the bytes", tried in this order: the device's
// own player request (no account, no helper machine), a Piped or Invidious
// instance, and a relay of the user's own -- and the settings can pin any of
// them first. This suite links the real ytmusic.c against a fake http layer
// (real http.c needs the POSIX socket API the tests' Linux host has; the
// fakes keep the suite portable) and checks the resolver end to end:
//
//   - the phone-app player is asked first in "auto", and its refusal does not
//     end the search: a real reply from it plays with no relay at all;
//   - only audio/mp4 streams are picked, never audio/webm (the player reads
//     AAC in MP4; its Opus decoder reads Ogg, not the WebM YouTube wraps Opus
//     in) and never anything carrying video;
//   - the highest bitrate at or under the cap wins;
//   - title, artist and duration come from whichever source answered;
//   - a pinned Invidious instance is played through its /latest_version
//     endpoint, a Piped one only when its own address is what it answers with,
//     and an instance that merely names Google's address is not used;
//   - the settings list holds the built-in answers, the relay only once one is
//     configured, and the instances -- so the page has something to offer on a
//     player that was never set up;
//   - the error left standing says every source refused, not that a relay is
//     missing, because a relay is no longer required.
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

// A page Google really sent: the fixtures carry whole responses the device
// received (and once parsed into nothing). Loaded whole and registered with
// fake_reply() by the test that wants one.
static char *load_fixture(const char *name) {
	FILE *f = fopen(name, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *body = malloc((size_t)size + 1);
	if (!body) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(body, 1, (size_t)size, f);
	fclose(f);
	body[got] = '\0';
	return body;
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

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out,
				 size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	if (strstr(url, "/streams/")) relay_calls++;
	if (status_out) *status_out = 0;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	if (status_out) *status_out = reply->status;
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

// An Invidious instance's formats reply, trimmed to what the resolver reads:
// the facts, and three adaptive formats -- the Opus track it must pass over,
// a low-bitrate AAC one, and the 128k AAC one a track is played from.
static const char INVIDIOUS_FORMATS[] =
	"{\"title\":\"Never Gonna Give You Up\",\"author\":\"Rick Astley\",\"lengthSeconds\":213,"
	"\"adaptiveFormats\":["
	"{\"itag\":251,\"type\":\"audio/webm; codecs=\\\"opus\\\"\",\"bitrate\":160000,\"url\":\"https://inv.test/x\"},"
	"{\"itag\":139,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.5\\\"\",\"bitrate\":48000,\"url\":\"https://inv.test/y\"},"
	"{\"itag\":140,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":128000,\"url\":\"https://inv.test/z\"}"
	"]}";

// A Piped instance that fronts the bytes itself, which is the only kind of
// Piped answer this player can fetch. The webm is passed over, as always.
static const char PIPED_OWN_HOST[] =
	"{\"title\":\"Never Gonna Give You Up\",\"uploader\":\"Rick Astley\",\"duration\":213,"
	"\"audioStreams\":["
	"{\"url\":\"https://piped.test/webm\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000},"
	"{\"url\":\"https://piped.test/audio/140\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000}"
	"]}";

// The same instance handing out Google's address, which belongs to it and not
// to this device: nothing here can be fetched, so it is not the answer.
static const char PIPED_GOOGLE_ADDRESS[] =
	"{\"title\":\"x\",\"uploader\":\"y\",\"duration\":1,"
	"\"audioStreams\":[{\"url\":\"https://rr3---sn-abc.googlevideo.com/videoplayback?itag=140\","
	"\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000}]}";

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

static void set_relay(const char *base) { config_set("ytmusic", "relay_base", base); }

static void test_nothing_configured_says_what_to_do(void) {
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	// The device is asked first and refused, no instance answers, and there is
	// no relay either: the error says so without naming a relay as the only
	// way out, because it is not one any more.
	CHECK(strcmp(error, "ytmusic_no_source") == 0);
}

static void test_relay_picks_the_aac_stream(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED); // the phone-app player, as today
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	int duration = 0;
	char title[YTMUSIC_TITLE_MAX] = "", artist[YTMUSIC_NAME_MAX] = "";
	bool proxied = false;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), &duration, title, sizeof(title), artist,
								sizeof(artist), &proxied, error, sizeof(error));
	CHECK(ok);
	// The reply names another machine, so the bytes are asked of the relay, and
	// the address carries the format this end chose: 256k beats 128k, the webm
	// is skipped and the 400k stream is over the cap.
	CHECK(strcmp(url, "http://relay.test/audio/" VID "?f=141") == 0);
	CHECK(proxied);
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
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(ok);
	CHECK(strcmp(url, "http://relay.test/audio/" VID "?f=141") == 0);
}

// The phone-app player's reply, byte for byte as YouTube sent it to this
// device's own request shape, replayed through the real parser. This is the
// standalone path: the player resolves the address itself and the bytes come
// straight from Google, so a track plays with no relay and no other machine.
static void test_phone_player_reply_plays_on_its_own(void) {
	char *player = load_fixture("fixtures/player-android-dQw4w9WgXcQ.json");
	if (!player) {
		CHECK(0 && "the phone-app player fixture is missing");
		return;
	}
	ytmusic_logout(); // no account, no relay: nothing but the reply
	fake_reset();
	fake_reply("youtubei/v1/player", 200, player);
	set_relay("");

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool relayed = true;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, &relayed, error,
								sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the phone-app reply gave no stream: %s\n", error);
	CHECK(strncmp(url, "https://", 8) == 0);
	CHECK(url[0] && strstr(url, "googlevideo.com") != NULL);
	CHECK(!relayed); // the address is Google's, resolved here, and fetched as it stands
	free(player);
}

// An instance named in the settings is what plays the track. Invidious answers
// the formats reply, and its own /latest_version endpoint -- local=true -- is
// asked for the bytes: the instance fetches the audio and passes it on, which
// is the one instance shape a different machine can use.
// A pin to the relay asks the relay and nothing else -- the device would answer
// this one, and the point of the pin is that the relay is what is wanted.
static void test_relay_pin_asks_only_the_relay(void) {
	char *player = load_fixture("fixtures/player-android-dQw4w9WgXcQ.json");
	if (!player) {
		CHECK(0 && "the phone-app player fixture is missing");
		return;
	}
	fake_reset();
	fake_reply("youtubei/v1/player", 200, player); // a playable answer, left unused
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");
	CHECK(ytmusic_source_select("relay"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
								sizeof(error));
	CHECK(ok);
	CHECK(strncmp(url, "http://relay.test/audio/", 24) == 0);
	CHECK(ytmusic_source_select("auto"));
	set_relay("");
	free(player);
}

static void test_pinned_invidious_instance_plays(void) {
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	set_relay("");
	CHECK(ytmusic_source_select("https://inv.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	char title[YTMUSIC_TITLE_MAX] = "", artist[YTMUSIC_NAME_MAX] = "";
	int duration = 0;
	bool proxied = false;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), &duration, title, sizeof(title), artist,
								sizeof(artist), &proxied, error, sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the instance gave no stream: %s\n", error);
	CHECK(strcmp(url, "https://inv.test/latest_version?id=" VID "&itag=140&local=true") == 0);
	CHECK(proxied);
	CHECK(duration == 213);
	CHECK(strcmp(title, "Never Gonna Give You Up") == 0);
	CHECK(strcmp(artist, "Rick Astley") == 0);
	CHECK(ytmusic_source_select("auto"));
}

// A Piped instance fronting the bytes itself: its own address is fetched as it
// stands, so nothing is proxied and the address is kept whole.
static void test_pinned_piped_instance_plays(void) {
	fake_reset();
	fake_reply("piped.test/streams/", 200, PIPED_OWN_HOST);
	set_relay("");
	CHECK(ytmusic_source_select("https://piped.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool proxied = true;
	CHECK(ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, &proxied, error,
								sizeof(error)));
	CHECK(strcmp(url, "https://piped.test/audio/140") == 0);
	CHECK(!proxied);
	CHECK(ytmusic_source_select("auto"));
}

// An instance that only names Google's address cannot play the track from
// here, however healthy it is; the pin is not honoured with an address that
// will be refused, and the failure is named rather than silent.
static void test_instance_naming_google_is_not_the_answer(void) {
	fake_reset();
	fake_reply("piped.test/streams/", 200, PIPED_GOOGLE_ADDRESS);
	set_relay("");
	CHECK(ytmusic_source_select("https://piped.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
								sizeof(error));
	CHECK(!ok);
	CHECK(url[0] == '\0');
	CHECK(strcmp(error, "ytmusic_no_source") == 0);
	CHECK(ytmusic_source_select("auto"));
}

// The settings page lists exactly what the resolver can use: the default, the
// device on its own, a relay once one is configured, and the instances -- the
// built-in public list when nothing was ever set, so the page has something to
// choose from out of the box.
static void test_sources_list_the_choices(void) {
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	ytmusic_source_t sources[YTMUSIC_SOURCES_MAX];
	int count = ytmusic_sources(sources, YTMUSIC_SOURCES_MAX);
	CHECK(count >= 3);
	CHECK(strcmp(sources[0].value, "auto") == 0);
	CHECK(strcmp(sources[0].label, "ytmusic_source_auto") == 0);
	CHECK(strcmp(sources[1].value, "direct") == 0);
	bool has_relay = false, has_instance = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(sources[i].value, "relay") == 0) has_relay = true;
		if (strstr(sources[i].value, "://")) has_instance = true;
	}
	CHECK(!has_relay); // none configured: no row that could not play
	CHECK(has_instance);

	set_relay("http://relay.test");
	count = ytmusic_sources(sources, YTMUSIC_SOURCES_MAX);
	has_relay = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(sources[i].value, "relay") == 0) has_relay = true;
	}
	CHECK(has_relay);
	set_relay("");

	// A host typed on its own is an address: https is filled in, because that
	// is what people type and what every instance serves. Something that is
	// not a host at all is refused rather than stored.
	CHECK(!ytmusic_source_select("not a host"));
	CHECK(ytmusic_source_select("inv.example"));
	char typed[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(typed, sizeof(typed));
	CHECK(strcmp(typed, "https://inv.example") == 0);
	CHECK(ytmusic_source_select("direct"));
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	CHECK(strcmp(active, "direct") == 0);
	CHECK(ytmusic_source_select("auto"));
}

static void test_webm_is_never_the_answer(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 200, RELAY_WEBM_ONLY);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
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
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreadable") == 0);
}

static void test_unreachable_relay_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	// No relay reply at all: the transport fails before a status is read.
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreachable") == 0);
}

// A relay's answer is fetched from the relay, or not at all. The media
// addresses a Piped or Invidious instance answers with belong to Google and to
// that instance's own address: fetched from here they are refused (403) and
// they are longer than the player's URL buffer besides, which is what "could
// not reach the stream" used to mean. See ytmusic_relay_stream_url().
static void test_stream_url_stays_on_the_relay(void) {
	char out[YTMUSIC_URL_MAX];
	bool proxied = false;

	// The relay's own address is used as it stands: tools/ytmusic-relay.py
	// answers with exactly this.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 140,
								   "http://relay.test/audio/" VID "?f=140", out, sizeof(out), &proxied));
	CHECK(!proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID "?f=140") == 0);

	// Somebody else's machine: the player asks the relay for the audio instead
	// of fetching an address that will refuse it -- and names the format it
	// picked, so the relay does not choose again.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 140,
								   "https://rr3---sn-abc.googlevideo.com/videoplayback?itag=140", out,
								   sizeof(out), &proxied));
	CHECK(proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID "?f=140") == 0);

	// A host that only begins with the relay's name is still somebody else's.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 0, "http://relay.tests.example/audio/x",
								   out, sizeof(out), &proxied));
	CHECK(proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID) == 0);

	// Nothing to build a request from.
	CHECK(!ytmusic_relay_stream_url("", VID, 140, "https://relay.example/x", out, sizeof(out), &proxied));
	CHECK(!ytmusic_relay_stream_url("http://relay.test", "short", 140, "https://relay.example/x", out,
									sizeof(out), &proxied));
}

// And a relay with no /streams/{id} -- Invidious serves its own API shape there
// -- is named as a relay that cannot serve this player, not as a broken stream.
static void test_relay_without_the_endpoint_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 404, "{\"error\":\"not found\"}");
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_not_serving") == 0);
}

// The pages Google really sent this device, replayed through the real parser.
// These arrived whole (the bodies end like JSON and carry full shelves of
// items) and once parsed into zero rows; whatever the fault was, these bytes
// are the truth the fix has to satisfy.
static void test_real_device_pages(void) {
	char *search = load_fixture("fixtures/empty-search.json");
	if (search) {
		fake_reset();
		fake_reply("youtubei/v1/search", 200, search);
		set_relay("http://relay.test"); // harmless: search never reaches the relay
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_search_items("kanye", YTMUSIC_FILTER_SONGS, items, YTMUSIC_ITEMS_MAX, error,
										 sizeof(error));
		// The fixture holds a full "Songs" shelf: twenty responsive items.
		CHECK(count == 20);
		if (count > 0) {
			CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
			CHECK(items[0].id[0] != '\0');
			CHECK(items[0].title[0] != '\0');
			// The artwork the row draws comes from the page's own thumbnail
			// list -- the largest size it offers.
			CHECK(strncmp(items[0].thumbnail, "https://", 8) == 0);
		} else {
			fprintf(stderr, "  search fixture parsed %d rows, error %s\n", count, error);
		}
		free(search);
	}

	char *charts = load_fixture("fixtures/empty-FEmusic_charts.json");
	if (charts) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, charts);
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_browse_items("FEmusic_charts", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
		CHECK(count > 0);
		if (count <= 0) fprintf(stderr, "  charts fixture parsed %d rows, error %s\n", count, error);
		free(charts);
	}

	char *explore = load_fixture("fixtures/empty-explore.json");
	if (explore) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, explore);
		ytmusic_section_t sections[YTMUSIC_SECTIONS_MAX];
		char error[128] = "";
		int count = ytmusic_explore_sections(sections, YTMUSIC_SECTIONS_MAX, error, sizeof(error));
		// The fixture carries four carousels; "Moods & genres" is a grid of
		// filter buttons and is dropped on purpose, so three sections show.
		CHECK(count == 3);
		if (count > 0) {
			CHECK(sections[0].count > 0);
			CHECK(sections[0].items[0].title[0] != '\0');
		} else {
			fprintf(stderr, "  explore fixture parsed %d sections, error %s\n", count, error);
		}
		// Each carousel is named after what it holds. The headings are read from
		// the header object at the front of each shelf; when that read failed the
		// page drew three rows all labelled "Explore", so the names are the check
		// and not only the count.
		CHECK(strcmp(sections[0].title, "New albums & singles") == 0);
		if (count > 1) CHECK(strcmp(sections[1].title, "Trending") == 0);
		for (int i = 0; i < count; i++) {
			CHECK(sections[i].title[0] != '\0');
			// And every card carries the cover its row draws.
			CHECK(sections[i].items[0].thumbnail[0] != '\0');
			if (!sections[i].items[0].thumbnail[0]) {
				fprintf(stderr, "  section %d (%s) has no thumbnail on its first card\n", i,
						sections[i].title);
			}
		}
		free(explore);
	}

	// And the one page that is REALLY empty signed out: liked songs, which is
	// Google's own "sign in" card. Zero rows is the correct answer there, with
	// no error attached.
	char *vllm = load_fixture("fixtures/empty-VLLM.json");
	if (vllm) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, vllm);
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
		CHECK(count == 0);
		free(vllm);
	}

	fake_reset();
}

// The library pages need the session imported from the card, and the player has
// to say so. A Google sign-in token is refused by the music endpoints, and
// refused quietly: the page comes back empty rather than with an error, so
// Favourites answered "no results" for an account that had in fact signed in.
//
// The sidecar is seeded by hand here, in the two shapes the player can hold: an
// OAuth account with no browser session beside it (which cannot serve the
// library), and an imported session (which can).
static void write_session_file(const char *body) {
	FILE *f = fopen(".build/ytmusic_session.ini", "w");
	if (!f) {
		CHECK(0 && "the sidecar could not be written");
		return;
	}
	fputs(body, f);
	fclose(f);
	ytmusic_init(); // the sidecar is read at init, so this is a reboot
}

static void test_library_says_what_is_missing(void) {
	char *vllm = load_fixture("fixtures/empty-VLLM.json");
	if (!vllm) return;

	// Signed in with Google: an account is attached, and it is not the browser
	// session the library needs.
	write_session_file("access_token=fake\nrefresh_token=fake\nexpires_at=0\n");
	CHECK(ytmusic_logged_in());
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, vllm);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
	CHECK(count == -1);
	CHECK(strcmp(error, "ytmusic_library_needs_session") == 0);

	// The imported session: the same empty page is then an honest empty list,
	// with nothing said about the account.
	write_session_file("cookie=SID=x; SAPISID=abcdefghijklmnop; HSID=y\n");
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, vllm);
	error[0] = '\0';
	count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
	CHECK(count == 0);
	CHECK(error[0] == '\0');

	ytmusic_logout();
	CHECK(!ytmusic_logged_in());
	free(vllm);
}

int main(void) {
	FILE *seed = fopen(".build/test_ytmusic_config.ini", "w");
	if (seed) {
		fclose(seed);
	}
	config_init(".build/test_ytmusic_config.ini");
	streamkeys_init();
	ytmusic_init();

	test_nothing_configured_says_what_to_do();
	test_relay_picks_the_aac_stream();
	test_phone_player_is_asked_first();
	test_phone_player_reply_plays_on_its_own();
	test_relay_pin_asks_only_the_relay();
	test_pinned_invidious_instance_plays();
	test_pinned_piped_instance_plays();
	test_instance_naming_google_is_not_the_answer();
	test_sources_list_the_choices();
	test_webm_is_never_the_answer();
	test_unreadable_relay_names_itself();
	test_unreachable_relay_names_itself();
	test_stream_url_stays_on_the_relay();
	test_relay_without_the_endpoint_names_itself();
	test_real_device_pages();
	test_library_says_what_is_missing();

	if (failures) {
		fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_ytmusic: ok\n");
	return 0;
}
