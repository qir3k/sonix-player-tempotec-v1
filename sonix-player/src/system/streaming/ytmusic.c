#define _GNU_SOURCE 1
#include "ytmusic.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/sha1.h"
#include "src/system/decode/growfile.h"

// Declared here because the tag writer near the top of the file also needs to
// know where a file it is describing sits; defined beside the downloader.
static const char *playback_cache_dir(void);
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define YTMUSIC_SESSION_FILE "ytmusic_session.ini"
#define YTMUSIC_COOKIE_MAX 6000
#define YTMUSIC_RESPONSE_LIMIT (3 * 1024 * 1024)
#define YTMUSIC_RENDERER_SIZE_LIMIT (256 * 1024)
#define YTMUSIC_RENDERER_TOKEN_LIMIT 8192
#define YTMUSIC_PLAYER_TOKEN_LIMIT 24000
#define YTMUSIC_TIMEOUT 15
#define YTMUSIC_SITE "https://music.youtube.com"
#define YTMUSIC_OAUTH_DEVICE_URL "https://www.youtube.com/o/oauth2/device/code"
#define YTMUSIC_OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
// The full YouTube scope, not youtube.readonly. The music endpoints answer a
// readonly token with an empty page: InnerTube has no read-only notion of the
// library, and ytmusicapi -- the client this follows -- asks for this one. A
// token granted only the readonly scope still works for the public pages, and
// youtubei_post() falls back to those when the account is refused.
#define YTMUSIC_OAUTH_SCOPE "https://www.googleapis.com/auth/youtube"
// The grant type the TV-device flow exchanges a code with. The standard
// urn:ietf:...:device_code grant issues a token the music endpoints refuse.
#define YTMUSIC_OAUTH_DEVICE_GRANT "http://oauth.net/grant_type/device/1.0"

// The web client's InnerTube key, supplied by the build.
//
// It is not an account credential and not a secret of anybody's: it is the key
// the music.youtube.com page hands every visitor, and it is used directly
// rather than scraped from the page because that page answers a client whose
// User-Agent it does not recognise -- and this player is not a browser -- with
// a "your browser is deprecated" stub that contains no key at all. That stub is
// what ytmusic_context_unreadable was.
//
// It is still not committed: it belongs to Google rather than to this
// repository, secret scanners flag the AIza... form wherever it appears (GitHub
// did, on the commit that first carried it), and a key baked into a public
// image is one Google can rotate out from under every build of it. So the
// value arrives from the build -- `make YTMUSIC_API_KEY=...`, the environment,
// or a .ytmusic-key file the Makefile reads -- and a build that was given none
// says so at the first request rather than sending one that cannot work.
#ifndef YTMUSIC_API_KEY
#define YTMUSIC_API_KEY ""
#endif

// The client version is the page's build stamp, in the same "1.YYYYMMDD.01.00"
// form the public clients use, derived from today's date so it never goes
// stale in a shipped image.
#define YTMUSIC_CLIENT_VERSION_FALLBACK "1.20250101.01.00"

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char session_cookie[YTMUSIC_COOKIE_MAX];
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static char api_key[128];
static char client_version[64];
static char oauth_client_id[256];
static char oauth_client_secret[256];
static char access_token[2048];
static char refresh_token[1024];
static time_t access_expires_at;

static void set_error(char *out, size_t size, const char *reason) {
	if (out && size) snprintf(out, size, "%s", reason ? reason : "ytmusic_request_failed");
}

static void wipe_free(char *s) {
	if (!s) return;
	volatile unsigned char *p = (volatile unsigned char *)s;
	size_t n = strlen(s);
	while (n--) *p++ = 0;
	free(s);
}

static void session_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) return;
	const char *slash = strrchr(cfg, '/');
	if (slash) snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, YTMUSIC_SESSION_FILE);
	else snprintf(out, size, "%s", YTMUSIC_SESSION_FILE);
}

static bool save_session(void) {
	char path[600], tmp[640];
	session_path(path, sizeof(path));
	if (!path[0]) return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	if (fchmod(fileno(f), 0600) != 0) {
		fclose(f);
		remove(tmp);
		return false;
	}
	bool wrote = fprintf(f, "cookie=%s\naccess_token=%s\nrefresh_token=%s\nexpires_at=%ld\n",
						 session_cookie, access_token, refresh_token, (long)access_expires_at) > 0;
	bool closed = fclose(f) == 0;
	bool ok = wrote && closed;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static bool has_session_cookie(const char *cookie) {
	static const char *const names[] = {"SAPISID=", "__Secure-3PAPISID=", "__Secure-1PAPISID="};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *p = cookie;
		while ((p = strstr(p, names[i])) != NULL) {
			if (p == cookie || p[-1] == ';' || isspace((unsigned char)p[-1])) {
				const char *v = p + strlen(names[i]);
				if (*v && *v != ';' && *v != '\r' && *v != '\n') return true;
			}
			p++;
		}
	}
	return false;
}

static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char *line = malloc(YTMUSIC_COOKIE_MAX);
	if (!line) { fclose(f); return; }
	while (fgets(line, YTMUSIC_COOKIE_MAX, f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (strncmp(line, "cookie=", 7) == 0 && has_session_cookie(line + 7)) snprintf(session_cookie, sizeof(session_cookie), "%s", line + 7);
		else if (strncmp(line, "access_token=", 13) == 0) snprintf(access_token, sizeof(access_token), "%s", line + 13);
		else if (strncmp(line, "refresh_token=", 14) == 0) snprintf(refresh_token, sizeof(refresh_token), "%s", line + 14);
		else if (strncmp(line, "expires_at=", 11) == 0) access_expires_at = (time_t)strtol(line + 11, NULL, 10);
	}
	memset(line, 0, YTMUSIC_COOKIE_MAX);
	free(line);
	fclose(f);
}

// Where an empty page's body is kept for pulling: the card's root, when one
// was named.
static char card_root[600];

void ytmusic_set_card_root(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&session_lock);
	snprintf(card_root, sizeof(card_root), "%s", path);
	// Without a trailing slash, so the joins below cannot double one up.
	size_t n = strlen(card_root);
	while (n > 1 && card_root[n - 1] == '/') {
		card_root[--n] = '\0';
	}
	pthread_mutex_unlock(&session_lock);
}

void ytmusic_init(void) {
	pthread_mutex_lock(&session_lock);
	load_session();
	pthread_mutex_unlock(&session_lock);
	snprintf(oauth_client_id, sizeof(oauth_client_id), "%s", streamkeys_ytmusic_client_id() ? streamkeys_ytmusic_client_id() : "");
	snprintf(oauth_client_secret, sizeof(oauth_client_secret), "%s", streamkeys_ytmusic_client_secret() ? streamkeys_ytmusic_client_secret() : "");
}

bool ytmusic_logged_in(void) {
	pthread_mutex_lock(&session_lock);
	bool logged_in = session_cookie[0] != '\0' || refresh_token[0] != '\0';
	pthread_mutex_unlock(&session_lock);
	return logged_in;
}

bool ytmusic_oauth_configured(void) { return oauth_client_id[0] != '\0' && oauth_client_secret[0] != '\0'; }

bool ytmusic_set_session_cookie(const char *cookie, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!cookie || strnlen(cookie, YTMUSIC_COOKIE_MAX) >= YTMUSIC_COOKIE_MAX ||
		!has_session_cookie(cookie) || strpbrk(cookie, "\r\n")) {
		set_error(error, error_size, "ytmusic_invalid_session");
		return false;
	}
	pthread_mutex_lock(&session_lock);
	char old_cookie[sizeof(session_cookie)], old_access[sizeof(access_token)], old_refresh[sizeof(refresh_token)];
	time_t old_expiry = access_expires_at;
	snprintf(old_cookie, sizeof(old_cookie), "%s", session_cookie);
	snprintf(old_access, sizeof(old_access), "%s", access_token);
	snprintf(old_refresh, sizeof(old_refresh), "%s", refresh_token);
	memset(access_token, 0, sizeof(access_token));
	memset(refresh_token, 0, sizeof(refresh_token));
	access_expires_at = 0;
	snprintf(session_cookie, sizeof(session_cookie), "%s", cookie);
	if (!save_session()) {
		snprintf(session_cookie, sizeof(session_cookie), "%s", old_cookie);
		snprintf(access_token, sizeof(access_token), "%s", old_access);
		snprintf(refresh_token, sizeof(refresh_token), "%s", old_refresh);
		access_expires_at = old_expiry;
		memset(old_cookie, 0, sizeof(old_cookie));
		memset(old_access, 0, sizeof(old_access));
		memset(old_refresh, 0, sizeof(old_refresh));
		pthread_mutex_unlock(&session_lock);
		set_error(error, error_size, "ytmusic_session_save_failed");
		return false;
	}
	memset(old_cookie, 0, sizeof(old_cookie));
	memset(old_access, 0, sizeof(old_access));
	memset(old_refresh, 0, sizeof(old_refresh));
	pthread_mutex_unlock(&session_lock);
	return true;
}

// The card file may be a whole browser export -- a Netscape cookies.txt of a
// few thousand bytes -- while the Cookie header built from it is only the
// YouTube and Google lines, which is why the file has its own, larger limit.
#define YTMUSIC_SESSION_FILE_MAX (64 * 1024)

// A browser's "export cookies" writes a Netscape file -- one tab-separated
// line per cookie -- and that is what people put on the card, because it is the
// only thing their browser will hand them. Reading only a single bare Cookie
// header meant the usual export was refused as "invalid", which looked exactly
// like a session Google had rejected. Both shapes are read now: a Netscape file
// is turned into a Cookie header here, and anything else is taken as one.
static bool cookies_from_netscape(const char *file, char *out, size_t size) {
	size_t used = 0;
	out[0] = '\0';
	char line[1024];
	const char *p = file;
	while (*p) {
		size_t n = 0;
		while (p[n] && p[n] != '\n' && n + 1 < sizeof(line)) n++;
		memcpy(line, p, n);
		line[n] = '\0';
		// Whatever is left of an over-long line is dropped rather than split:
		// half a cookie value is a wrong cookie, not a shorter one.
		while (p[n] && p[n] != '\n') n++;
		p += n;
		if (*p == '\n') p++;
		char *cr = strchr(line, '\r');
		if (cr) *cr = '\0';
		if (line[0] == '#' || line[0] == '\0') continue;
		// domain, flag, path, secure, expiry, name, value
		char *field[7];
		int count = 0;
		char *at = line;
		while (count < 7) {
			field[count++] = at;
			char *tab = strchr(at, '\t');
			if (!tab) break;
			*tab = '\0';
			at = tab + 1;
		}
		if (count < 7) continue;
		if (!strstr(field[0], "youtube.com") && !strstr(field[0], "google.com")) continue;
		if (!field[5][0]) continue;
		int wrote = snprintf(out + used, size - used, "%s%s=%s", used ? "; " : "", field[5], field[6]);
		if (wrote <= 0 || (size_t)wrote >= size - used) return false;
		used += (size_t)wrote;
	}
	return used > 0;
}

bool ytmusic_import_session_file(const char *card_root, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!card_root || !card_root[0] || strnlen(card_root, 512) >= 512) {
		set_error(error, error_size, "ytmusic_session_file_missing");
		return false;
	}
	char path[600];
	int n = snprintf(path, sizeof(path), "%s/%s", card_root, "ytmusic_session.txt");
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		set_error(error, error_size, "ytmusic_session_file_missing");
		return false;
	}
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > YTMUSIC_SESSION_FILE_MAX) {
		set_error(error, error_size, "ytmusic_session_file_invalid");
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		set_error(error, error_size, "ytmusic_session_file_unreadable");
		return false;
	}
	char *file = calloc(YTMUSIC_SESSION_FILE_MAX + 1, 1);
	if (!file) {
		fclose(f);
		set_error(error, error_size, "ytmusic_out_of_memory");
		return false;
	}
	size_t got = fread(file, 1, YTMUSIC_SESSION_FILE_MAX, f);
	file[got] = '\0';
	fclose(f);
	bool ok = false;
	if (got > 0 && strchr(file, '\t') &&
		(strstr(file, "youtube.com") || strstr(file, "google.com"))) {
		char *header = calloc(YTMUSIC_COOKIE_MAX, 1);
		if (header) {
			if (cookies_from_netscape(file, header, YTMUSIC_COOKIE_MAX)) {
				ok = ytmusic_set_session_cookie(header, error, error_size);
			}
			memset(header, 0, YTMUSIC_COOKIE_MAX);
			free(header);
		}
	} else {
		char *line_end = strpbrk(file, "\r\n");
		if (line_end) *line_end = '\0';
		char *value = strncmp(file, "cookie=", 7) == 0 ? file + 7 : file;
		// Accept the exact value emitted by browser cookie exporters as well as a
		// bare Cookie header value. Keeping the field name would otherwise result
		// in an invalid nested `Cookie: Cookie: ...` request after import/reboot.
		if (strncasecmp(value, "Cookie:", 7) == 0) {
			value += 7;
			while (*value == ' ' || *value == '\t') value++;
		}
		ok = got > 0 && ytmusic_set_session_cookie(value, error, error_size);
	}
	memset(file, 0, YTMUSIC_SESSION_FILE_MAX);
	free(file);
	if (!ok) {
		if (!error || !error_size || !error[0]) set_error(error, error_size, "ytmusic_session_file_invalid");
		return false;
	}
	// The internal copy is now persisted with mode 0600. Remove the removable
	// card copy as well so it is not left exposed if the card changes hands.
	if (remove(path) != 0) {
		fprintf(stderr, "ytmusic: imported session file could not be removed\n");
	}
	return true;
}

void ytmusic_session_cookie(char *out, size_t size) {
	if (!out || !size) return;
	pthread_mutex_lock(&session_lock);
	snprintf(out, size, "%s", session_cookie);
	pthread_mutex_unlock(&session_lock);
}

bool ytmusic_logout(void) {
	pthread_mutex_lock(&session_lock);
	memset(session_cookie, 0, sizeof(session_cookie));
	memset(access_token, 0, sizeof(access_token));
	memset(refresh_token, 0, sizeof(refresh_token));
	access_expires_at = 0;
	char path[600];
	session_path(path, sizeof(path));
	bool ok = !path[0] || remove(path) == 0 || errno == ENOENT;
	pthread_mutex_unlock(&session_lock);
	return ok;
}

static bool oauth_post_locked(const char *url, const char *form, char **response, int *status,
							  char *error, size_t error_size) {
	const char *headers = "Content-Type: application/x-www-form-urlencoded\r\n";
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = form,
		.content_type = "application/x-www-form-urlencoded", .want_error_body = true,
		.no_https_downgrade = true};
	int status_code = 0;
	if (!http_request(url, &req, response, NULL, 64 * 1024, YTMUSIC_TIMEOUT, &status_code)) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_oauth_no_reply");
		return false;
	}
	if (status) *status = status_code;
	return true;
}

bool ytmusic_login_device_begin(ytmusic_device_login_t *login, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!login || !oauth_client_id[0] || !oauth_client_secret[0]) {
		set_error(error, error_size, "ytmusic_oauth_keys_missing");
		return false;
	}
	memset(login, 0, sizeof(*login));
	char client_id[768], scope[256], form[1200];
	http_url_encode(oauth_client_id, client_id, sizeof(client_id));
	http_url_encode(YTMUSIC_OAUTH_SCOPE, scope, sizeof(scope));
	snprintf(form, sizeof(form), "client_id=%s&scope=%s", client_id, scope);
	char *body = NULL;
	int status = 0;
	pthread_mutex_lock(&request_lock);
	bool ok = oauth_post_locked(YTMUSIC_OAUTH_DEVICE_URL, form, &body, &status, error, error_size);
	pthread_mutex_unlock(&request_lock);
	memset(form, 0, sizeof(form));
	if (!ok) return false;
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		wipe_free(body);
		set_error(error, error_size, "ytmusic_oauth_unreadable_reply");
		return false;
	}
	int root = json_root(&doc);
	char api_error[96] = "";
	json_obj_str(&doc, root, "error", api_error, sizeof(api_error));
	json_obj_str(&doc, root, "device_code", login->device_code, sizeof(login->device_code));
	json_obj_str(&doc, root, "user_code", login->user_code, sizeof(login->user_code));
	if (!json_obj_str(&doc, root, "verification_url_complete", login->verification_url,
				   sizeof(login->verification_url))) {
		json_obj_str(&doc, root, "verification_url", login->verification_url, sizeof(login->verification_url));
	}
	login->interval_secs = (int)json_obj_long(&doc, root, "interval", 5);
	login->expires_secs = (int)json_obj_long(&doc, root, "expires_in", 1800);
	json_free(&doc);
	wipe_free(body);
	if (status < 200 || status >= 300 || !login->device_code[0] || !login->user_code[0] || !login->verification_url[0]) {
		set_error(error, error_size, api_error[0] ? api_error : "ytmusic_oauth_request_refused");
		return false;
	}
	if (login->interval_secs < 1) login->interval_secs = 5;
	return true;
}

ytmusic_login_state_t ytmusic_login_device_poll(ytmusic_device_login_t *login,
											 char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!login || !login->device_code[0] || !oauth_client_id[0] || !oauth_client_secret[0]) {
		set_error(error, error_size, "ytmusic_oauth_invalid_device_code");
		return YTMUSIC_LOGIN_ERROR;
	}
	char device_code[1400], client_id[768], secret[768], grant[128], form[3500];
	http_url_encode(login->device_code, device_code, sizeof(device_code));
	http_url_encode(oauth_client_id, client_id, sizeof(client_id));
	http_url_encode(oauth_client_secret, secret, sizeof(secret));
	http_url_encode(YTMUSIC_OAUTH_DEVICE_GRANT, grant, sizeof(grant));
	int n = snprintf(form, sizeof(form), "client_id=%s&code=%s&grant_type=%s", client_id, device_code, grant);
	if (oauth_client_secret[0] && n > 0 && (size_t)n < sizeof(form)) {
		size_t used = (size_t)n;
		snprintf(form + used, sizeof(form) - used, "&client_secret=%s", secret);
	}
	char *body = NULL;
	int status = 0;
	pthread_mutex_lock(&request_lock);
	bool posted = oauth_post_locked(YTMUSIC_OAUTH_TOKEN_URL, form, &body, &status, error, error_size);
	pthread_mutex_unlock(&request_lock);
	memset(form, 0, sizeof(form));
	memset(device_code, 0, sizeof(device_code));
	memset(secret, 0, sizeof(secret));
	if (!posted) return YTMUSIC_LOGIN_ERROR;
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		wipe_free(body);
		set_error(error, error_size, "ytmusic_oauth_unreadable_reply");
		return YTMUSIC_LOGIN_ERROR;
	}
	int root = json_root(&doc);
	char api_error[96] = "", new_access[sizeof(access_token)] = "";
	char new_refresh[sizeof(refresh_token)] = "";
	json_obj_str(&doc, root, "error", api_error, sizeof(api_error));
	if (status >= 200 && status < 300) {
		json_obj_str(&doc, root, "access_token", new_access, sizeof(new_access));
		json_obj_str(&doc, root, "refresh_token", new_refresh, sizeof(new_refresh));
		long expires = json_obj_long(&doc, root, "expires_in", 3600);
		if (!new_access[0] || !new_refresh[0]) {
			api_error[0] = '\0';
			set_error(error, error_size, "ytmusic_oauth_tokens_missing");
			json_free(&doc);
			wipe_free(body);
			return YTMUSIC_LOGIN_ERROR;
		}
		pthread_mutex_lock(&session_lock);
		char old_cookie[sizeof(session_cookie)], old_access[sizeof(access_token)];
		char old_refresh[sizeof(refresh_token)];
		time_t old_expiry = access_expires_at;
		snprintf(old_cookie, sizeof(old_cookie), "%s", session_cookie);
		snprintf(old_access, sizeof(old_access), "%s", access_token);
		snprintf(old_refresh, sizeof(old_refresh), "%s", refresh_token);
		snprintf(access_token, sizeof(access_token), "%s", new_access);
		snprintf(refresh_token, sizeof(refresh_token), "%s", new_refresh);
		memset(session_cookie, 0, sizeof(session_cookie));
		access_expires_at = time(NULL) + (expires > 60 ? expires - 60 : expires);
		bool saved = save_session();
		if (!saved) {
			snprintf(session_cookie, sizeof(session_cookie), "%s", old_cookie);
			snprintf(access_token, sizeof(access_token), "%s", old_access);
			snprintf(refresh_token, sizeof(refresh_token), "%s", old_refresh);
			access_expires_at = old_expiry;
		}
		pthread_mutex_unlock(&session_lock);
		memset(old_cookie, 0, sizeof(old_cookie));
		memset(old_access, 0, sizeof(old_access));
		memset(old_refresh, 0, sizeof(old_refresh));
		memset(new_access, 0, sizeof(new_access));
		memset(new_refresh, 0, sizeof(new_refresh));
		json_free(&doc);
		wipe_free(body);
		if (!saved) {
			set_error(error, error_size, "ytmusic_session_save_failed");
			return YTMUSIC_LOGIN_ERROR;
		}
		return YTMUSIC_LOGIN_OK;
	}
	json_free(&doc);
	wipe_free(body);
	if (strcmp(api_error, "authorization_pending") == 0) return YTMUSIC_LOGIN_PENDING;
	if (strcmp(api_error, "slow_down") == 0) {
		login->interval_secs += 5;
		return YTMUSIC_LOGIN_PENDING;
	}
	if (strcmp(api_error, "expired_token") == 0) return YTMUSIC_LOGIN_EXPIRED;
	set_error(error, error_size, api_error[0] ? api_error : "ytmusic_oauth_request_refused");
	return YTMUSIC_LOGIN_ERROR;
}

// The API key and the client version the request headers and bodies carry.
// Fixed at the first request and never refetched: neither changes while the
// player runs, and the version is today's date at boot.
static bool ensure_context(char *error, size_t error_size) {
	// A build with no key can make no InnerTube request at all: the endpoint
	// answers a request without one with the same stub an unknown User-Agent
	// gets, which used to be reported as "no rows" and left the page looking
	// empty. Say which piece is missing, once, and name it.
	if (!YTMUSIC_API_KEY[0]) {
		static bool told;
		if (!told) {
			told = true;
			fprintf(stderr, "ytmusic: this build carries no InnerTube key; rebuild with "
							"YTMUSIC_API_KEY set (see YOUTUBE_MUSIC_SETUP.md)\n");
		}
		set_error(error, error_size, "ytmusic_api_key_missing");
		return false;
	}

	pthread_mutex_lock(&api_lock);
	if (api_key[0] && client_version[0]) {
		pthread_mutex_unlock(&api_lock);
		return true;
	}

	snprintf(api_key, sizeof(api_key), "%s", YTMUSIC_API_KEY);
	time_t now = time(NULL);
	struct tm utc;
	if (gmtime_r(&now, &utc)) {
		snprintf(client_version, sizeof(client_version), "1.%04d%02d%02d.01.00", utc.tm_year + 1900,
				 utc.tm_mon + 1, utc.tm_mday);
	} else {
		snprintf(client_version, sizeof(client_version), "%s", YTMUSIC_CLIENT_VERSION_FALLBACK);
	}
	pthread_mutex_unlock(&api_lock);
	(void)error;
	(void)error_size;
	return true;
}

// OAuth access tokens are short-lived. The refresh token is stored in the
// private sidecar beside device_config.ini, and renewed before it reaches the
// player API. The request lock is held by youtubei_post() around this call.
static bool refresh_access_token(char *error, size_t error_size) {
	char current_refresh[sizeof(refresh_token)], client_id[sizeof(oauth_client_id)];
	char client_secret[sizeof(oauth_client_secret)];
	pthread_mutex_lock(&session_lock);
	snprintf(current_refresh, sizeof(current_refresh), "%s", refresh_token);
	pthread_mutex_unlock(&session_lock);
	snprintf(client_id, sizeof(client_id), "%s", oauth_client_id);
	snprintf(client_secret, sizeof(client_secret), "%s", oauth_client_secret);
	if (!current_refresh[0] || !client_id[0]) {
		set_error(error, error_size, "ytmusic_oauth_keys_missing");
		return false;
	}
	char enc_id[768], enc_secret[768], enc_refresh[3072], form[5000];
	http_url_encode(client_id, enc_id, sizeof(enc_id));
	http_url_encode(client_secret, enc_secret, sizeof(enc_secret));
	http_url_encode(current_refresh, enc_refresh, sizeof(enc_refresh));
	int n = snprintf(form, sizeof(form), "client_id=%s&refresh_token=%s&grant_type=refresh_token", enc_id, enc_refresh);
	if (client_secret[0] && n > 0 && (size_t)n < sizeof(form)) {
		size_t used = (size_t)n;
		snprintf(form + used, sizeof(form) - used, "&client_secret=%s", enc_secret);
	}
	char *body = NULL;
	int status = 0;
	bool posted = oauth_post_locked(YTMUSIC_OAUTH_TOKEN_URL, form, &body, &status, error, error_size);
	memset(form, 0, sizeof(form));
	memset(current_refresh, 0, sizeof(current_refresh));
	memset(client_secret, 0, sizeof(client_secret));
	if (!posted) return false;
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		wipe_free(body);
		set_error(error, error_size, "ytmusic_oauth_unreadable_reply");
		return false;
	}
	int root = json_root(&doc);
	char renewed[sizeof(access_token)] = "", rotated[sizeof(refresh_token)] = "";
	json_obj_str(&doc, root, "access_token", renewed, sizeof(renewed));
	json_obj_str(&doc, root, "refresh_token", rotated, sizeof(rotated));
	long expires = json_obj_long(&doc, root, "expires_in", 3600);
	json_free(&doc);
	wipe_free(body);
	if (status < 200 || status >= 300 || !renewed[0]) {
		set_error(error, error_size, status == 400 ? "ytmusic_session_expired" : "ytmusic_oauth_refresh_failed");
		memset(renewed, 0, sizeof(renewed));
		return false;
	}
	pthread_mutex_lock(&session_lock);
	char old_access[sizeof(access_token)], old_refresh[sizeof(refresh_token)];
	time_t old_expiry = access_expires_at;
	snprintf(old_access, sizeof(old_access), "%s", access_token);
	snprintf(old_refresh, sizeof(old_refresh), "%s", refresh_token);
	snprintf(access_token, sizeof(access_token), "%s", renewed);
	if (rotated[0]) snprintf(refresh_token, sizeof(refresh_token), "%s", rotated);
	access_expires_at = time(NULL) + (expires > 60 ? expires - 60 : expires);
	bool saved = save_session();
	if (!saved) {
		snprintf(access_token, sizeof(access_token), "%s", old_access);
		snprintf(refresh_token, sizeof(refresh_token), "%s", old_refresh);
		access_expires_at = old_expiry;
	}
	pthread_mutex_unlock(&session_lock);
	memset(old_access, 0, sizeof(old_access));
	memset(old_refresh, 0, sizeof(old_refresh));
	memset(renewed, 0, sizeof(renewed));
	memset(rotated, 0, sizeof(rotated));
	if (!saved) {
		set_error(error, error_size, "ytmusic_session_save_failed");
		return false;
	}
	return true;
}

// True when the account is the imported browser session, which is the only
// credential the music endpoints accept for the library pages.
static bool session_cookie_attached(void) {
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool has = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	return has;
}

// True when the player holds something it can send as an account: the
// imported browser session or the OAuth refresh token.
static bool account_attached(void) {
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	pthread_mutex_lock(&session_lock);
	bool has = cookie[0] != '\0' || refresh_token[0] != '\0';
	pthread_mutex_unlock(&session_lock);
	memset(cookie, 0, sizeof(cookie));
	return has;
}

// The credential lines for one request.
//
// The imported browser session comes first, because it is the only thing
// InnerTube accepts as an account. Google's OAuth device token is good for the
// Data API and refused by the web music endpoints: a signed-in search comes
// back as a 400, or as a page with no rows in it at all, which is why search
// used to work only while signed out. The token is kept as a second choice, so
// a player set up before the session import still tries something.
//
// `allow_account` false leaves the request anonymous, which is what the retry
// in youtubei_post() uses to keep the public pages working.
static bool auth_headers(char *out, size_t size, bool required, bool allow_account,
						 char *error, size_t error_size) {
	char cookie[YTMUSIC_COOKIE_MAX];
	char token[sizeof(access_token)];
	if (!allow_account) {
		if (required) {
			set_error(error, error_size, "ytmusic_sign_in_required");
			return false;
		}
		if (out && size) out[0] = '\0';
		return true;
	}
	ytmusic_session_cookie(cookie, sizeof(cookie));
	pthread_mutex_lock(&session_lock);
	bool oauth = cookie[0] == '\0' && refresh_token[0] != '\0';
	bool refresh_needed = oauth && (!access_token[0] || time(NULL) + 30 >= access_expires_at);
	snprintf(token, sizeof(token), "%s", access_token);
	pthread_mutex_unlock(&session_lock);
	if (oauth) {
		memset(cookie, 0, sizeof(cookie));
		if (refresh_needed) {
			memset(token, 0, sizeof(token));
			if (!refresh_access_token(error, error_size)) return false;
			pthread_mutex_lock(&session_lock);
			snprintf(token, sizeof(token), "%s", access_token);
			pthread_mutex_unlock(&session_lock);
		}
		// X-Goog-Request-Time is not optional on a Bearer request: without it
		// Google answers a 400 and the player showed "request refused" for
		// every search once an account was attached. ytmusicapi sends the same
		// header, in the same seconds-since-epoch form.
		int n = snprintf(out, size, "Authorization: Bearer %s\r\nX-Goog-Request-Time: %ld\r\nOrigin: %s\r\nReferer: %s/\r\n",
						 token, (long)time(NULL), YTMUSIC_SITE, YTMUSIC_SITE);
		memset(token, 0, sizeof(token));
		return n > 0 && (size_t)n < size;
	}
	memset(token, 0, sizeof(token));
	if (!cookie[0]) {
		if (required) set_error(error, error_size, "ytmusic_sign_in_required");
		memset(cookie, 0, sizeof(cookie));
		return !required;
	}
	const char *names[] = {"SAPISID", "__Secure-3PAPISID", "__Secure-1PAPISID"};
	char sapisid[512] = "";
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char name[64];
		snprintf(name, sizeof(name), "%s=", names[i]);
		const char *p = strstr(cookie, name);
		if (!p || (p != cookie && p[-1] != ';' && !isspace((unsigned char)p[-1]))) continue;
		p += strlen(name);
		const char *end = strpbrk(p, "; \t");
		if (!end) end = p + strlen(p);
		if (end > p && (size_t)(end - p) < sizeof(sapisid)) {
			memcpy(sapisid, p, (size_t)(end - p));
			sapisid[end - p] = '\0';
			break;
		}
	}
	if (!sapisid[0]) {
		memset(cookie, 0, sizeof(cookie));
		set_error(error, error_size, "ytmusic_invalid_session");
		return false;
	}
	char input[1024], hash[SHA1_HEX_LEN];
	long stamp = (long)time(NULL);
	snprintf(input, sizeof(input), "%ld %s %s", stamp, sapisid, YTMUSIC_SITE);
	sha1_hex(input, strlen(input), hash);
	int n = snprintf(out, size,
		"Cookie: %s\r\nAuthorization: SAPISIDHASH %ld_%s\r\nOrigin: %s\r\nReferer: %s/\r\n",
		cookie, stamp, hash, YTMUSIC_SITE, YTMUSIC_SITE);
	memset(cookie, 0, sizeof(cookie));
	memset(sapisid, 0, sizeof(sapisid));
	memset(input, 0, sizeof(input));
	return n > 0 && (size_t)n < size;
}

static bool json_escape(const char *in, char *out, size_t size) {
	if (!in || !out || size < 3) return false;
	size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		const char *escape = NULL;
		char simple[2] = {(char)*p, '\0'};
		switch (*p) {
		case '"': escape = "\\\""; break;
		case '\\': escape = "\\\\"; break;
		case '\b': escape = "\\b"; break;
		case '\f': escape = "\\f"; break;
		case '\n': escape = "\\n"; break;
		case '\r': escape = "\\r"; break;
		case '\t': escape = "\\t"; break;
		default: if (*p < 0x20) return false;
		}
		const char *s = escape ? escape : simple;
		size_t n = strlen(s);
		if (n >= size - used) return false;
		memcpy(out + used, s, n);
		used += n;
	}
	out[used] = '\0';
	return true;
}

// Some Google endpoints protect JSON replies with the standard anti-XSSI
// prefix. The YouTube Music API usually omits it, but tolerate it here so a
// harmless transport wrapper does not turn a valid search reply into a parse
// error.
static const char *youtubei_json_start(const char *body) {
	if (!body) return NULL;
	while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
	if (strncmp(body, ")]}'", 4) == 0) {
		body += 4;
		if (*body == ',') body++;
		while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
	}
	return body;
}

// Whether a JSON body ended like one: the last non-space byte of any complete
// JSON document is '}' or ']'. The InnerTube request goes out with
// "Connection: close", and a reply delimited by the closing connection cannot
// be told from one cut off by a dropping Wi-Fi link -- several hundred
// kilobytes arrive, the last kilobyte does not, and the page parses into
// nothing at all. The body itself is the only witness.
static bool response_is_complete(const char *body) {
	if (!body) {
		return false;
	}
	size_t n = strlen(body);
	while (n > 0 && (body[n - 1] == ' ' || body[n - 1] == '\t' || body[n - 1] == '\r' || body[n - 1] == '\n')) {
		n--;
	}
	return n > 0 && (body[n - 1] == '}' || body[n - 1] == ']');
}

// One InnerTube POST. `force_anonymous` leaves the account out of the request
// altogether: a caller uses it to ask the same public question the signed-out
// player asks, after the account's own answer came back with no rows in it.
static bool youtubei_post_ex(const char *endpoint, const char *body, bool login_required,
							 bool force_anonymous, char **response, int *status,
							 char *error, size_t error_size) {
	pthread_mutex_lock(&request_lock);
	if (!ensure_context(error, error_size)) {
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	char key[sizeof(api_key)], version[sizeof(client_version)];
	pthread_mutex_lock(&api_lock);
	snprintf(key, sizeof(key), "%s", api_key);
	snprintf(version, sizeof(version), "%s", client_version);
	pthread_mutex_unlock(&api_lock);
	char url[512], headers[YTMUSIC_COOKIE_MAX + 1024];
	snprintf(url, sizeof(url), "%s/youtubei/v1/%s?key=%s&prettyPrint=false", YTMUSIC_SITE, endpoint, key);
	snprintf(headers, sizeof(headers), "Content-Type: application/json\r\nOrigin: %s\r\nReferer: %s/\r\nX-YouTube-Client-Name: 67\r\nX-YouTube-Client-Version: %s\r\n", YTMUSIC_SITE, YTMUSIC_SITE, version);
	char request_body[4096];
	const char *stale = "1.20250101.00.00";
	const char *at = strstr(body, stale);
	if (at) {
		size_t before = (size_t)(at - body);
		size_t stale_len = strlen(stale), version_len = strlen(version), tail = strlen(at + stale_len);
		if (before + version_len + tail + 1 > sizeof(request_body)) {
			set_error(error, error_size, "ytmusic_request_too_large");
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		memcpy(request_body, body, before);
		memcpy(request_body + before, version, version_len);
		memcpy(request_body + before + version_len, at + stale_len, tail + 1);
	} else {
		if (strlen(body) >= sizeof(request_body)) {
			set_error(error, error_size, "ytmusic_request_too_large");
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		snprintf(request_body, sizeof(request_body), "%s", body);
	}
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = request_body,
		.content_type = "application/json", .want_error_body = true, .no_https_downgrade = true};
	int status_code = 0;
	bool ok = false;
	bool retried_oauth = false;
	bool retried_truncated = false;
	bool anonymous = false;
	// At most four passes: the account, a forced token refresh, and the
	// anonymous retry, which is the one that matters. A page that does not need
	// an account is never allowed to fail just because the account's credential
	// was refused -- that is what made search come back empty, and Top Songs
	// report a refusal, for anyone who had signed in.
	for (int attempt = 0; attempt < 4; attempt++) {
		size_t used = strlen(headers);
		bool want_account = !anonymous && !force_anonymous;
		if (!auth_headers(headers + used, sizeof(headers) - used, login_required && want_account,
						  want_account, error, error_size)) {
			memset(headers, 0, sizeof(headers));
			memset(request_body, 0, sizeof(request_body));
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		if (!want_account) {
			// The consent wall: from a European address Google answers the
			// public pages with framework JSON that carries no shelves at all
			// -- hundreds of kilobytes in, zero rows out, which is what an
			// empty search page was. These are the cookies the interstitial
			// sets on acceptance; where no wall stands they are ignored.
			static const char consent[] = "Cookie: SOCS=CAI; CONSENT=YES+cb\r\n";
			size_t room = sizeof(headers) - strlen(headers) - 1;
			strncat(headers, consent, room);
		}
		ok = http_request(url, &req, response, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_TIMEOUT, &status_code);
		if (!ok) break;
		if (status_code >= 200 && status_code < 300) {
			// The socket cannot tell a finished close-delimited reply from one
			// cut off halfway, so the body is asked instead. An incomplete one
			// is fetched once more before it is given up on: the pages are
			// read-only and idempotent, and half an Explore page is worth less
			// than none, because it parses into an empty screen with no error.
			if (!response_is_complete(*response)) {
				if (response && *response) {
					wipe_free(*response);
					*response = NULL;
				}
				if (!retried_truncated) {
					retried_truncated = true;
					memset(headers + used, 0, sizeof(headers) - used);
					continue;
				}
				ok = false;
				set_error(error, error_size, "ytmusic_connection_cut");
				break;
			}
			break;
		}

		// Not a 2xx. The reply body is of no use to the caller, and the next
		// attempt would otherwise leak it.
		if (response && *response) {
			wipe_free(*response);
			*response = NULL;
		}

		// The device clock can be corrected after boot, so a locally unexpired
		// token may already be rejected by Google. Force one refresh and retry
		// this read-only request before giving up on the saved session.
		if (status_code == 401 && !retried_oauth && !anonymous && !force_anonymous) {
			pthread_mutex_lock(&session_lock);
			bool can_refresh = refresh_token[0] != '\0';
			if (can_refresh) access_expires_at = 0;
			pthread_mutex_unlock(&session_lock);
			if (can_refresh) {
				retried_oauth = true;
				memset(headers + used, 0, sizeof(headers) - used);
				continue;
			}
		}

		// Google refused the account (a 400, or a 401 the refresh could not
		// help). Try the same read without it: the catalogue pages are public,
		// and a refusal is not a reason to show the user nothing.
		if (!anonymous && !login_required && !force_anonymous && account_attached()) {
			anonymous = true;
			memset(headers + used, 0, sizeof(headers) - used);
			continue;
		}
		break;
	}
	memset(headers, 0, sizeof(headers));
	memset(request_body, 0, sizeof(request_body));
	if (!ok) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_no_reply");
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	if (status) *status = status_code;
	if (status_code < 200 || status_code >= 300) {
		set_error(error, error_size, status_code == 401 || status_code == 403 ? "ytmusic_session_expired" : "ytmusic_request_refused");
		free(*response);
		*response = NULL;
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	pthread_mutex_unlock(&request_lock);
	return true;
}

static bool youtubei_post(const char *endpoint, const char *body, bool login_required,
						  char **response, int *status, char *error, size_t error_size) {
	return youtubei_post_ex(endpoint, body, login_required, false, response, status, error, error_size);
}

// The phone app's player endpoint. It is what resolves the media bytes: the
// web music client answers with a ciphered stream the device cannot solve (it
// has no JavaScript engine), and it wants a signed-in session even to try. This
// one answers with plain https URLs and needs no account, so a track plays
// whether or not anybody has signed in.
//
// ANDROID_VR used to be the direct-URL client of choice; Google has since put
// it behind a "sign in to confirm you're not a bot" check that answers no
// streaming data at all, which is why playback stopped working.
#define YTMUSIC_ANDROID_VERSION "20.10.38"
#define YTMUSIC_ANDROID_SDK 30

// ---------------------------------------------------------------------------
// the market the catalogue answers in
// ---------------------------------------------------------------------------

// Two letters, or the fallback. Google takes these as ISO codes and answers a
// malformed one with a page for somewhere else, so nothing but letters is let
// through.
static void two_letters(const char *in, const char *fallback, bool upper, char out[3]) {
	char a = in && in[0] ? in[0] : fallback[0];
	char b = in && in[0] && in[1] ? in[1] : fallback[1];
	if (!isalpha((unsigned char)a) || !isalpha((unsigned char)b)) {
		a = fallback[0];
		b = fallback[1];
	}
	out[0] = upper ? (char)toupper((unsigned char)a) : (char)tolower((unsigned char)a);
	out[1] = upper ? (char)toupper((unsigned char)b) : (char)tolower((unsigned char)b);
	out[2] = '\0';
}

// YouTube Music chooses what to recommend -- Explore, Top songs, the charts --
// from `gl`, the country's store, and `hl`, the language its labels come back
// in. They are independent on purpose: Polish charts with English row names is
// a real setup, and it is what somebody who lives in Poland and reads English
// wants. Both are set on the settings page and both are only ever two letters.
static void catalogue_bounds(char *gl, size_t gl_size, char *hl, size_t hl_size) {
	char market[3], language[3];
	two_letters(config_get("ytmusic", "region", ""), "US", true, market);
	two_letters(config_get("ytmusic", "language", ""), "en", false, language);
	snprintf(gl, gl_size, "%s", market);
	snprintf(hl, hl_size, "%s", language);
}

// Whether the best format offered is wanted, or the safe one. 140 is the
// 128 kbit/s AAC every instance carries; 141 is the same codec at 256, which
// sounds markedly better on anything but earbuds and costs twice the bytes over
// a public instance. High is the default and Standard is the way back when a
// download over a busy instance is too slow to start.
static bool high_quality_wanted(void) {
	const char *choice = config_get("ytmusic", "quality", "high");
	return !choice || strcmp(choice, "standard") != 0;
}

bool ytmusic_quality_high(void) { return high_quality_wanted(); }

void ytmusic_quality_set(bool high) {
	config_set("ytmusic", "quality", high ? "high" : "standard");
	config_save();
}

// The pairs the region row offers. The labels are tags, translated where the
// row is drawn: a country name is the one string a language file is for.
// "Worldwide" is the US store, which is what a request with no preference gets
// and what most of the catalogue is indexed against.
static const struct {
	const char *region;
	const char *language;
	const char *label;
} REGIONS[] = {
	{"US", "en", "ytmusic_region_worldwide"},
	{"PL", "en", "ytmusic_region_poland_english"},
	{"PL", "pl", "ytmusic_region_poland"},
	{"CZ", "cs", "ytmusic_region_czechia"},
	{"DE", "de", "ytmusic_region_germany"},
	{"GB", "en", "ytmusic_region_uk"},
};

int ytmusic_regions(ytmusic_region_t *out, int limit) {
	if (!out || limit <= 0) return 0;
	int count = 0;
	for (size_t i = 0; i < sizeof(REGIONS) / sizeof(REGIONS[0]) && count < limit; i++) {
		snprintf(out[count].region, sizeof(out[count].region), "%s", REGIONS[i].region);
		snprintf(out[count].language, sizeof(out[count].language), "%s", REGIONS[i].language);
		snprintf(out[count].label, sizeof(out[count].label), "%s", REGIONS[i].label);
		count++;
	}
	return count;
}

void ytmusic_region_active(char *region, size_t region_size, char *language, size_t language_size) {
	char market[3], lang[3];
	two_letters(config_get("ytmusic", "region", ""), "US", true, market);
	two_letters(config_get("ytmusic", "language", ""), "en", false, lang);
	if (region && region_size) snprintf(region, region_size, "%s", market);
	if (language && language_size) snprintf(language, language_size, "%s", lang);
}

bool ytmusic_region_select(const char *region, const char *language) {
	if (!region || !language) return false;
	if (!isalpha((unsigned char)region[0]) || !isalpha((unsigned char)region[1]) || region[2]) return false;
	if (!isalpha((unsigned char)language[0]) || !isalpha((unsigned char)language[1]) || language[2]) return false;
	char market[3], lang[3];
	two_letters(region, "US", true, market);
	two_letters(language, "en", false, lang);
	config_set("ytmusic", "region", market);
	config_set("ytmusic", "language", lang);
	config_save();
	return true;
}

static bool android_player(const char *video_id, char **response, char *error, size_t error_size) {
	if (!ensure_context(error, error_size)) return false;
	char key[sizeof(api_key)];
	pthread_mutex_lock(&api_lock);
	snprintf(key, sizeof(key), "%s", api_key);
	pthread_mutex_unlock(&api_lock);
	char url[512], body[1024], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(url, sizeof(url), "https://www.youtube.com/youtubei/v1/player?key=%s&prettyPrint=false", key);
	snprintf(body, sizeof(body),
		"{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"%s\","
		"\"androidSdkVersion\":%d,\"osName\":\"Android\",\"osVersion\":\"11\","
		"\"hl\":\"%s\",\"gl\":\"%s\"}},\"videoId\":\"%s\","
		"\"contentCheckOk\":true,\"racyCheckOk\":true}",
		YTMUSIC_ANDROID_VERSION, YTMUSIC_ANDROID_SDK, hl, gl, video_id);
	const char *headers = "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/\r\n"
						  "X-YouTube-Client-Name: 3\r\nX-YouTube-Client-Version: " YTMUSIC_ANDROID_VERSION "\r\n";
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = body,
		.content_type = "application/json", .want_error_body = true, .no_https_downgrade = true};
	int status = 0;
	pthread_mutex_lock(&request_lock);
	bool ok = http_request(url, &req, response, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_TIMEOUT, &status);
	pthread_mutex_unlock(&request_lock);
	if (!ok) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_no_reply");
		return false;
	}
	if (status < 200 || status >= 300) {
		free(*response);
		*response = NULL;
		set_error(error, error_size, "ytmusic_stream_refused");
		return false;
	}
	return true;
}

// The relay: a Piped-API-compatible server (GET /streams/{videoId}).
//
// Google now demands a browser-grade attestation -- a PO token its JavaScript
// computes -- from every first-party client before it hands out media URLs.
// The phone-app client answers "precondition check failed", the web music
// player "the page needs to be reloaded", the VR client "sign in to confirm
// you're not a bot", all without giving one byte of streaming data. This
// device has no JavaScript engine and no way to run that attestation, so a
// direct guest stream from YouTube is no longer possible from here at all.
//
// A relay is a small self-hostable service (Piped, Invidious, or anything
// that answers the same two endpoints) that runs the attestation on its own
// connection and passes plain audio URLs back. It is a URL in the settings,
// not a built-in host: public instances appear and die weekly, and pointing
// every shipped player at one would break it the day it does. Search and
// browsing need none of this -- the web music client still answers those
// signed out, which is why only playback asks for a relay.
//
// Only the audio streams are read, and only the AAC-in-MP4 ones: the player
// decodes AAC in an MP4 container, its Opus decoder reads Ogg and not the
// WebM YouTube wraps Opus in, and the video streams are never fetched at all.
#define YTMUSIC_RELAY_MAX_KBPS 320

// A relay that resolves with yt-dlp has to run it before it can answer, and a
// cold run fetches the player script and solves a signature first: fifteen
// seconds is enough for a request to Google and not for that. The relay calls
// get their own, longer one; everything else keeps the short one.
#define YTMUSIC_RELAY_TIMEOUT 60

// The relay's own address, without the trailing slashes the setting may carry.
// False when nothing usable is configured -- no setting, or not an address.
static bool relay_base_address(char *out, size_t size) {
	const char *configured = config_get("ytmusic", "relay_base", "");
	if (!configured[0]) {
		return false;
	}
	snprintf(out, size, "%s", configured);
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
	// http:// is accepted as well as https://: a relay is often a machine on
	// the same LAN, where there is no certificate to serve.
	if (!len || (strncmp(out, "https://", 8) != 0 && strncmp(out, "http://", 7) != 0)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// Where the player fetches a track's bytes from.
//
// A relay answers with a media URL, and the ones a Piped or Invidious instance
// answers with point at Google's own servers. Those cannot be fetched from here
// at all: the address is bound to the machine that resolved it -- the same
// request from a different client on a different address is answered 403 -- and
// it is several thousand characters long, past this player's URL buffer, so
// what would be requested is a truncated address that is refused without saying
// why. That is what "could not reach the stream" used to mean, and it left no
// trace anywhere.
//
// So the bytes are always fetched from the relay itself: from the URL it
// answered with when that URL is its own address, and from /audio/<video-id>
// -- the endpoint tools/ytmusic-relay.py serves -- when it is somebody else's.
// A relay that does neither is reported as such instead of having its address
// quietly fetched and refused.
//
// `itag` is the format the reply was read for, carried into the proxied address
// so the relay serves that one rather than choosing again -- the choice of
// m4a/AAC under the bitrate cap is this end's, and it should stay that way.
//
// `proxied` (optional) says the second path was taken: the one that needs the
// relay to serve the bytes itself.
bool ytmusic_relay_stream_url(const char *base, const char *video_id, long itag, const char *reported,
							  char *out, size_t out_size, bool *proxied) {
	if (proxied) {
		*proxied = false;
	}
	if (!base || !base[0] || !video_id || strlen(video_id) != 11 || !out || out_size == 0) {
		return false;
	}
	size_t base_len = strlen(base);
	if (reported && reported[0] && strncmp(reported, base, base_len) == 0 &&
		(reported[base_len] == '\0' || reported[base_len] == '/' || reported[base_len] == '?')) {
		snprintf(out, out_size, "%s", reported);
		return true;
	}
	int built = itag > 0 ? snprintf(out, out_size, "%s/audio/%.11s?f=%ld", base, video_id, itag)
						 : snprintf(out, out_size, "%s/audio/%.11s", base, video_id);
	if (built < 0 || built >= (int)out_size) {
		out[0] = '\0';
		return false;
	}
	if (proxied) {
		*proxied = true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------
//
// A Piped or Invidious instance resolves a track on a machine that can do the
// attestation this device cannot. What it hands back decides whether that is
// any use here:
//
//   * Piped answers /streams/{id} with audioStreams whose URLs point at
//     Google's own hosts. Those are bound to the address that resolved them,
//     so fetching one from here is refused; a Piped instance only helps when
//     it fronts the bytes itself, and then its URL already points at it.
//   * Invidious answers /api/v1/videos/{id}, and has /latest_version?id=...
//     &itag=...&local=true for the bytes: the instance fetches the audio and
//     passes it on, which is the one shape that works from another machine.
//     That is what is asked for once the formats reply names an itag.
//
// Public instances come and go, and the survivors are counted in single
// digits: when this list was last measured against a real track, one public
// host answered /api/v1/videos and then served actual audio bytes through
// /latest_version?local=true, while every Piped host on the published list
// answered 502, 403 or nothing at all. So the list starts with the host that
// was measured, and the settings page can pin another (a self-hosted instance
// is the version of this that keeps working).
static const char *const builtin_instances[] = {
	"https://yt.chocolatemoo53.com",
	"https://inv.nadeko.net",
	"https://invidious.nerdvpn.de",
	"https://invidious.tiekoetter.com",
	// Piped hosts answer this player's API shape when they are up; every one
	// named on the official list was down when the hosts above were measured,
	// so they sit after the hosts that were not.
	"https://pipedapi.kavin.rocks",
	"https://pipedapi.adminforge.de",
};

// One instance request, not a page: the ones that are down fail on the
// connection, and the ones that answer do it quickly. Waiting the relay's
// minute on each would turn a failed track into a two-minute stall.
#define YTMUSIC_INSTANCE_TIMEOUT 12
// How many of the built-in hosts are tried before the player gives up: they
// are guesses, and a play attempt should not wait on six of them.
#define YTMUSIC_INSTANCE_TRIES 4
// Room for the [ytmusic] instances line, which lists hosts by hand.
#define YTMUSIC_INSTANCE_LIST_MAX 512

// A base address as the player needs it: scheme kept, trailing slashes gone.
// False when it is not an http(s) address at all, which is what a bare host
// name typed into the settings is.
static bool normalise_instance(const char *in, char *out, size_t size) {
	if (!in || !out || size < 16) return false;
	while (*in == ' ' || *in == '\t') in++;
	if (!in[0]) return false;
	// A bare host is what people type -- "invidious.example" -- and https is
	// what every instance serves, so it is filled in rather than refused.
	bool scheme = strncmp(in, "https://", 8) == 0 || strncmp(in, "http://", 7) == 0;
	int n = scheme ? snprintf(out, size, "%s", in) : snprintf(out, size, "https://%s", in);
	if (n <= 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	size_t len = strlen(out);
	while (len > 8 && out[len - 1] == '/') out[--len] = '\0';
	// A host name is all that may follow the scheme: no path, no space.
	const char *host = out + (strncmp(out, "https://", 8) == 0 ? 8 : 7);
	return host[0] != '\0' && strpbrk(host, " \t/?#") == NULL;
}

// The instance a choice pins, or empty for the three built-in answers.
static bool pinned_instance(char *out, size_t size) {
	const char *active = config_get("ytmusic", "source", "auto");
	if (!active[0] || strcmp(active, "auto") == 0 || strcmp(active, "direct") == 0 ||
		strcmp(active, "relay") == 0) {
		return false;
	}
	return normalise_instance(active, out, size);
}

static bool instance_add(char out[][YTMUSIC_SOURCE_MAX], int *count, int limit, const char *base) {
	char normalised[YTMUSIC_SOURCE_MAX];
	if (*count >= limit || !normalise_instance(base, normalised, sizeof(normalised))) return false;
	for (int i = 0; i < *count; i++) {
		if (strcmp(out[i], normalised) == 0) return false;
	}
	snprintf(out[(*count)++], YTMUSIC_SOURCE_MAX, "%s", normalised);
	return true;
}

int ytmusic_instance_bases(char out[][YTMUSIC_SOURCE_MAX], int limit) {
	int count = 0;
	char pinned[YTMUSIC_SOURCE_MAX];
	if (pinned_instance(pinned, sizeof(pinned))) instance_add(out, &count, limit, pinned);
	// [ytmusic] instances is a comma or space separated list, and replaces the
	// built-in hosts rather than adding to them: someone who has a working
	// instance does not want six dead ones tried before it.
	const char *configured = config_get("ytmusic", "instances", "");
	if (configured[0]) {
		char list[YTMUSIC_INSTANCE_LIST_MAX];
		snprintf(list, sizeof(list), "%s", configured);
		for (char *token = strtok(list, " ,;\t\r\n"); token; token = strtok(NULL, " ,;\t\r\n")) {
			instance_add(out, &count, limit, token);
		}
		return count;
	}
	for (size_t i = 0; i < sizeof(builtin_instances) / sizeof(builtin_instances[0]); i++) {
		instance_add(out, &count, limit, builtin_instances[i]);
	}
	return count;
}

// The stored choice, "auto" when nothing was ever chosen.
static const char *source_value(void) {
	const char *active = config_get("ytmusic", "source", "auto");
	return active[0] ? active : "auto";
}

int ytmusic_sources(ytmusic_source_t *out, int limit) {
	if (!out || limit <= 0) return 0;
	int count = 0;
	static const struct {
		const char *value;
		const char *label;
	} built_in[] = {
		{"auto", "ytmusic_source_auto"},
		{"direct", "ytmusic_source_direct"},
		{"relay", "ytmusic_source_relay"},
	};
	for (size_t i = 0; i < sizeof(built_in) / sizeof(built_in[0]) && count < limit; i++) {
		// The relay is only a choice once there is one configured: a row that
		// cannot play anything is worse than no row.
		if (strcmp(built_in[i].value, "relay") == 0) {
			char base[256];
			bool pinned = strcmp(source_value(), "relay") == 0;
			if (!relay_base_address(base, sizeof(base)) && !pinned) continue;
			snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%s", built_in[i].value);
			snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%s", built_in[i].label);
			count++;
			continue;
		}
		snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%s", built_in[i].value);
		snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%s", built_in[i].label);
		count++;
	}
	char bases[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int instances = ytmusic_instance_bases(bases, YTMUSIC_SOURCES_MAX);
	for (int i = 0; i < instances && count < limit; i++) {
		const char *host = strstr(bases[i], "://");
		snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1, bases[i]);
		snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1,
				 host ? host + 3 : bases[i]);
		count++;
	}
	return count;
}

void ytmusic_source_active(char *out, size_t size) {
	if (!out || !size) return;
	snprintf(out, size, "%s", source_value());
}

bool ytmusic_source_select(const char *value) {
	if (!value || !value[0]) return false;
	char chosen[YTMUSIC_SOURCE_MAX];
	if (strcmp(value, "auto") == 0 || strcmp(value, "direct") == 0 || strcmp(value, "relay") == 0) {
		snprintf(chosen, sizeof(chosen), "%s", value);
	} else if (!normalise_instance(value, chosen, sizeof(chosen))) {
		snprintf(chosen, sizeof(chosen), "%s", value);
		return false;
	}
	config_set("ytmusic", "source", chosen);
	// The choice is in force either way; a settings file that cannot be
	// written only means it will not survive a reboot, which is not a reason
	// to refuse it now.
	if (!config_save()) {
		fprintf(stderr, "ytmusic: the source could not be saved, kept for this session\n");
	}
	return true;
}

static bool relay_audio_url(const char *video_id, char *url, size_t url_size,
							int *duration, char *title, size_t title_size,
							char *artist, size_t artist_size, bool *proxied,
							char *error, size_t error_size) {
	if (proxied) *proxied = false;
	char base[256];
	if (!relay_base_address(base, sizeof(base))) {
		// No relay is not a missing setup step any more: the device plays a
		// track by itself, and an instance is the other way in. What is left
		// to say is that every one of them refused.
		set_error(error, error_size, "ytmusic_no_source");
		return false;
	}

	char endpoint[320];
	snprintf(endpoint, sizeof(endpoint), "%s/streams/%.11s", base, video_id);
	char *body = NULL;
	int relay_status = 0;
	pthread_mutex_lock(&request_lock);
	// The body of a refusal is kept: it is where the relay says why, and that
	// belongs in the log rather than in the guess of "no reply".
	bool got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT,
						   YTMUSIC_RELAY_TIMEOUT, &relay_status);
	pthread_mutex_unlock(&request_lock);
	if (!got) {
		fprintf(stderr, "ytmusic: relay did not answer at %.64s (%s)\n", endpoint,
				http_last_error() ? http_last_error() : "no reason given");
		set_error(error, error_size, "ytmusic_relay_unreachable");
		return false;
	}
	if (relay_status >= 400) {
		// A relay that does not know /streams/{id} is not the kind of relay
		// this player can use, whatever else it answers: Invidious serves its
		// own API shape, not this one. Its reply says which it is, in the log.
		fprintf(stderr, "ytmusic: relay answered %d for %.11s: %.160s\n", relay_status, video_id,
				body ? body : "");
		free(body);
		set_error(error, error_size,
				  relay_status == 404 || relay_status == 405 ? "ytmusic_relay_not_serving"
															 : "ytmusic_relay_unreadable");
		return false;
	}

	json_doc_t doc;
	bool ok = false;
	if (json_parse_limited(youtubei_json_start(body), &doc, 32000)) {
		int root = json_root(&doc);
		int streams = json_get(&doc, root, "audioStreams");
		int best = -1;
		long best_bitrate = -1;
		for (int i = 0; i < json_len(&doc, streams); i++) {
			int stream = json_at(&doc, streams, i);
			char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
			json_obj_str(&doc, stream, "mimeType", mime, sizeof(mime));
			if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
			if (!json_obj_str(&doc, stream, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
			long bitrate = json_obj_long(&doc, stream, "bitrate", 0);
			if (bitrate <= 0 || bitrate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
			if (bitrate > best_bitrate) {
				best = stream;
				best_bitrate = bitrate;
			}
		}
		if (best >= 0 && json_obj_str(&doc, best, "url", url, url_size) && url[0]) {
			// The address the relay answered with is only worth fetching when it
			// is the relay's own; see ytmusic_relay_stream_url(). This is the
			// one place that rewrite belongs: an address the device resolved
			// itself, through a signed-in session, is fetchable as it stands
			// and is left alone.
			char chosen[YTMUSIC_URL_MAX];
			bool through_relay = false;
			long itag = json_obj_long(&doc, best, "itag", 0);
			if (ytmusic_relay_stream_url(base, video_id, itag, url, chosen, sizeof(chosen),
										&through_relay) &&
				through_relay) {
				snprintf(url, url_size, "%s", chosen);
				printf("ytmusic: %.11s: the relay's own address is elsewhere, so its /audio endpoint is used\n",
					   video_id);
			}
			if (proxied) *proxied = through_relay;
			ok = true;
			if (duration) *duration = (int)json_obj_long(&doc, root, "duration", 0);
			if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
			if (artist && artist_size) json_obj_str(&doc, root, "uploader", artist, artist_size);
		} else {
			set_error(error, error_size, "ytmusic_aac_stream_unavailable");
		}
		json_free(&doc);
	} else {
		set_error(error, error_size, "ytmusic_relay_unreadable");
	}
	free(body);
	if (ok) {
		printf("ytmusic: relay served an audio stream for %.11s\n", video_id);
	}
	return ok;
}

// One instance, asked for the formats of a track and turned into an address
// this player can fetch. Two API shapes are tried, because a Piped host and an
// Invidious host answer different ones and either kind can be pinned:
//
//   Piped     /streams/{id}                 audioStreams[].url
//   Invidious /api/v1/videos/{id}           adaptiveFormats[].itag
//
// The second only turns into bytes through /latest_version?local=true, which
// is why the itag is what is carried out of that reply rather than its URL.
static bool instance_audio_url(const char *base, const char *video_id, char *url, size_t url_size,
							   int *duration, char *title, size_t title_size,
							   char *artist, size_t artist_size, bool *proxied,
							   uint64_t *expected_bytes, char *error, size_t error_size) {
	if (proxied) *proxied = false;
	if (expected_bytes) *expected_bytes = 0;
	if (!base || !base[0] || !video_id || strlen(video_id) != 11) return false;
	size_t base_len = strlen(base);
	char endpoint[YTMUSIC_SOURCE_MAX + 160], *body = NULL;
	int status = 0;
	// Whether the host answered at all, which decides which failure to name:
	// "no playable audio" is only true of a host that replied.
	bool answered = false;

	// Piped first: it is the shape the relay already speaks, and the one a
	// Piped instance answers without a query string of its own.
	snprintf(endpoint, sizeof(endpoint), "%s/streams/%.11s", base, video_id);
	pthread_mutex_lock(&request_lock);
	bool got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT,
						   YTMUSIC_INSTANCE_TIMEOUT, &status);
	pthread_mutex_unlock(&request_lock);
	if (got && body && status < 400) {
		answered = true;
		json_doc_t doc;
		if (json_parse_limited(youtubei_json_start(body), &doc, 128 * 1024)) {
			int root = json_root(&doc);
			int streams = json_get(&doc, root, "audioStreams");
			int best = -1;
			long best_rate = -1;
			for (int i = 0; i < json_len(&doc, streams); i++) {
				int stream = json_at(&doc, streams, i);
				char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
				json_obj_str(&doc, stream, "mimeType", mime, sizeof(mime));
				if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
				if (!json_obj_str(&doc, stream, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
				long rate = json_obj_long(&doc, stream, "bitrate", 0);
				if (rate <= 0 || rate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
				if (rate > best_rate) {
					best = stream;
					best_rate = rate;
				}
			}
			if (best >= 0) {
				char candidate[YTMUSIC_URL_MAX] = "";
				if (json_obj_str(&doc, best, "url", candidate, sizeof(candidate)) && candidate[0] &&
					strncmp(candidate, base, base_len) == 0 &&
					(candidate[base_len] == '/' || candidate[base_len] == '?' ||
					 candidate[base_len] == '\0')) {
					// The instance fronts the bytes itself, which is the only kind
					// of address from somebody else this player can fetch.
					snprintf(url, url_size, "%s", candidate);
					if (duration) *duration = (int)json_obj_long(&doc, root, "duration", 0);
					if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
					if (artist && artist_size) json_obj_str(&doc, root, "uploader", artist, artist_size);
					json_free(&doc);
					free(body);
					return true;
				}
			}
			json_free(&doc);
		}
	}
	free(body);
	body = NULL;

	// Invidious: the formats reply names the itag, and /latest_version with
	// local=true is the endpoint that fetches and passes the audio on.
	snprintf(endpoint, sizeof(endpoint), "%s/api/v1/videos/%.11s?fields=title,author,lengthSeconds,adaptiveFormats",
			 base, video_id);
	status = 0;
	pthread_mutex_lock(&request_lock);
	got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_INSTANCE_TIMEOUT,
					  &status);
	pthread_mutex_unlock(&request_lock);
	bool ok = false;
	if (got && body && status < 400) {
		answered = true;
		json_doc_t doc;
		if (json_parse_limited(youtubei_json_start(body), &doc, YTMUSIC_RESPONSE_LIMIT)) {
			int root = json_root(&doc);
			int formats = json_get(&doc, root, "adaptiveFormats");
			int best = -1;
			long best_rate = -1;
			for (int i = 0; i < json_len(&doc, formats); i++) {
				int format = json_at(&doc, formats, i);
				char mime[96] = "";
				json_obj_str(&doc, format, "type", mime, sizeof(mime));
				if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
				long itag = json_obj_long(&doc, format, "itag", 0);
				if (itag <= 0) continue;
				// Past the cap the device is decoding audio it cannot hear
				// anyway. High takes the best the instance offers -- 141 is 256
				// kbit/s AAC where it exists; Standard pins 140, the 128 kbit/s
				// format every instance carries.
				long rate = json_obj_long(&doc, format, "bitrate", 0);
				if (rate <= 0 || rate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
				long score = high_quality_wanted() ? rate : (itag == 140 ? (1L << 30) : rate);
				if (score > best_rate) {
					best = format;
					best_rate = score;
				}
			}
			if (best >= 0) {
				long itag = json_obj_long(&doc, best, "itag", 0);
				int built = snprintf(url, url_size, "%s/latest_version?id=%.11s&itag=%ld&local=true",
									 base, video_id, itag);
				if (built > 0 && (size_t)built < url_size) {
					ok = true;
					if (proxied) *proxied = true;
					// What the whole stream is, from the instance's own reply: the
					// downloader checks the file against it, so a transfer that
					// stopped halfway cannot pass for a finished track.
					if (expected_bytes) {
						*expected_bytes = (uint64_t)json_obj_long(&doc, best, "clen", 0);
					}
					printf("ytmusic: %.11s: %.48s offers itag %ld at %ld kbps\n", video_id, base,
						   itag, json_obj_long(&doc, best, "bitrate", 0) / 1000);
					if (duration) *duration = (int)json_obj_long(&doc, root, "lengthSeconds", 0);
					if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
					if (artist && artist_size) json_obj_str(&doc, root, "author", artist, artist_size);
				}
			}
		}
		json_free(&doc);
	}
	free(body);
	if (!ok && answered) set_error(error, error_size, "ytmusic_instance_no_stream");
	return ok;
}

static bool token_video_id(const json_doc_t *doc, int renderer, char out[YTMUSIC_ID_MAX]) {
	static const char *const paths[] = {
		"playlistItemData.videoId",
		"navigationEndpoint.watchEndpoint.videoId",
		"flexColumns.0.musicResponsiveListItemFlexColumnRenderer.text.runs.0.navigationEndpoint.watchEndpoint.videoId",
		"overlay.musicItemThumbnailOverlayRenderer.content.musicPlayButtonRenderer.playNavigationEndpoint.watchEndpoint.videoId",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (json_str(doc, json_path(doc, renderer, paths[i]), out, YTMUSIC_ID_MAX) && out[0]) return true;
	}
	return false;
}

static int parse_duration(const char *text) {
	int a = 0, b = 0, c = 0;
	if (sscanf(text, "%d:%d:%d", &a, &b, &c) == 3) return a * 3600 + b * 60 + c;
	if (sscanf(text, "%d:%d", &a, &b) == 2) return a * 60 + b;
	return 0;
}

// The page a container row opens: a playlist ("VL..."), an album ("MPRE..."),
// a podcast show ("MPSP...") or an artist channel.
static bool token_browse_id(const json_doc_t *doc, int renderer, char *out, size_t size) {
	static const char *const paths[] = {
		"navigationEndpoint.browseEndpoint.browseId",
		"title.runs.0.navigationEndpoint.browseEndpoint.browseId",
		"subtitle.runs.0.navigationEndpoint.browseEndpoint.browseId",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (json_str(doc, json_path(doc, renderer, paths[i]), out, size) && out[0]) return true;
	}
	return false;
}

// The artwork of one row: the largest size the page offers, which is the last
// entry of the thumbnails array. A song carries it under "thumbnail" and a
// playlist, album or show card under "thumbnailRenderer"; below that the two
// shapes are the same. Empty when the row has no artwork, which is a row that
// keeps its placeholder glyph.
static bool read_thumbnail(const json_doc_t *doc, int renderer, char *out, size_t size) {
	static const char *const paths[] = {
		"thumbnail.musicThumbnailRenderer.thumbnail.thumbnails",
		"thumbnailRenderer.musicThumbnailRenderer.thumbnail.thumbnails",
	};
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int list = json_path(doc, renderer, paths[i]);
		for (int j = json_len(doc, list) - 1; j >= 0; j--) {
			if (json_obj_str(doc, json_at(doc, list, j), "url", out, size) && out[0]) {
				return true;
			}
		}
	}
	return false;
}

// The album of a song row.
//
// The second column is "artist <bullet> album <bullet> duration", one run per
// piece, and the artist itself is several runs when a track lists more than one
// (", " and " & " between the names). So the album is the group between the two
// bullets -- unless that group is the length itself, which is the shape a
// playlist or album track list has. Read from the runs rather than guessed from
// the row's text, so an artist called "4:09" cannot be mistaken for one.
static void read_album(const json_doc_t *doc, int renderer, ytmusic_item_t *item) {
	int runs = json_path(doc, renderer, "flexColumns.1.musicResponsiveListItemFlexColumnRenderer.text.runs");
	char album[YTMUSIC_TITLE_MAX] = "";
	bool past_artist = false;
	for (int i = 0; i < json_len(doc, runs); i++) {
		char text[YTMUSIC_TITLE_MAX] = "";
		if (!json_obj_str(doc, json_at(doc, runs, i), "text", text, sizeof(text)) || !text[0]) continue;
		if (strstr(text, "\xE2\x80\xA2")) { // the bullet: " • "
			if (past_artist) break;            // the album group has ended
			past_artist = true;
			continue;
		}
		if (!past_artist) continue; // the artist, and the "&" / "," between names
		size_t used = strlen(album);
		snprintf(album + used, sizeof(album) - used, "%s", text);
	}
	// A duration where the album should be: the page did not name one.
	size_t len = strlen(album);
	while (len > 0 && (album[len - 1] == ' ' || album[len - 1] == '\t')) album[--len] = '\0';
	char *start = album;
	while (*start == ' ' || *start == '\t') start++;
	if (!start[0] || (isdigit((unsigned char)start[0]) && strchr(start, ':'))) return;
	snprintf(item->album, sizeof(item->album), "%s", start);
}

// One row of any of the three shapes the store uses for a list entry: a song
// row, a podcast episode (a multi-row item), or a card for a playlist, album or
// show. A row with a video id plays; one that only names a page opens it.
static bool read_item(const json_doc_t *doc, int renderer, ytmusic_item_t *item) {
	memset(item, 0, sizeof(*item));
	if (token_video_id(doc, renderer, item->id)) {
		item->kind = YTMUSIC_ITEM_TRACK;
	} else if (token_browse_id(doc, renderer, item->browse_id, sizeof(item->browse_id))) {
		item->kind = YTMUSIC_ITEM_CONTAINER;
	} else {
		return false;
	}
	json_str(doc, json_path(doc, renderer, "flexColumns.0.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), item->title, sizeof(item->title));
	if (!item->title[0]) json_str(doc, json_path(doc, renderer, "title.runs.0.text"), item->title, sizeof(item->title));
	// The second column is the artist, or the show for an episode; the third
	// holds the album, or a play count on an album page. The first one that says
	// something is the subtitle.
	json_str(doc, json_path(doc, renderer, "flexColumns.1.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), item->subtitle, sizeof(item->subtitle));
	if (!item->subtitle[0]) {
		json_str(doc, json_path(doc, renderer, "flexColumns.2.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), item->subtitle, sizeof(item->subtitle));
	}
	if (!item->subtitle[0]) json_str(doc, json_path(doc, renderer, "subtitle.runs.0.text"), item->subtitle, sizeof(item->subtitle));
	if (item->kind == YTMUSIC_ITEM_TRACK) read_album(doc, renderer, item);
	read_thumbnail(doc, renderer, item->thumbnail, sizeof(item->thumbnail));
	char duration[32] = "";
	json_str(doc, json_path(doc, renderer, "fixedColumns.0.musicResponsiveListItemFixedColumnRenderer.text.runs.0.text"), duration, sizeof(duration));
	item->duration = parse_duration(duration);
	return item->title[0] != '\0';
}

static const char *skip_json_space(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	return p;
}

// Return the byte after a JSON string. This small scanner lets search skip the
// large unrelated parts of an InnerTube response without allocating a token
// for every field in it.
static const char *json_string_end(const char *p) {
	if (*p != '"') return NULL;
	for (p++; *p; p++) {
		if (*p == '\\') {
			if (!p[1]) return NULL;
			p++;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return NULL;
}

// Return the byte after one complete object/array, respecting strings and
// escaped quotes. A response may be truncated at the HTTP cap; in that case
// complete track rows before the cut remain usable.
static const char *json_compound_end(const char *start) {
	char closers[JSON_MAX_DEPTH];
	int depth = 0;
	bool in_string = false;
	for (const char *p = start; *p; p++) {
		if (in_string) {
			if (*p == '\\') {
				if (!p[1]) return NULL;
				p++;
			} else if (*p == '"') {
				in_string = false;
			}
			continue;
		}
		if (*p == '"') {
			in_string = true;
		} else if (*p == '{' || *p == '[') {
			if (depth >= JSON_MAX_DEPTH) return NULL;
			closers[depth++] = *p == '{' ? '}' : ']';
		} else if (*p == '}' || *p == ']') {
			if (depth <= 0 || closers[depth - 1] != *p) return NULL;
			if (--depth == 0) return p + 1;
		}
	}
	return NULL;
}

static bool track_renderer_is_ad(const json_doc_t *doc, int renderer) {
	// Ad markers are keys of the renderer itself -- "adSlot", "promoted" and
	// their kin. They are never anything nested inside it, and reading that
	// deep once flagged every real row on the page: the action menu each row
	// carries holds "signInEndpoint":{"hack":true}, and a boolean called
	// "hack" read exactly like the ad this check went looking for. So the
	// scan stays on the renderer's own keys, and only the known names count.
	int end_char = doc->toks[renderer].end;
	for (int i = renderer + 1; i < doc->count && doc->toks[i].start < end_char; i++) {
		if (doc->toks[i].parent != renderer || doc->toks[i].type != JSON_STRING) {
			continue; // a value, or a key of something nested deeper in
		}
		char key[128];
		if (!json_str(doc, i, key, sizeof(key))) continue;
		if (strcasestr(key, "adSlot") || strcasestr(key, "adPlacement") ||
			strcasestr(key, "displayAd") || strcasestr(key, "promoted") ||
			strcasestr(key, "sponsored")) {
			return true;
		}
	}
	return false;
}

// The three renderers a list entry arrives in. A song and a podcast show are
// both "responsive list items", a playlist/album/show card is a "two row item",
// and a podcast episode is a "multi row item".
static bool item_renderer_key(const char *key, size_t len) {
	static const char *const keys[] = {
		"musicResponsiveListItemRenderer",
		"musicTwoRowItemRenderer",
		"musicMultiRowListItemRenderer",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (len == strlen(keys[i]) && memcmp(key, keys[i], len) == 0) return true;
	}
	return false;
}

// Parse each item renderer on its own instead of materializing the entire
// page as one token tree. This uses much less RAM and still returns complete
// early rows if YouTube sends a response larger than our bounded HTTP buffer.
//
// A public page that came back with nothing is the one failure a log can
// diagnose from an armchair: what the body holds says whether Google sent
// results nobody read (the renderers would be there), a consent or bot wall
// ("consent", "Sign in to confirm"), or nothing at all. The head and the tail
// say the rest -- a body cut off mid-transfer ends mid-string, and that tail
// is the fingerprint of a connection that died on the way.
static void ytmusic_log_empty_body(const char *what, const char *body) {
	if (!body) {
		fprintf(stderr, "ytmusic: %s page empty: no body at all\n", what);
		return;
	}
	static const char *const markers[] = {
		"musicResponsiveListItemRenderer",
		"musicTwoRowItemRenderer",
		"musicShelfRenderer",
		"musicCarouselShelfRenderer",
		"consent",
		"Sign in to confirm",
		"playabilityStatus",
	};
	fprintf(stderr, "ytmusic: %s page empty, %zu bytes:", what, strlen(body));
	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
		int hits = 0, as_keys = 0;
		const char *at = body;
		size_t len = strlen(markers[i]);
		while ((at = strstr(at, markers[i])) != NULL) {
			hits++;
			// A key occurrence carries a closing quote and a colon; anything
			// else is the name inside some other string.
			if (at[len] == '"' && at[len + 1] == ':') {
				as_keys++;
			}
			at += len;
		}
		if (hits) {
			fprintf(stderr, " %s=%d(key %d)", markers[i], hits, as_keys);
		}
	}
	// The head names what the page actually is; the tail is where a cut-off
	// transfer shows itself.
	fprintf(stderr, "\nytmusic: %s head: %.200s\n", what, body);
	size_t n = strlen(body);
	fprintf(stderr, "ytmusic: %s tail: %.160s\n", what, n > 160 ? body + n - 160 : body);

	// And the whole body lands beside the player's own files for a pull, if
	// the log is not enough: <card>/.local/ytmusic/ when a card was named
	// (it survives reboots), the tmpfs when it was not. One file per kind,
	// rewritten on every empty answer.
	char folder[640], path[700];
	if (card_root[0]) {
		char parent[620];
		snprintf(parent, sizeof(parent), "%s/.local", card_root);
		mkdir(parent, 0755);
		snprintf(folder, sizeof(folder), "%s/ytmusic", parent);
		mkdir(folder, 0755);
		snprintf(path, sizeof(path), "%s/empty-%s.json", folder, what);
	} else {
		mkdir(playback_cache_dir(), 0700);
		snprintf(folder, sizeof(folder), "%s", playback_cache_dir());
		snprintf(path, sizeof(path), "%s/empty-%s.json", folder, what);
	}
	FILE *f = fopen(path, "wb");
	if (f) {
		size_t dumped = strlen(body);
		if (dumped > 1024 * 1024) {
			dumped = 1024 * 1024;
		}
		fwrite(body, 1, dumped, f);
		fclose(f);
		fprintf(stderr, "ytmusic: %s body kept at %s\n", what, path);
	}
}

static int collect_items_compact(const char *response, ytmusic_item_t *items, int limit) {
	const char *json = youtubei_json_start(response);
	if (!json || (*json != '{' && *json != '[')) return -1;
	int count = 0;
	bool saw_unreadable = false;
	const char *p = json;

	while (*p && count < limit) {
		if (*p != '"') {
			p++;
			continue;
		}
		const char *key_end = json_string_end(p);
		if (!key_end) break;
		bool is_item_key = item_renderer_key(p + 1, (size_t)(key_end - p - 2));
		const char *value = skip_json_space(key_end);
		if (!is_item_key || *value != ':') {
			p = key_end;
			continue;
		}
		value = skip_json_space(value + 1);
		if (*value != '{') {
			saw_unreadable = true;
			p = key_end;
			continue;
		}
		const char *end = json_compound_end(value);
		if (!end) {
			saw_unreadable = true;
			break;
		}
		size_t item_len = (size_t)(end - value);
		if (item_len <= YTMUSIC_RENDERER_SIZE_LIMIT) {
			char *item_json = malloc(item_len + 1);
			if (!item_json) return count ? count : -1;
			memcpy(item_json, value, item_len);
			item_json[item_len] = '\0';
			json_doc_t item_doc;
			if (json_parse_limited(item_json, &item_doc, YTMUSIC_RENDERER_TOKEN_LIMIT)) {
				int root = json_root(&item_doc);
				// An ad, sponsored or promoted card is skipped in silence: it is
				// not a broken row, it is a row this player does not offer.
				if (!track_renderer_is_ad(&item_doc, root)) {
					ytmusic_item_t candidate;
					if (read_item(&item_doc, root, &candidate)) {
						bool duplicate = false;
						for (int i = 0; i < count; i++) {
							const char *mine = items[i].kind == YTMUSIC_ITEM_TRACK ? items[i].id : items[i].browse_id;
							const char *theirs = candidate.kind == YTMUSIC_ITEM_TRACK ? candidate.id : candidate.browse_id;
							if (mine[0] && strcmp(mine, theirs) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) items[count++] = candidate;
					} else {
						saw_unreadable = true;
					}
				}
				json_free(&item_doc);
			} else {
				saw_unreadable = true;
			}
			free(item_json);
		} else {
			saw_unreadable = true;
		}
		p = end;
	}
	if (!count && saw_unreadable) return -1;
	return count;
}

// The heading of one carousel of the Explore page.
//
// The heading lives in the carousel's header, a small object at the front of
// it -- but the carousel itself is the whole shelf, and on a real Explore page
// that is a couple of hundred kilobytes and far more tokens than a bounded
// parse will take. Parsing all of it to read one string used to run past that
// ceiling and answer "no heading" for every row on the page, which drew three
// rows all labelled Explore. So the header object is cut out first and parsed
// on its own, which costs one scan of the carousel's first few hundred bytes.
static void read_section_title(const char *object, char *out, size_t size) {
	static const char *const headers[] = {
		"\"musicCarouselShelfBasicHeaderRenderer\"",
		"\"musicCardShelfHeaderBasicRenderer\"",
	};
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
		const char *key = strstr(object, headers[i]);
		if (!key) continue;
		const char *value = skip_json_space(key + strlen(headers[i]));
		if (*value != ':') continue;
		value = skip_json_space(value + 1);
		if (*value != '{') continue;
		const char *end = json_compound_end(value);
		if (!end) continue;
		size_t len = (size_t)(end - value);
		char *header = malloc(len + 1);
		if (!header) return;
		memcpy(header, value, len);
		header[len] = '\0';
		json_doc_t doc;
		if (json_parse_limited(header, &doc, 2000)) {
			int root = json_root(&doc);
			if (!json_str(&doc, json_path(&doc, root, "title.runs.0.text"), out, size) || !out[0]) {
				json_str(&doc, json_path(&doc, root, "title.simpleText"), out, size);
			}
			json_free(&doc);
		}
		free(header);
		if (out[0]) return;
	}
}

// The cards under one carousel. The carousel is a slice of the whole page, so
// it is copied out (the scanner wants a terminated string) and the items are
// read from the copy.
static int collect_section_items(const char *object, size_t len, ytmusic_section_t *section) {
	char *copy = malloc(len + 1);
	if (!copy) return -1;
	memcpy(copy, object, len);
	copy[len] = '\0';
	read_section_title(copy, section->title, sizeof(section->title));
	int count = collect_items_compact(copy, section->items, YTMUSIC_SECTION_ITEMS_MAX);
	free(copy);
	return count;
}

static bool browse_id_valid(const char *id) {
	if (!id || !id[0] || strlen(id) >= YTMUSIC_BROWSE_ID_MAX) return false;
	for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
		if (!isalnum(*p) && *p != '-' && *p != '_') return false;
	}
	return true;
}

static bool playable_track_response(const json_doc_t *doc, int root, const char *video_id,
									char *error, size_t error_size) {
	char response_id[YTMUSIC_ID_MAX] = "";
	char status[32] = "";
	if (!json_obj_str(doc, json_get(doc, root, "videoDetails"), "videoId",
					  response_id, sizeof(response_id)) ||
		strcmp(response_id, video_id) != 0) {
		set_error(error, error_size, "ytmusic_unexpected_video");
		return false;
	}
	if (!json_obj_str(doc, json_get(doc, root, "playabilityStatus"), "status",
					  status, sizeof(status)) ||
		strcmp(status, "OK") != 0) {
		set_error(error, error_size, "ytmusic_track_unavailable");
		return false;
	}
	return true;
}

int ytmusic_search_items(const char *query, ytmusic_filter_t filter,
						 ytmusic_item_t *items, int limit, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!query || !query[0] || !items || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_search");
		return -1;
	}
	if (limit > YTMUSIC_ITEMS_MAX) limit = YTMUSIC_ITEMS_MAX;
	char escaped[512], request[1400], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	if (!json_escape(query, escaped, sizeof(escaped))) {
		set_error(error, error_size, "ytmusic_search_too_long");
		return -1;
	}
	// The songs filter and the podcasts one. Both are the store's own encoded
	// filter values, the same ones its search chips send.
	const char *params = filter == YTMUSIC_FILTER_PODCASTS ? "EgWKAQJQAWoMEA4QChADEAQQCRAF"
														 : "EgWKAQIIAWoKEAkQBRAKEAMQBA==";
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"query\":\"%s\",\"params\":\"%s\"}",
		hl, gl, escaped, params);
	char *body = NULL;
	// Search is public. Never attach the Google OAuth bearer token here: the
	// music catalogue can answer an OAuth request with a successful but empty
	// page, even though the same query works for a signed-out visitor.
	if (!youtubei_post_ex("search", request, false, true, &body, NULL, error, error_size)) return -1;
	size_t response_bytes = strlen(body);
	int count = collect_items_compact(body, items, limit);
	if (count <= 0) {
		ytmusic_log_empty_body("search", body);
	}
	free(body);
	fprintf(stderr, "ytmusic: public search returned %d rows from %zu response bytes\n", count, response_bytes);
	if (count < 0) set_error(error, error_size, "ytmusic_unreadable_reply");
	return count;
}

int ytmusic_browse_items(const char *browse_id, ytmusic_item_t *items, int limit,
						 char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!items || limit <= 0 || !browse_id_valid(browse_id)) {
		set_error(error, error_size, "ytmusic_invalid_browse");
		return -1;
	}
	if (limit > YTMUSIC_ITEMS_MAX) limit = YTMUSIC_ITEMS_MAX;
	char request[512], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"browseId\":\"%s\"}",
		hl, gl, browse_id);
	char *body = NULL;
	bool used_account = false;
	// Public albums, charts and search-result cards should load even when a
	// saved OAuth session is not accepted by the Music catalogue. Try them as a
	// visitor first; an account-owned playlist can still fall through below.
	if (!youtubei_post_ex("browse", request, false, true, &body, NULL, error, error_size)) {
		if (!account_attached() || !youtubei_post("browse", request, false, &body, NULL, error, error_size)) return -1;
		used_account = true;
	}
	int count = collect_items_compact(body, items, limit);
	if (count == 0 && !used_account) {
		ytmusic_log_empty_body(browse_id, body);
	}
	free(body);
	if (count == 0 && !used_account && account_attached()) {
		char *private_page = NULL;
		if (youtubei_post("browse", request, false, &private_page, NULL, error, error_size)) {
			count = collect_items_compact(private_page, items, limit);
			free(private_page);
		}
		if (error && error_size) error[0] = '\0';
	}
	// The liked songs -- "VLLM" -- are the account's own list, and the only
	// credential Google's music endpoints accept for it is the browser session
	// imported from the card. The Google sign-in token is refused, and refused
	// *quietly*: the page comes back with no rows and no error, so Favourites
	// sat there saying "no results" for an account that was in fact signed in.
	// Where the imported session is what is missing, say that instead of
	// showing an empty list that can never fill.
	if (count == 0 && strcmp(browse_id, "VLLM") == 0 && account_attached() &&
		!session_cookie_attached()) {
		set_error(error, error_size, "ytmusic_library_needs_session");
		return -1;
	}
	fprintf(stderr, "ytmusic: browse %s returned %d rows\n", browse_id, count);
	if (count < 0) {
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return count;
	}
	return count;
}

// The carousels of one Explore page, in the order the page lists them.
static int collect_sections(const char *body, ytmusic_section_t *sections, int limit) {
	int count = 0;
	static const char NEEDLE[] = "\"musicCarouselShelfRenderer\"";
	const char *p = body;
	while (count < limit && (p = strstr(p, NEEDLE)) != NULL) {
		p += sizeof(NEEDLE) - 1;
		const char *value = skip_json_space(p);
		if (*value != ':') continue;
		value = skip_json_space(value + 1);
		if (*value != '{') continue;
		const char *end = json_compound_end(value);
		if (!end) break;
		ytmusic_section_t *section = &sections[count];
		memset(section, 0, sizeof(*section));
		int items = collect_section_items(value, (size_t)(end - value), section);
		// A carousel whose cards are not playable or browsable -- "Moods &
		// genres" is a grid of filter buttons -- is not a row worth showing.
		if (items > 0) {
			section->count = items;
			count++;
		}
		p = end;
	}
	// A page without carousels still has rows on it: show them under one
	// untitled heading rather than an empty Explore page.
	if (!count) {
		memset(&sections[0], 0, sizeof(sections[0]));
		int items = collect_items_compact(body, sections[0].items, YTMUSIC_SECTION_ITEMS_MAX);
		if (items > 0) {
			sections[0].count = items;
			count = 1;
		}
	}
	return count;
}

int ytmusic_explore_sections(ytmusic_section_t *sections, int limit,
							 char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!sections || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_browse");
		return -1;
	}
	if (limit > YTMUSIC_SECTIONS_MAX) limit = YTMUSIC_SECTIONS_MAX;
	// Explore, the store's own front page of new releases and charts. It is a
	// public page, so it answers without an account.
	char request[512], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"browseId\":\"FEmusic_explore\"}",
		hl, gl);
	char *body = NULL;
	// Explore is public too; keep it independent of an attached account just
	// like search, so a rejected token cannot turn the front page empty.
	if (!youtubei_post_ex("browse", request, false, true, &body, NULL, error, error_size)) return -1;
	size_t response_bytes = strlen(body);
	int count = collect_sections(body, sections, limit);
	if (!count) {
		ytmusic_log_empty_body("explore", body);
	}
	free(body);
	fprintf(stderr, "ytmusic: public Explore returned %d sections from %zu response bytes\n", count, response_bytes);
	if (!count) set_error(error, error_size, "ytmusic_explore_empty");
	return count;
}

// The audio stream to download: the AAC track with the highest bitrate that
// the reply gives a plain URL for. Opus is skipped -- there is an Opus decoder
// but it reads Ogg, not the WebM container YouTube puts Opus in.
static bool pick_audio_format(const json_doc_t *doc, int root, char *url, size_t url_size) {
	int best = -1;
	long best_bitrate = -1;
	int formats = json_path(doc, root, "streamingData.adaptiveFormats");
	for (int i = 0; i < json_len(doc, formats); i++) {
		int format = json_at(doc, formats, i);
		char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
		json_obj_str(doc, format, "mimeType", mime, sizeof(mime));
		if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
		if (!json_obj_str(doc, format, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
		long bitrate = json_obj_long(doc, format, "bitrate", 0);
		if (bitrate > best_bitrate) {
			best = format;
			best_bitrate = bitrate;
		}
	}
	if (best < 0) {
		// Nothing audio-only answered a plain URL. That is the end of it: the
		// muxed progressive file carries a video track this player must never
		// fetch, demux or decode, so a video stream is not a fallback but the
		// one answer that is refused. player_reply_media() names the error.
		return false;
	}
	return json_obj_str(doc, best, "url", url, url_size) && url[0];
}

// One player reply: the track that was asked for, playable, with its media URL
// and the facts the now-playing screen shows.
static bool player_reply_media(const char *body, const char *video_id, char *url, size_t url_size,
							   int *duration, char *title, size_t title_size,
							   char *artist, size_t artist_size,
							   char *error, size_t error_size) {
	json_doc_t doc;
	if (!body || !json_parse_limited(youtubei_json_start(body), &doc, YTMUSIC_PLAYER_TOKEN_LIMIT)) {
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return false;
	}
	int root = json_root(&doc);
	if (!playable_track_response(&doc, root, video_id, error, error_size)) {
		json_free(&doc);
		return false;
	}
	int details = json_get(&doc, root, "videoDetails");
	if (duration) *duration = (int)json_obj_long(&doc, details, "lengthSeconds", 0);
	if (title && title_size) json_obj_str(&doc, details, "title", title, title_size);
	if (artist && artist_size) json_obj_str(&doc, details, "author", artist, artist_size);
	bool ok = pick_audio_format(&doc, root, url, url_size);
	json_free(&doc);
	if (!ok) set_error(error, error_size, "ytmusic_aac_stream_unavailable");
	return ok;
}

// One source's answer to "where are the bytes", named the way the log and the
// settings page speak about it: an instance base address, or one of these
// words.
#define YTMUSIC_VIA_WEB "web"
#define YTMUSIC_VIA_DIRECT "direct"
#define YTMUSIC_VIA_RELAY "relay"

// The imported browser session, through the web music player: it knows the
// account's region and, with Premium, its own stream. An OAuth token alone is
// not enough -- that client refuses one outright -- so this asks only when a
// cookie was imported from a browser.
static bool web_session_audio_url(const char *video_id, char *url, size_t url_size,
								  int *duration, char *title, size_t title_size,
								  char *artist, size_t artist_size, char *error,
								  size_t error_size) {
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool have = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	if (!have) return false;
	char request[1024], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
			 "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
			 client_version[0] ? client_version : YTMUSIC_CLIENT_VERSION_FALLBACK, hl, gl, video_id);
	char *body = NULL;
	bool ok = youtubei_post("player", request, true, &body, NULL, error, error_size) &&
		player_reply_media(body, video_id, url, url_size, duration, title, title_size, artist,
						   artist_size, error, error_size);
	free(body);
	return ok;
}

// One source, asked. False when it has nothing, with `error` naming why when
// it said more than a shrug.
static bool resolve_via(const char *source, const char *video_id, char *url, size_t url_size,
						int *duration, char *title, size_t title_size, char *artist,
						size_t artist_size, bool *proxied, uint64_t *expected_bytes, char *error,
						size_t error_size) {
	if (!source || !source[0]) return false;
	if (proxied) *proxied = false;
	if (expected_bytes) *expected_bytes = 0;
	if (strcmp(source, YTMUSIC_VIA_WEB) == 0) {
		return web_session_audio_url(video_id, url, url_size, duration, title, title_size, artist,
									 artist_size, error, error_size);
	}
	if (strcmp(source, YTMUSIC_VIA_DIRECT) == 0) {
		char *body = NULL;
		bool asked = android_player(video_id, &body, error, error_size);
		bool ok = asked && player_reply_media(body, video_id, url, url_size, duration, title,
											  title_size, artist, artist_size, error, error_size);
		free(body);
		return ok;
	}
	if (strcmp(source, YTMUSIC_VIA_RELAY) == 0) {
		return relay_audio_url(video_id, url, url_size, duration, title, title_size, artist,
							   artist_size, proxied, error, error_size);
	}
	return instance_audio_url(source, video_id, url, url_size, duration, title, title_size,
							  artist, artist_size, proxied, expected_bytes, error, error_size);
}

// What a source is called in the log: a host reads better than an address, and
// "this device" better than "direct".
static const char *source_label(const char *source, char *out, size_t size) {
	if (strcmp(source, YTMUSIC_VIA_WEB) == 0) {
		snprintf(out, size, "%s", "the browser session");
	} else if (strcmp(source, YTMUSIC_VIA_DIRECT) == 0) {
		snprintf(out, size, "%s", "this device");
	} else if (strcmp(source, YTMUSIC_VIA_RELAY) == 0) {
		snprintf(out, size, "%s", "the relay");
	} else {
		const char *host = strstr(source, "://");
		snprintf(out, size, "%s", host ? host + 3 : source);
	}
	return out;
}

// The answers one play attempt walks, in order, and the only place that order
// is written down:
//
//   1. The imported browser session, when there is one: it knows the account's
//      region and, with Premium, its own stream.
//   2. The instances, the pinned one first. This is the path measured to work
//      on this device with no other machine involved: an Invidious instance
//      fetches the audio from Google and passes the bytes on through
//      /latest_version?local=true, so a track plays with nothing but the
//      player and its Wi-Fi.
//   3. The device itself, through the phone-app player. It resolves an address
//      Google then refuses to serve (403 on every media fetch in the field
//      log), so it is a hopeful extra here rather than the primary answer --
//      but the day that refusal stops, it plays with no instance at all.
//   4. The relay, which needs another machine and so comes last.
//
// A pin to "direct" or "relay" replaces the whole list with that one answer.
// An instance pin is tried first and the rest still follow it.
static int play_sources(char out[][YTMUSIC_SOURCE_MAX], int limit) {
	if (!out || limit <= 0) return 0;
	int count = 0;
	const char *source = source_value();
	bool relay_only = strcmp(source, "relay") == 0;
	bool direct_only = strcmp(source, "direct") == 0;
	if (relay_only) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_RELAY);
		return count;
	}
	if (direct_only) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_DIRECT);
		return count;
	}
	// Two slots are kept for the device and the relay: an instance list
	// somebody wrote must not push them out of the walk.
	int room = limit > 2 ? limit - 2 : 1;
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool have_session = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	if (have_session && count < room) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_WEB);
	}
	char bases[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int instances = ytmusic_instance_bases(bases, YTMUSIC_SOURCES_MAX);
	// The built-in list is a guess at what might be up today; when it is the
	// list in use, a few tries is enough to know. A list somebody wrote
	// themselves is tried whole, because each entry there is a machine they
	// chose.
	if (!config_get("ytmusic", "instances", "")[0] && instances > YTMUSIC_INSTANCE_TRIES) {
		instances = YTMUSIC_INSTANCE_TRIES;
	}
	for (int i = 0; i < instances && count < room; i++) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1, bases[i]);
	}
	if (count < limit) snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_DIRECT);
	if (count < limit) snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_RELAY);
	return count;
}

bool ytmusic_audio_url(const char *video_id, char *url, size_t url_size,
					  int *duration, char *title, size_t title_size,
					  char *artist, size_t artist_size, bool *relay_proxied,
					  char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (relay_proxied) *relay_proxied = false;
	if (!video_id || strlen(video_id) != 11 || !url || !url_size) {
		set_error(error, error_size, "ytmusic_invalid_track");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)video_id; *p; p++) {
		if (!isalnum(*p) && *p != '-' && *p != '_') {
			set_error(error, error_size, "ytmusic_invalid_track");
			return false;
		}
	}

	char sources[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int count = play_sources(sources, YTMUSIC_SOURCES_MAX);
	// The error left standing is the last one a source had something to say
	// about: "the relay does not serve /streams" is actionable, and a generic
	// "no source" would throw it away.
	char last[192] = "";
	for (int i = 0; i < count; i++) {
		char label[YTMUSIC_SOURCE_MAX], reason[192] = "";
		source_label(sources[i], label, sizeof(label));
		if (resolve_via(sources[i], video_id, url, url_size, duration, title, title_size,
						artist, artist_size, relay_proxied, NULL, reason, sizeof(reason))) {
			printf("ytmusic: %.11s: %s served it\n", video_id, label);
			return true;
		}
		// A half-written address is worse than none: the caller fetches what
		// this buffer holds.
		url[0] = '\0';
		if (relay_proxied) *relay_proxied = false;
		if (reason[0]) snprintf(last, sizeof(last), "%s", reason);
		fprintf(stderr, "ytmusic: %.11s: %s had nothing (%s)\n", video_id, label,
				reason[0] ? reason : "no reason given");
	}
	set_error(error, error_size, last[0] ? last : "ytmusic_no_source");
	return false;
}

static void clean_tag_value(char *value) {
	for (; value && *value; value++) {
		if (*value == '\r' || *value == '\n') *value = ' ';
	}
}

static bool write_playback_tags(const char *path, const ytmusic_track_t *track) {
	char tags[640], tmp[680];
	if (snprintf(tags, sizeof(tags), "%s.tags", path) >= (int)sizeof(tags) ||
		snprintf(tmp, sizeof(tmp), "%s.tmp", tags) >= (int)sizeof(tmp)) return false;
	char title[YTMUSIC_TITLE_MAX], artist[YTMUSIC_NAME_MAX], album[YTMUSIC_TITLE_MAX];
	snprintf(title, sizeof(title), "%s", track->title);
	snprintf(artist, sizeof(artist), "%s", track->artist);
	snprintf(album, sizeof(album), "%s", track->album);
	clean_tag_value(title);
	clean_tag_value(artist);
	clean_tag_value(album);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	bool wrote = fprintf(f, "title=%s\nartist=%s\nalbum=%s\n", title, artist, album) > 0;
	bool closed = fclose(f) == 0;
	bool ok = wrote && closed;
	if (!ok || rename(tmp, tags) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

// Google throttles one long transfer of a media URL to about 32 KB/s -- a
// three-megabyte track then takes two minutes before a note comes out of the
// player -- while the same bytes asked for a few hundred kilobytes at a time
// arrive at the link's own speed. So the download is chunked, and one long
// transfer is kept as the fallback for a server that ignores Range.
#define YTMUSIC_CHUNK (256 * 1024)

// Where the playback cache lives: /tmp on the device, because it is scratch and
// nothing there has to survive a reboot. SONIX_YTMUSIC_CACHE moves it, which is
// what lets the host suite write it at all -- some Windows machines refuse
// every create under \tmp, and a suite that cannot make the folder would have
// to skip the download checks it exists to make.
static const char *playback_cache_dir(void) {
	const char *override = getenv("SONIX_YTMUSIC_CACHE");
	return (override && override[0]) ? override : "/tmp/sonix-ytmusic";
}

// What the download is doing, for the page's loading card. A percentage rather
// than a byte count: a 64-bit counter read from another thread on a 32-bit
// device can tear, and being one step out on a bar is not a problem.
static volatile int progress_percent = -1;
static volatile int progress_active;
static uint64_t progress_expected;

void ytmusic_download_progress(int *percent, bool *running) {
	if (percent) *percent = progress_percent;
	if (running) *running = progress_active != 0;
}

static void progress_note(uint64_t done) {
	if (progress_expected == 0) return;
	uint64_t percent = done * 100 / progress_expected;
	progress_percent = percent > 100 ? 100 : (int)percent;
}

// Starting the player before the file is whole.
//
// growfile.c gives the decoder a file that is still being written -- the same
// mechanism Qobuz plays through -- and this is the other end of that contract:
// announce the path, say what has arrived after every chunk, and say once, at
// the byte the track can be opened from, that playback may begin. NULL means an
// ordinary download, which is what "keep on card" wants: it has nothing to
// play while it waits, and a file that is only whole at the end.
typedef struct {
	const char *path;   // the file as the player will open it
	uint64_t ready_at;  // bytes that have to be on disk before it may
	ytmusic_ready_cb ready;
	void *user;
	bool called;
	bool indexed;       // the moov box has been looked for
	bool have_moov;     // and found, so the decoder has something to read
	uint64_t indexed_at;
} stream_hook_t;

// Is the MP4 index in the part that has arrived?
//
// An M4A is no use without its `moov` box: that is where the sample table
// lives, and the decoder cannot describe the first frame without it. It sits at
// the front of the streams these instances serve, but "at the front" is a
// property of the container, not a promise, and handing the player a file whose
// index has not landed yet is a decode failure a second before it would have
// worked. So the start waits for it -- scanning once, over the first 64 kB,
// which is where a faststart file keeps it.
static bool moov_arrived(const char *path, uint64_t upto) {
	uint8_t head[64 * 1024];
	size_t want = upto < sizeof(head) ? (size_t)upto : sizeof(head);
	if (want < 8) return false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	size_t got = fread(head, 1, want, f);
	fclose(f);
	for (size_t i = 0; i + 4 <= got; i++) {
		if (memcmp(head + i, "moov", 4) == 0) return true;
	}
	return false;
}

// How far past the threshold the start will wait for the index before going
// ahead anyway: a stream that keeps its moov at the end still plays, it just
// plays when it would have before this existed.
#define STREAM_MOOV_PATIENCE (512 * 1024)

static void stream_note(stream_hook_t *hook, uint64_t done) {
	if (!hook) return;
	growfile_progress(hook->path, (long)done);
	if (hook->called || done < hook->ready_at) return;
	if (!hook->indexed) {
		hook->indexed = true;
		hook->indexed_at = done;
		hook->have_moov = moov_arrived(hook->path, done);
	}
	if (!hook->have_moov && done < hook->indexed_at + STREAM_MOOV_PATIENCE) return;
	if (!hook->have_moov) {
		printf("ytmusic: no moov box in the first part; starting anyway\n");
	}
	hook->called = true;
	printf("ytmusic: %llu bytes are enough to start playing\n", (unsigned long long)done);
	if (hook->ready) hook->ready(hook->path, hook->user);
}

// How many bytes before the decoder is given the file. A fraction of the whole
// when the instance said how big it is -- an eighth, so a long track is not
// held back for longer than a short one -- with a floor and a ceiling: under
// the floor the header and the first frames are not there yet, and above the
// ceiling the wait stops being about latency and starts being the download.
static uint64_t stream_threshold(uint64_t expected) {
	uint64_t at = expected ? expected / 8 : 0;
	if (at < 196608) at = 196608;
	if (expected && at > expected / 2) at = expected / 2;
	if (at > 1048576) at = 1048576;
	return at;
}

// The file in ranged chunks. False when Range is not honoured (or nothing came
// back), which is the caller's cue to download it in one piece instead.
static bool download_ranged(const char *url, const char *part, uint64_t *downloaded, bool *complete,
							stream_hook_t *hook) {
	*downloaded = 0;
	*complete = false;
	FILE *f = fopen(part, "wb");
	if (!f) {
		fprintf(stderr, "ytmusic: the cache file %.64s cannot be written (%s)\n", part,
				strerror(errno));
		return false;
	}
	bool ok = true;
	int empty_asks = 0;
	while (!*complete) {
		char range[96], *body = NULL;
		size_t len = 0;
		int status = 0;
		// A connection that drops mid-chunk is worth one more ask for the same
		// bytes: the offset has not moved, so nothing is written twice.
		bool got = false;
		for (int attempt = 0; attempt < 2 && !got; attempt++) {
			snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
					 (unsigned long long)*downloaded,
					 (unsigned long long)(*downloaded + YTMUSIC_CHUNK - 1));
			http_req_t req = {.method = "GET",
							  .extra_headers = range,
							  .allow_empty_body = true, // a 206 is success, and so is a 204
							  .want_error_body = true,  // a refusal says why in its body
							  .no_https_downgrade = true};
			got = http_request(url, &req, &body, &len, YTMUSIC_CHUNK + 1, YTMUSIC_RELAY_TIMEOUT,
							   &status);
			if (!got) {
				fprintf(stderr, "http: media fetch failed at byte %llu: %s%s\n",
						(unsigned long long)*downloaded,
						http_last_error() ? http_last_error() : "no reason given",
						attempt == 0 ? ", asking again" : "");
				free(body);
				body = NULL;
				len = 0;
			}
		}
		if (!got) {
			ok = false;
			break;
		}
		if (status >= 400) {
			// A refused media fetch carries its reason in the body -- Google
			// says which rule was broken, and that sentence is the whole
			// diagnosis. It goes to the log, one line, with the newlines out.
			char why[161];
			size_t n = body && len < 160 ? len : (body ? 160 : 0);
			if (n) memcpy(why, body, n);
			why[n] = '\0';
			for (size_t k = 0; k < n; k++) {
				if (why[k] == '\r' || why[k] == '\n' || why[k] == '\t') why[k] = ' ';
			}
			fprintf(stderr, "http: media answered %d: %s\n", status, n ? why : "(no body)");
		}
		if (status == 200) {
			// The range was ignored and this is the whole file (or as much of
			// it as was kept): not a piece of anything.
			free(body);
			ok = false;
			break;
		}
		if (status == 416) {
			// Asked past the end: what is on disk is the whole file.
			free(body);
			*complete = *downloaded > 0;
			break;
		}
		if (len == 0) {
			// A 206 carrying nothing is the instance's own upstream pausing, not
			// the end of the file -- treating it as an ending is the other way a
			// track used to finish after a minute. Only a 416 says the file is
			// over, so ask again for the same offset a few times first.
			free(body);
			if (++empty_asks >= 6) {
				*complete = *downloaded > 0;
				break;
			}
			usleep(300 * 1000);
			continue;
		}
		if (status != 206 || !body || fwrite(body, 1, len, f) != len) {
			free(body);
			ok = false;
			break;
		}
		free(body);
		*downloaded += len;
		// Flushed before it is announced: a reader that is handed this path
		// reads the file, not the writer's stdio buffer, so bytes still sitting
		// in it are bytes the decoder waits for. Only the growing case pays for
		// this -- an ordinary download has no reader until it is over.
		if (hook) fflush(f);
		progress_note(*downloaded);
		stream_note(hook, *downloaded);
		empty_asks = 0;
		// A short chunk is NOT the end of the file. Invidious proxies the bytes
		// and hands back less than was asked for whenever its own upstream
		// pauses, and taking that for an ending is what cut a three-minute
		// track off after a minute, at a file size the player then reported as
		// a 35 kbps stream. So the next request asks for what follows, and the
		// server says 416 -- or sends nothing at all -- only when there is
		// nothing left.
	}
	if (fclose(f) != 0) ok = false;
	return ok && *complete;
}

// The same file in one transfer, for a server that does not answer Range. This
// is what the download always did, and the reason for the chunked path above.
static bool download_streamed(const char *url, const char *part, uint64_t *downloaded, bool *complete,
							  stream_hook_t *hook) {
	*downloaded = 0;
	*complete = false;
	http_stream_t stream;
	http_stream_options_t options = {.no_icy = true, .no_https_downgrade = true};
	if (!http_stream_open_ex(&stream, url, YTMUSIC_RELAY_TIMEOUT, &options)) {
		return false;
	}
	if (stream.content_length64) progress_expected = stream.content_length64;
	FILE *f = fopen(part, "wb");
	if (!f) {
		http_stream_close(&stream);
		return false;
	}
	char *buffer = malloc(32768);
	bool ok = buffer != NULL;
	while (ok) {
		int n = http_stream_read(&stream, buffer, 32768);
		if (n < 0) {
			ok = false;
			break;
		}
		if (n == 0) break;
		if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		*downloaded += (uint64_t)n;
		if (hook) fflush(f);
		progress_note(*downloaded);
		stream_note(hook, *downloaded);
	}
	free(buffer);
	if (fclose(f) != 0) ok = false;
	if (stream.content_length64 && *downloaded != stream.content_length64) ok = false;
	http_stream_close(&stream);
	*complete = ok;
	return ok;
}

static void prune_playback_cache(const char *keep_id) {
	const char *cache = playback_cache_dir();
	DIR *dir = opendir(cache);
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		char *dot = strrchr(entry->d_name, '.');
		if (!dot || (strcmp(dot, ".m4a") != 0 && strcmp(dot, ".tags") != 0 &&
					 strcmp(dot, ".part") != 0 && strcmp(dot, ".jpg") != 0)) {
			continue;
		}
		if (strncmp(entry->d_name, keep_id, 11) == 0 && entry->d_name[11] == '.') continue;
		char path[640];
		if (snprintf(path, sizeof(path), "%s/%s", cache, entry->d_name) >= (int)sizeof(path)) continue;
		// The song that just stopped is still somebody's: "save to the card"
		// copies it out on a worker thread, and deleting it here would leave a
		// truncated track on the card. Anything touched in the last two minutes
		// is left alone; the sweep catches it on a later play.
		struct stat st;
		if (stat(path, &st) == 0 && st.st_mtime > time(NULL) - 120) continue;
		remove(path);
	}
	closedir(dir);
}

// Whether a path is one of this module's own finished downloads -- the file the
// player was handed when a YouTube Music song started. That is what the
// now-playing menu's "save to the card" copies out, and nothing else may be
// treated as such: a file the user put on the card is already theirs.
bool ytmusic_cache_owns(const char *path) {
	const char *cache = playback_cache_dir();
	size_t prefix_len = strlen(cache);
	if (!path || strncmp(path, cache, prefix_len) != 0 || path[prefix_len] != '/') {
		return false;
	}
	const char *leaf = path + prefix_len + 1;
	if (!leaf[0] || strchr(leaf, '/')) {
		return false;
	}
	size_t n = strlen(leaf);
	return n > 4 && strcmp(leaf + n - 4, ".m4a") == 0;
}

// Playback: every source the player has, in turn, until one hands over the
// whole track. Resolving is not the same as playing -- a source can name a
// stream that is then refused, which is what the device's own Google address
// does (403 on every media fetch in the field log) -- so a source that keeps no
// bytes is passed over and the next one is asked, each attempt in the log.
// The sleeve.
//
// YouTube's artwork is a URL on the row, not a picture inside the file, and the
// player reads a track's cover from the file it was handed -- or from an image
// beside it named after it (albumart's find_cover_file). So the row's picture is
// fetched and written as <id>.jpg next to <id>.m4a: the cover loader then finds
// it the same way it finds any folder cover, and now-playing has a sleeve.
//
// The URL carries its own sizing directive after the last '=' ("=w120-h120-"),
// so that is replaced rather than the picture being asked for at thumbnail size.
#define YTMUSIC_COVER_MAX (512 * 1024)
#define YTMUSIC_COVER_SIZE 320

static void cover_url_at_size(const char *in, char *out, size_t size) {
	if (!out || !size) return;
	out[0] = '\0';
	if (!in || !in[0]) return;
	const char *eq = strrchr(in, '=');
	size_t base = eq ? (size_t)(eq - in) : strlen(in);
	if (base == 0 || base + 40 >= size) {
		snprintf(out, size, "%.*s", (int)(size - 1), in);
		return;
	}
	snprintf(out, size, "%.*s=w%d-h%d-l90-rj", (int)base, in, YTMUSIC_COVER_SIZE,
			 YTMUSIC_COVER_SIZE);
}

static void save_cover(const char *audio_path, const char *thumb_url) {
	char url[YTMUSIC_URL_MAX];
	cover_url_at_size(thumb_url, url, sizeof(url));
	if (!url[0]) return;
	char cover[600];
	snprintf(cover, sizeof(cover), "%s", audio_path);
	char *dot = strrchr(cover, '.');
	if (dot) *dot = '\0';
	if (strlen(cover) + 5 >= sizeof(cover)) return;
	snprintf(cover + strlen(cover), sizeof(cover) - strlen(cover), ".jpg");
	char *body = NULL;
	size_t len = 0;
	pthread_mutex_lock(&request_lock);
	bool got = http_get_ex(url, NULL, false, &body, &len, YTMUSIC_COVER_MAX, YTMUSIC_TIMEOUT, NULL);
	pthread_mutex_unlock(&request_lock);
	if (!got || !body || len < 1024) {
		fprintf(stderr, "ytmusic: the cover was not fetched (%.96s)\n", url);
		free(body);
		return;
	}
	FILE *f = fopen(cover, "wb");
	bool wrote = f && fwrite(body, 1, len, f) == len;
	if (f) fclose(f);
	free(body);
	if (!wrote) {
		remove(cover);
		return;
	}
	printf("ytmusic: cover saved, %zu bytes\n", len);
}

// `hook` non-NULL means the player is waiting: the bytes go straight into the
// file it will open, the file is announced to growfile so the decoder can read
// it while it fills, and playback is started as soon as there is enough of it.
// NULL is the plain download -- a .part beside it, renamed only when whole.
static bool download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								  stream_hook_t *hook, char *error, size_t error_size) {
	bool streaming = hook != NULL;
	if (error && error_size) error[0] = '\0';
	if (!track || !path || !path_size || strlen(track->id) != 11) {
		set_error(error, error_size, "ytmusic_invalid_track");
		return false;
	}
	const char *cache = playback_cache_dir();
	if (mkdir(cache, 0700) != 0 && errno != EEXIST) {
		set_error(error, error_size, "ytmusic_cache_unavailable");
		return false;
	}
	char final[512], part[520];
	if (snprintf(final, sizeof(final), "%s/%s.m4a", cache, track->id) >= (int)sizeof(final) ||
		snprintf(part, sizeof(part), "%s.part", final) >= (int)sizeof(part) ||
		strlen(final) >= path_size) {
		set_error(error, error_size, "ytmusic_path_too_long");
		return false;
	}
	prune_playback_cache(track->id);

	char sources[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int count = play_sources(sources, YTMUSIC_SOURCES_MAX);
	char last[192] = "";
	bool kept_any = false;
	for (int i = 0; i < count; i++) {
		char label[YTMUSIC_SOURCE_MAX], reason[192] = "", url[YTMUSIC_URL_MAX];
		bool proxied = false;
		uint64_t expected = 0;
		source_label(sources[i], label, sizeof(label));
		printf("ytmusic: %.11s: asking %s\n", track->id, label);
		if (!resolve_via(sources[i], track->id, url, sizeof(url), NULL, NULL, 0, NULL, 0,
						 &proxied, &expected, reason, sizeof(reason))) {
			fprintf(stderr, "ytmusic: %.11s: %s had nothing (%s)\n", track->id, label,
					reason[0] ? reason : "no reason given");
			continue;
		}
		printf("ytmusic: %.11s: fetching from %s: %s\n", track->id, label, url);
		// The loading card's bar: the total is what this source said the file
		// is, and is 0 when it never said.
		progress_expected = expected;
		progress_percent = -1;
		// A download for playback writes the file the player opens; a plain
		// download keeps its .part until it is whole.
		const char *target = streaming ? final : part;
		if (streaming) {
			hook->path = final;
			hook->ready_at = stream_threshold(expected);
			// Announced before the first byte, because the reader registers
			// itself from the player the moment playback starts, and an
			// unannounced path is an ordinary -- empty -- file to it.
			remove(final);
			growfile_announce(final, (long)expected);
		} else {
			remove(part);
		}
		uint64_t downloaded = 0;
		bool complete = false;
		bool ok = download_ranged(url, target, &downloaded, &complete, hook);
		if (!ok && !(hook && hook->called)) {
			// A server that will not answer a Range keeps the old behaviour: one
			// transfer, started over, with the signature problem to live with.
			// Not once playback has begun: starting the file over would truncate
			// it under the decoder that is already reading it.
			printf("ytmusic: %.11s: %s refused ranged chunks, one transfer instead\n",
				   track->id, label);
			remove(target);
			downloaded = 0;
			complete = false;
			if (streaming) {
				// The file starts over, so the search for the index does too:
				// what the first attempt read is not on the disk any more.
				hook->indexed = false;
				hook->have_moov = false;
				growfile_announce(final, (long)expected);
			}
			ok = download_streamed(url, target, &downloaded, &complete, hook);
		}
		memset(url, 0, sizeof(url));
		// The instance's reply says how many bytes the whole stream is, so a
		// transfer that stopped early is caught here rather than playing as a
		// track that ends after a minute.
		if (ok && complete && expected && downloaded != expected) {
			printf("ytmusic: %.11s: %s stopped at %llu of %llu bytes\n", track->id, label,
				   (unsigned long long)downloaded, (unsigned long long)expected);
			ok = false;
			complete = false;
		}
		bool landed = ok && complete && downloaded >= 1024;
		if (landed && !streaming) {
			// A whole file replaces whatever the cache held under that name. The
			// remove is for Windows’ rename(), which refuses an existing target
			// where POSIX replaces it; the device never needed it.
			remove(final);
			if (rename(part, final) != 0) landed = false;
		}
		if (landed) {
			write_playback_tags(final, track);
			save_cover(final, track->thumbnail);
			snprintf(path, path_size, "%s", final);
			if (streaming) {
				// A file that is whole is an ordinary file from here on. A track
				// short enough to have finished before the threshold was reached
				// starts the player here instead, at its end.
				if (!hook->called) {
					hook->called = true;
					if (hook->ready) hook->ready(final, hook->user);
				}
				growfile_finish(final, true);
			}
			printf("ytmusic: %.11s: played from %s, %llu bytes\n", track->id, label,
				   (unsigned long long)downloaded);
			return true;
		}
		if (streaming) {
			growfile_finish(final, false);
			if (hook->called) {
				// Playback is already running on what arrived. The file is left as
				// it is: removing it would take the decoder's file out from under
				// the listener, and the next play of this song truncates it anyway.
				printf("ytmusic: %.11s: %s stopped at %llu bytes after playback began\n",
					   track->id, label, (unsigned long long)downloaded);
				return true;
			}
			remove(final);
		} else {
			remove(part);
		}
		printf("ytmusic: %.11s: %s kept %llu bytes (%s, %s)\n", track->id, label,
			   (unsigned long long)downloaded, ok ? "complete" : "failed",
			   http_last_error() ? http_last_error() : "no reason given");
		if (downloaded == 0) {
			const char *why = http_last_error() ? http_last_error() : "ytmusic_stream_failed";
			if (strcmp(sources[i], YTMUSIC_VIA_RELAY) == 0) why = "ytmusic_relay_not_serving";
			snprintf(last, sizeof(last), "%s", why);
		} else {
			kept_any = true;
			snprintf(last, sizeof(last), "%s", "ytmusic_download_incomplete");
		}
	}
	set_error(error, error_size,
			  last[0] ? last : (kept_any ? "ytmusic_download_incomplete" : "ytmusic_no_source"));
	return false;
}

// The entry the page calls. The work is download_for_playback above; this marks
// the span for the loading card, so a bar is shown from the first byte to the
// last -- a track takes long enough over a public instance that being told
// something is happening is the difference between waiting and thinking the
// player has hung.
bool ytmusic_download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								   char *error, size_t error_size) {
	progress_expected = 0;
	progress_percent = -1;
	progress_active = 1;
	bool ok = download_for_playback(track, path, path_size, NULL, error, error_size);
	progress_active = 0;
	progress_expected = 0;
	progress_percent = -1;
	return ok;
}

// The same download, started for a listener who is waiting to hear it: the
// loading card's span covers the whole transfer, and `ready` fires part-way
// through it so the page can hand the growing file to the player.
bool ytmusic_stream_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								 ytmusic_ready_cb ready, void *user,
								 char *error, size_t error_size) {
	stream_hook_t hook = {.path = NULL, .ready_at = 0, .ready = ready, .user = user, .called = false};
	progress_expected = 0;
	progress_percent = -1;
	progress_active = 1;
	bool ok = download_for_playback(track, path, path_size, &hook, error, error_size);
	progress_active = 0;
	progress_expected = 0;
	progress_percent = -1;
	return ok;
}
