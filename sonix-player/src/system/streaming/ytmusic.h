#ifndef YTMUSIC_H
#define YTMUSIC_H

#include <stdbool.h>
#include <stddef.h>

#define YTMUSIC_TRACKS_MAX 50
#define YTMUSIC_ID_MAX 16
#define YTMUSIC_TITLE_MAX 200
#define YTMUSIC_NAME_MAX 128
#define YTMUSIC_URL_MAX 2048
#define YTMUSIC_BROWSE_ID_MAX 128
#define YTMUSIC_ITEMS_MAX 24
#define YTMUSIC_SECTIONS_MAX 6
#define YTMUSIC_SECTION_ITEMS_MAX 12

typedef struct {
	char id[YTMUSIC_ID_MAX];
	char title[YTMUSIC_TITLE_MAX];
	char artist[YTMUSIC_NAME_MAX];
	char album[YTMUSIC_TITLE_MAX];
	char thumbnail[YTMUSIC_URL_MAX];
	int duration;
} ytmusic_track_t;

// One row of a list: either something that plays (a song, a podcast episode),
// or something that opens another list (a playlist, an album, a podcast show).
//
// YouTube Music answers every one of those from the same page shape, so the
// page keeps one kind of row and lets the id decide what a tap does. The two
// ids are mutually exclusive: a track has the eleven-character video id, a
// container the browse id ("VL..." playlist, "MPRE..." album, "MPSP..." show).
typedef enum {
	YTMUSIC_ITEM_TRACK = 0,
	YTMUSIC_ITEM_CONTAINER,
} ytmusic_item_kind_t;

typedef struct {
	ytmusic_item_kind_t kind;
	char id[YTMUSIC_ID_MAX];
	char browse_id[YTMUSIC_BROWSE_ID_MAX];
	char title[YTMUSIC_TITLE_MAX];
	char subtitle[YTMUSIC_NAME_MAX];
	int duration;
} ytmusic_item_t;

// A titled row of the Explore page -- "New albums &amp; singles", "Trending" --
// with the handful of cards under it. The cards are loaded with the page rather
// than on the tap, so opening a row costs no network round trip.
typedef struct {
	char title[64];
	int count;
	ytmusic_item_t items[YTMUSIC_SECTION_ITEMS_MAX];
} ytmusic_section_t;

// Which part of the catalogue a search asks for. Songs is the default; Podcasts
// returns shows, which open like any other container.
typedef enum {
	YTMUSIC_FILTER_SONGS = 0,
	YTMUSIC_FILTER_PODCASTS,
} ytmusic_filter_t;

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
//
// All three fill `items` and return how many there are, 0 for an honest empty
// page, or -1 with a tag in `error`. Public pages -- search, charts, Explore --
// still answer when nobody is signed in, and an account whose session Google
// refuses falls back to that anonymous answer instead of failing.
int ytmusic_search_items(const char *query, ytmusic_filter_t filter,
						 ytmusic_item_t *items, int limit, char *error, size_t error_size);
// A playlist ("VL..."), an album ("MPRE..."), a podcast show ("MPSP..."), the
// charts ("FEmusic_charts"), the liked songs ("VLLM"), or anything else whose
// page carries rows.
int ytmusic_browse_items(const char *browse_id, ytmusic_item_t *items, int limit,
						 char *error, size_t error_size);
// Explore: the carousels of the store's front page, each with a few cards.
int ytmusic_explore_sections(ytmusic_section_t *sections, int limit,
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
