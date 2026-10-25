#ifndef YTMUSIC_H
#define YTMUSIC_H

#include <stdbool.h>
#include <stddef.h>

#define YTMUSIC_TRACKS_MAX 50
#define YTMUSIC_ID_MAX 16
#define YTMUSIC_TITLE_MAX 200
#define YTMUSIC_NAME_MAX 128
#define YTMUSIC_URL_MAX 2048

typedef struct {
	char id[YTMUSIC_ID_MAX];
	char title[YTMUSIC_TITLE_MAX];
	char artist[YTMUSIC_NAME_MAX];
	char album[YTMUSIC_TITLE_MAX];
	char thumbnail[YTMUSIC_URL_MAX];
	int duration;
} ytmusic_track_t;

typedef struct {
	char device_code[512];
	char user_code[32];
	char verification_url[256];
	int interval_secs;
	int expires_secs;
} ytmusic_device_login_t;

typedef enum {
	YTMUSIC_LOGIN_PENDING = 0,
	YTMUSIC_LOGIN_OK,
	YTMUSIC_LOGIN_EXPIRED,
	YTMUSIC_LOGIN_ERROR,
} ytmusic_login_state_t;

// Call after config_init() and streamkeys_init(). OAuth tokens or the optional
// imported cookie are stored in a private sidecar beside device_config.ini,
// outside config.c's small per-value limit, and survive player reboots.
void ytmusic_init(void);
bool ytmusic_logged_in(void);
bool ytmusic_oauth_configured(void);
bool ytmusic_login_device_begin(ytmusic_device_login_t *login, char *error, size_t error_size);
ytmusic_login_state_t ytmusic_login_device_poll(ytmusic_device_login_t *login,
											 char *error, size_t error_size);
bool ytmusic_set_session_cookie(const char *cookie, char *error, size_t error_size);
// One-time import from <card_root>/ytmusic_session.txt. The file contains a
// single Cookie header value exported by a trusted YouTube Music sign-in
// helper. Successful import removes the card copy after saving it internally.
bool ytmusic_import_session_file(const char *card_root, char *error, size_t error_size);
void ytmusic_session_cookie(char *out, size_t size);
bool ytmusic_logout(void);

// Blocking network operations. Call only from a service worker, never from
// the LVGL or audio thread.
int ytmusic_search_tracks(const char *query, ytmusic_track_t *tracks, int limit,
						  char *error, size_t error_size);
int ytmusic_browse_chart_tracks(ytmusic_track_t *tracks, int limit,
								char *error, size_t error_size);
bool ytmusic_audio_url(const char *video_id, char *url, size_t url_size,
					  int *duration, char *title, size_t title_size,
					  char *artist, size_t artist_size,
					  char *error, size_t error_size);

// Fetches one AAC/MP4 audio stream to the temporary playback cache. This is a
// blocking operation; call from a worker, not from the UI or audio thread.
bool ytmusic_download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								   char *error, size_t error_size);

#endif /* YTMUSIC_H */
