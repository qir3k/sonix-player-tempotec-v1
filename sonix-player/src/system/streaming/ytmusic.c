#define _GNU_SOURCE 1
#include "ytmusic.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/sha1.h"
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
#define YTMUSIC_OAUTH_DEVICE_URL "https://oauth2.googleapis.com/device/code"
#define YTMUSIC_OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define YTMUSIC_OAUTH_SCOPE "https://www.googleapis.com/auth/youtube.readonly"

// The web client's public InnerTube key. It is not an account credential: it
// is the key music.youtube.com hands every visitor, and every open-source
// YouTube Music client carries it. It is used directly rather than scraped
// from the page, because the page now answers a client whose User-Agent it
// does not recognise -- and this player is not a browser -- with a "your
// browser is deprecated" stub that contains no key at all. That stub is what
// ytmusic_context_unreadable was.
//
// The client version is the page's build stamp, in the same "1.YYYYMMDD.01.00"
// form the public clients use, derived from today's date so it never goes
// stale in a shipped image.
#define YTMUSIC_API_KEY "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX30"
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
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= YTMUSIC_COOKIE_MAX) {
		set_error(error, error_size, "ytmusic_session_file_invalid");
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		set_error(error, error_size, "ytmusic_session_file_unreadable");
		return false;
	}
	char *cookie = calloc(YTMUSIC_COOKIE_MAX, 1);
	if (!cookie) {
		fclose(f);
		set_error(error, error_size, "ytmusic_out_of_memory");
		return false;
	}
	bool read_ok = fgets(cookie, YTMUSIC_COOKIE_MAX, f) != NULL;
	int extra = fgetc(f); // reject a second line or truncated input
	fclose(f);
	char *line_end = strpbrk(cookie, "\r\n");
	if (line_end) *line_end = '\0';
	char *value = strncmp(cookie, "cookie=", 7) == 0 ? cookie + 7 : cookie;
	// Accept the exact value emitted by browser cookie exporters as well as a
	// bare Cookie header value. Keeping the field name would otherwise result
	// in an invalid nested `Cookie: Cookie: ...` request after import/reboot.
	if (strncasecmp(value, "Cookie:", 7) == 0) {
		value += 7;
		while (*value == ' ' || *value == '\t') value++;
	}
	bool ok = read_ok && extra == EOF && ytmusic_set_session_cookie(value, error, error_size);
	memset(cookie, 0, YTMUSIC_COOKIE_MAX);
	free(cookie);
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
	char device_code[1400], client_id[768], secret[768], form[3500];
	http_url_encode(login->device_code, device_code, sizeof(device_code));
	http_url_encode(oauth_client_id, client_id, sizeof(client_id));
	http_url_encode(oauth_client_secret, secret, sizeof(secret));
	int n = snprintf(form, sizeof(form),
		"client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
		client_id, device_code);
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

static bool auth_headers(char *out, size_t size, bool required, char *error, size_t error_size) {
	char cookie[YTMUSIC_COOKIE_MAX];
	char token[sizeof(access_token)];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	pthread_mutex_lock(&session_lock);
	bool oauth = refresh_token[0] != '\0';
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
		int n = snprintf(out, size, "Authorization: Bearer %s\r\nOrigin: %s\r\nReferer: %s/\r\n",
						 token, YTMUSIC_SITE, YTMUSIC_SITE);
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

// The charts landing page is a set of carousels; the tracks themselves are one
// browse away, in the playlist behind each chart. This finds the first such
// playlist ("VL...") the page offers, for the case where the songs chart did
// not arrive as a list of rows -- signed out, YouTube returns only the video
// charts. Without it the row opened a page with nothing playable on it.
static bool first_chart_playlist(const char *body, char *out, size_t size) {
	// The whole value, "VL" included: a playlist is browsed as "VL<id>" and a
	// bare "PL..." is refused. Chart entries carry it complete.
	static const char NEEDLE[] = "\"browseId\":\"";
	const char *p = body;
	while ((p = strstr(p, NEEDLE)) != NULL) {
		p += sizeof(NEEDLE) - 1;
		if (strncmp(p, "VL", 2) != 0) {
			continue;
		}
		const char *end = strchr(p, '"');
		if (!end || end == p) return false;
		size_t n = (size_t)(end - p);
		if (n + 1 > size) return false;
		memcpy(out, p, n);
		out[n] = '\0';
		return true;
	}
	return false;
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

static bool youtubei_post(const char *endpoint, const char *body, bool login_required,
						  char **response, int *status, char *error, size_t error_size) {
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
	for (;;) {
		size_t used = strlen(headers);
		if (!auth_headers(headers + used, sizeof(headers) - used, login_required, error, error_size)) {
			memset(headers, 0, sizeof(headers));
			memset(request_body, 0, sizeof(request_body));
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		ok = http_request(url, &req, response, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_TIMEOUT, &status_code);
		if (!ok || status_code != 401 || retried_oauth) break;

		// The device clock can be corrected after boot, so a locally unexpired
		// token may already be rejected by Google. Force one refresh and retry
		// this read-only request before reporting the saved session as expired.
		pthread_mutex_lock(&session_lock);
		bool can_refresh = refresh_token[0] != '\0';
		if (can_refresh) access_expires_at = 0;
		pthread_mutex_unlock(&session_lock);
		if (!can_refresh) break;
		if (response && *response) {
			wipe_free(*response);
			*response = NULL;
		}
		memset(headers + used, 0, sizeof(headers) - used);
		retried_oauth = true;
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

// The web music client sometimes returns signatureCipher instead of a direct
// stream URL. The device has no JavaScript runtime to execute YouTube's player
// script, so request an AAC-capable Android VR player response as a direct-URL
// fallback. Search and account personalization still use the saved Music
// session; the VR client is only used to resolve the media bytes.
static bool android_vr_player(const char *video_id, char **response, char *error, size_t error_size) {
	if (!ensure_context(error, error_size)) return false;
	char key[sizeof(api_key)];
	pthread_mutex_lock(&api_lock);
	snprintf(key, sizeof(key), "%s", api_key);
	pthread_mutex_unlock(&api_lock);
	char url[512], body[1024];
	snprintf(url, sizeof(url), "https://www.youtube.com/youtubei/v1/player?key=%s&prettyPrint=false", key);
	snprintf(body, sizeof(body),
		"{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.65.10\",\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", video_id);
	const char *headers = "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/\r\nX-YouTube-Client-Name: 28\r\nX-YouTube-Client-Version: 1.65.10\r\n";
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

static bool read_track(const json_doc_t *doc, int item, ytmusic_track_t *track) {
	memset(track, 0, sizeof(*track));
	int renderer = json_get(doc, item, "musicResponsiveListItemRenderer");
	if (renderer < 0) renderer = item;
	if (!token_video_id(doc, renderer, track->id)) return false;
	json_str(doc, json_path(doc, renderer, "flexColumns.0.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), track->title, sizeof(track->title));
	json_str(doc, json_path(doc, renderer, "flexColumns.1.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), track->artist, sizeof(track->artist));
	json_str(doc, json_path(doc, renderer, "flexColumns.2.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), track->album, sizeof(track->album));
	char duration[32] = "";
	json_str(doc, json_path(doc, renderer, "fixedColumns.0.musicResponsiveListItemFixedColumnRenderer.text.runs.0.text"), duration, sizeof(duration));
	track->duration = parse_duration(duration);
	int thumbs = json_path(doc, renderer, "thumbnail.musicThumbnailRenderer.thumbnail.thumbnails");
	int count = json_len(doc, thumbs);
	if (count > 0) json_obj_str(doc, json_at(doc, thumbs, count - 1), "url", track->thumbnail, sizeof(track->thumbnail));
	return track->title[0] != '\0';
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
	for (int i = renderer + 1; i < doc->count; i++) {
		int object = doc->toks[i].parent;
		if (doc->toks[i].type != JSON_STRING || object < 0 || doc->toks[object].type != JSON_OBJECT) continue;
		char key[128];
		if (!json_str(doc, i, key, sizeof(key))) continue;
		if (strcasestr(key, "adSlot") || strcasestr(key, "adPlacement") ||
			strcasestr(key, "displayAd") || strcasestr(key, "promoted") ||
			strcasestr(key, "sponsored") || json_obj_bool(doc, object, key, false)) return true;
	}
	return false;
}

// Parse only each musicResponsiveListItemRenderer instead of materializing
// the entire search/charts response as one token tree. This uses much less RAM
// and still returns complete early rows if YouTube sends a response larger
// than our bounded HTTP buffer.
static int collect_track_rows_compact(const char *response, ytmusic_track_t *tracks, int limit) {
	const char *json = youtubei_json_start(response);
	if (!json || (*json != '{' && *json != '[')) return -1;
	static const char ROW_KEY[] = "musicResponsiveListItemRenderer";
	const size_t key_len = sizeof(ROW_KEY) - 1;
	int count = 0;
	bool saw_unreadable_row = false;
	const char *p = json;

	while (*p && count < limit) {
		if (*p != '"') {
			p++;
			continue;
		}
		const char *key_end = json_string_end(p);
		if (!key_end) break;
		bool is_row_key = (size_t)(key_end - p - 2) == key_len &&
			memcmp(p + 1, ROW_KEY, key_len) == 0;
		const char *value = skip_json_space(key_end);
		if (!is_row_key || *value != ':') {
			p = key_end;
			continue;
		}
		value = skip_json_space(value + 1);
		if (*value != '{') {
			saw_unreadable_row = true;
			p = key_end;
			continue;
		}
		const char *end = json_compound_end(value);
		if (!end) {
			saw_unreadable_row = true;
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
				ytmusic_track_t candidate;
				if (!track_renderer_is_ad(&item_doc, root)) {
					if (read_track(&item_doc, root, &candidate)) {
						bool duplicate = false;
						for (int i = 0; i < count; i++) {
							if (strcmp(tracks[i].id, candidate.id) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) tracks[count++] = candidate;
					} else {
						saw_unreadable_row = true;
					}
				}
				json_free(&item_doc);
			} else {
				saw_unreadable_row = true;
			}
			free(item_json);
		} else {
			saw_unreadable_row = true;
		}
		p = end;
	}
	if (!count && saw_unreadable_row) return -1;
	return count;
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

int ytmusic_search_tracks(const char *query, ytmusic_track_t *tracks, int limit,
						  char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!query || !query[0] || !tracks || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_search");
		return -1;
	}
	if (limit > YTMUSIC_TRACKS_MAX) limit = YTMUSIC_TRACKS_MAX;
	char escaped[512], request[1200];
	if (!json_escape(query, escaped, sizeof(escaped))) {
		set_error(error, error_size, "ytmusic_search_too_long");
		return -1;
	}
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"en\",\"gl\":\"US\"}},\"query\":\"%s\",\"params\":\"EgWKAQIIAWoKEAkQBRAKEAMQBA==\"}", escaped);
	char *body = NULL;
	if (!youtubei_post("search", request, false, &body, NULL, error, error_size)) return -1;
	int count = collect_track_rows_compact(body, tracks, limit);
	free(body);
	if (count < 0) set_error(error, error_size, "ytmusic_unreadable_reply");
	return count;
}

int ytmusic_browse_chart_tracks(ytmusic_track_t *tracks, int limit,
								char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!tracks || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_browse");
		return -1;
	}
	if (limit > YTMUSIC_TRACKS_MAX) limit = YTMUSIC_TRACKS_MAX;
	const char *request =
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"en\",\"gl\":\"US\"}},\"browseId\":\"FEmusic_charts\"}";
	char *body = NULL;
	if (!youtubei_post("browse", request, false, &body, NULL, error, error_size)) return -1;
	int count = collect_track_rows_compact(body, tracks, limit);

	// Signed in, the page carries the songs chart as rows. Signed out it only
	// links to the video charts, so follow the first playlist it offers and
	// list that chart's tracks instead of reporting nothing.
	if (count <= 0) {
		char playlist[96];
		if (first_chart_playlist(body, playlist, sizeof(playlist))) {
			free(body);
			body = NULL;
			char playlist_request[320];
			snprintf(playlist_request, sizeof(playlist_request),
					 "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\""
					 ",\"hl\":\"en\",\"gl\":\"US\"}},\"browseId\":\"%s\"}",
					 playlist);
			if (youtubei_post("browse", playlist_request, false, &body, NULL, error, error_size)) {
				count = collect_track_rows_compact(body, tracks, limit);
			}
		}
	}
	free(body);
	if (count < 0) {
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return count;
	}
	if (!count) set_error(error, error_size, "ytmusic_chart_tracks_unavailable");
	return count;
}

bool ytmusic_audio_url(const char *video_id, char *url, size_t url_size,
					  int *duration, char *title, size_t title_size,
					  char *artist, size_t artist_size,
					  char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
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
	if (!ytmusic_logged_in()) {
		set_error(error, error_size, "ytmusic_sign_in_required");
		return false;
	}
	char request[1024];
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", video_id);
	char *body = NULL;
	if (!youtubei_post("player", request, true, &body, NULL, error, error_size)) return false;
	json_doc_t doc;
	if (!json_parse_limited(youtubei_json_start(body), &doc, YTMUSIC_PLAYER_TOKEN_LIMIT)) {
		free(body);
		body = NULL;
		goto android_vr_fallback;
	}
	int root = json_root(&doc);
	if (!playable_track_response(&doc, root, video_id, error, error_size)) {
		json_free(&doc);
		free(body);
		return false;
	}
	int details = json_get(&doc, root, "videoDetails");
	if (duration) *duration = (int)json_obj_long(&doc, details, "lengthSeconds", 0);
	if (title && title_size) json_obj_str(&doc, details, "title", title, title_size);
	if (artist && artist_size) json_obj_str(&doc, details, "author", artist, artist_size);
	int formats = json_path(&doc, root, "streamingData.adaptiveFormats");
	int chosen = -1;
	for (int i = 0; i < json_len(&doc, formats); i++) {
		int format = json_at(&doc, formats, i);
		char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
		json_obj_str(&doc, format, "mimeType", mime, sizeof(mime));
		if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
		if (json_obj_str(&doc, format, "url", candidate, sizeof(candidate)) && candidate[0]) {
			chosen = format;
			break;
		}
	}
	bool ok = chosen >= 0 && json_obj_str(&doc, chosen, "url", url, url_size) && url[0];
	json_free(&doc);
	free(body);
	if (ok) return true;

	// Web Music may return ciphered formats, which require running YouTube's
	// current player JavaScript. Avoid embedding a large JS engine in the small
	// player firmware: the Android VR response can provide a direct AAC URL.
android_vr_fallback:
	body = NULL;
	if (!android_vr_player(video_id, &body, error, error_size)) return false;
	if (!json_parse_limited(body, &doc, YTMUSIC_PLAYER_TOKEN_LIMIT)) {
		free(body);
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return false;
	}
	root = json_root(&doc);
	if (!playable_track_response(&doc, root, video_id, error, error_size)) {
		json_free(&doc);
		free(body);
		return false;
	}
	details = json_get(&doc, root, "videoDetails");
	if (duration && !*duration) *duration = (int)json_obj_long(&doc, details, "lengthSeconds", 0);
	if (title && title_size && !title[0]) json_obj_str(&doc, details, "title", title, title_size);
	if (artist && artist_size && !artist[0]) json_obj_str(&doc, details, "author", artist, artist_size);
	formats = json_path(&doc, root, "streamingData.adaptiveFormats");
	ok = false;
	for (int i = 0; i < json_len(&doc, formats); i++) {
		int format = json_at(&doc, formats, i);
		char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
		json_obj_str(&doc, format, "mimeType", mime, sizeof(mime));
		if (strncasecmp(mime, "audio/mp4", 9) == 0 &&
			json_obj_str(&doc, format, "url", candidate, sizeof(candidate)) && candidate[0]) {
			snprintf(url, url_size, "%s", candidate);
			ok = true;
			break;
		}
	}
	json_free(&doc);
	free(body);
	if (!ok) set_error(error, error_size, "ytmusic_aac_stream_unavailable");
	return ok;
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

static void prune_playback_cache(const char *keep_id) {
	DIR *dir = opendir("/tmp/sonix-ytmusic");
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		char *dot = strrchr(entry->d_name, '.');
		if (!dot || (strcmp(dot, ".m4a") != 0 && strcmp(dot, ".tags") != 0 && strcmp(dot, ".part") != 0)) continue;
		if (strncmp(entry->d_name, keep_id, 11) == 0 && entry->d_name[11] == '.') continue;
		char path[640];
		if (snprintf(path, sizeof(path), "/tmp/sonix-ytmusic/%s", entry->d_name) < (int)sizeof(path)) remove(path);
	}
	closedir(dir);
}

bool ytmusic_download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								   char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!track || !path || !path_size || strlen(track->id) != 11) {
		set_error(error, error_size, "ytmusic_invalid_track");
		return false;
	}
	char url[YTMUSIC_URL_MAX], final[512], part[520];
	if (!ytmusic_audio_url(track->id, url, sizeof(url), NULL, NULL, 0, NULL, 0, error, error_size)) return false;
	if (mkdir("/tmp/sonix-ytmusic", 0700) != 0 && errno != EEXIST) {
		set_error(error, error_size, "ytmusic_cache_unavailable");
		return false;
	}
	if (snprintf(final, sizeof(final), "/tmp/sonix-ytmusic/%s.m4a", track->id) >= (int)sizeof(final) ||
		snprintf(part, sizeof(part), "%s.part", final) >= (int)sizeof(part) || strlen(final) >= path_size) {
		set_error(error, error_size, "ytmusic_path_too_long");
		return false;
	}
	prune_playback_cache(track->id);
	remove(part);
	http_stream_t stream;
	http_stream_options_t options = {.no_icy = true, .no_https_downgrade = true};
	if (!http_stream_open_ex(&stream, url, YTMUSIC_TIMEOUT, &options)) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_stream_failed");
		return false;
	}
	FILE *f = fopen(part, "wb");
	if (!f) {
		http_stream_close(&stream);
		set_error(error, error_size, "ytmusic_cache_unavailable");
		return false;
	}
	char *buffer = malloc(32768);
	bool ok = buffer != NULL;
	uint64_t downloaded = 0;
	while (ok) {
		int n = http_stream_read(&stream, buffer, 32768);
		if (n < 0) { ok = false; break; }
		if (n == 0) break;
		if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
		downloaded += (uint64_t)n;
	}
	free(buffer);
	if (fclose(f) != 0) ok = false;
	if (stream.content_length64 && downloaded != stream.content_length64) ok = false;
	http_stream_close(&stream);
	memset(url, 0, sizeof(url));
	if (!ok || downloaded < 1024 || rename(part, final) != 0) {
		remove(part);
		set_error(error, error_size, "ytmusic_download_incomplete");
		return false;
	}
	write_playback_tags(final, track);
	snprintf(path, path_size, "%s", final);
	return true;
}
