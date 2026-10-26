#include "ytmusicpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/ytmusic.h"

#define YTMUSIC_PAGE_RESULTS YTMUSIC_ITEMS_MAX
#define YTMUSIC_LOGIN_QR_SIZE 190
#define YTMUSIC_LOGIN_QUIET 10
#define YTMUSIC_QUERY_MAX 192

// The page is a small tree: the home screen lists what can be opened, the
// Explore screen lists the store's carousels, the search screen takes a query,
// and two shared lists show what came back -- the results screen for a search
// or a chart, the tracks screen for whatever a card opens (a playlist, an
// album, a podcast show). A tap on a card loads its page into the tracks
// screen, so drilling down is one screen at a time and the chevron walks back
// out of it.
lv_obj_t *ytmusic_screen;

static gui_config_t *page_cfg;
static lv_obj_t *home_container;
static lv_obj_t *search_screen;
static lv_obj_t *search_title;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;
static lv_obj_t *results_screen;
static lv_obj_t *results_list;
static lv_obj_t *results_title;
static lv_obj_t *tracks_screen;
static lv_obj_t *tracks_list;
static lv_obj_t *tracks_title;
static lv_obj_t *explore_screen;
static lv_obj_t *explore_list;
static lv_obj_t *settings_screen;
static lv_obj_t *sign_in_row;
static lv_obj_t *sign_out_row;
static lv_obj_t *login_screen;
static lv_obj_t *login_qr;
static lv_obj_t *login_code_label;
static lv_obj_t *login_url_label;
static lv_obj_t *login_message;
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
// One network job at a time: a search, an Explore load, a card's page, or the
// download-and-play of a track. They share the request lock in the system
// layer and the user can only be looking at one of them.
static bool page_busy;
static unsigned list_generation;
static ytmusic_filter_t search_filter;
static ytmusic_item_t result_items[YTMUSIC_PAGE_RESULTS];
static int result_count;
static ytmusic_item_t track_items[YTMUSIC_PAGE_RESULTS];
static int track_count;
static ytmusic_section_t explore_sections[YTMUSIC_SECTIONS_MAX];
static int explore_section_count;
static ytmusic_device_login_t device_login;
static lv_timer_t *login_timer;
static bool login_active;
static bool login_busy;
static int login_elapsed;
static unsigned login_generation;

static void home_refresh_session_rows(void);
static void home_build(void);

// Whether the network is really there. Every row on this page is a request to
// YouTube, and with no IP address each one ends in a generic error that does
// not tell the user what they need to know -- so the notice goes in place of
// the rows, as on the podcast and Tidal pages.
static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine with a network and no wlan0.
	return true;
#else
	if (!wifi_get_enabled()) {
		return false;
	}
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

// The row under the notice: it leads where the problem is fixed.
static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

typedef enum {
	LIST_SEARCH = 0,
	LIST_BROWSE,
} list_kind_t;

// A page of rows on its way back from a worker. `to_tracks` says which of the
// two lists it fills: a card's contents go to the tracks screen, a search or a
// chart to the results screen.
typedef struct {
	list_kind_t kind;
	bool to_tracks;
	unsigned generation;
	ytmusic_filter_t filter;
	char query[YTMUSIC_QUERY_MAX];
	char browse_id[YTMUSIC_BROWSE_ID_MAX];
	char title[96];
	int count;
	char error[192];
	ytmusic_item_t items[YTMUSIC_PAGE_RESULTS];
} list_result_t;

typedef struct {
	unsigned generation;
	int count;
	char error[192];
	ytmusic_section_t sections[YTMUSIC_SECTIONS_MAX];
} explore_result_t;

typedef struct {
	ytmusic_track_t track;
	char path[512];
	char error[192];
	bool ok;
} play_result_t;

typedef struct {
	bool ok;
	bool begin;
	unsigned generation;
	ytmusic_login_state_t state;
	ytmusic_device_login_t login;
	char error[192];
} login_result_t;

static void login_tick_cb(lv_timer_t *timer);
static void list_done(void *data);
static void *list_worker(void *arg);
static void explore_done(void *data);
static void item_clicked_cb(lv_event_t *e);
static void section_clicked_cb(lv_event_t *e);

static bool begin_busy(void) {
	pthread_mutex_lock(&page_lock);
	if (page_busy) {
		pthread_mutex_unlock(&page_lock);
		return false;
	}
	page_busy = true;
	pthread_mutex_unlock(&page_lock);
	return true;
}

static void end_busy(void) {
	pthread_mutex_lock(&page_lock);
	page_busy = false;
	pthread_mutex_unlock(&page_lock);
}

static lv_obj_t *ytmusic_corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
									   lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(ytmusic_screen);
	settingsrow_place_corner_button(button, cfg, slot);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
	lv_obj_center(icon);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	home_refresh_session_rows();
	switch_screen(settings_screen);
}

static void home_refresh_session_rows(void) {
	bool logged_in = ytmusic_logged_in();
	if (sign_in_row) lv_obj_set_hidden(sign_in_row, logged_in);
	if (sign_out_row) lv_obj_set_hidden(sign_out_row, !logged_in);
}

static void login_stop(void) {
	login_active = false;
	login_busy = false;
	login_generation++;
	memset(&device_login, 0, sizeof(device_login));
	if (login_timer) {
		lv_timer_delete(login_timer);
		login_timer = NULL;
	}
}

static void login_result_done(void *data) {
	login_result_t *result = data;
	if (!result) return;
	if (result->generation != login_generation) {
		free(result);
		return;
	}
	login_busy = false;
	if (!login_active) {
		free(result);
		return;
	}
	if (result->begin) {
		if (!result->ok) {
			login_stop();
			toast_error(result->error[0] ? result->error : "ytmusic_login_failed");
			free(result);
			return;
		}
		device_login = result->login;
		login_elapsed = 0;
		lv_label_set_text(login_code_label, device_login.user_code);
		lv_label_set_text(login_url_label, device_login.verification_url);
		lv_label_set_text(login_message, tr("ytmusic_scan_qr"));
		bool qr_ok = login_qr &&
			lv_qrcode_update(login_qr, device_login.verification_url,
							 (uint32_t)strlen(device_login.verification_url)) == LV_RESULT_OK;
		lv_obj_set_hidden(login_qr, !qr_ok);
		lv_obj_set_hidden(login_url_label, qr_ok);
		if (!login_timer) login_timer = lv_timer_create(login_tick_cb, 1000, NULL);
		if (!login_timer) {
			login_stop();
			toast_error("ytmusic_login_failed");
		}
		free(result);
		return;
	}
	if (result->state == YTMUSIC_LOGIN_PENDING) {
		device_login = result->login;
		free(result);
		return;
	}
	if (result->state == YTMUSIC_LOGIN_OK) {
		login_stop();
		home_refresh_session_rows();
		toast_success("ytmusic_signed_in");
		if (lv_screen_active() == login_screen) switch_screen(ytmusic_screen);
		free(result);
		return;
	}
	login_stop();
	toast_error(result->error[0] ? result->error :
		(result->state == YTMUSIC_LOGIN_EXPIRED ? "ytmusic_login_code_expired" : "ytmusic_login_failed"));
	free(result);
}

static void *login_worker(void *arg) {
	login_result_t *result = arg;
	if (result->begin) {
		result->ok = ytmusic_login_device_begin(&result->login, result->error, sizeof(result->error));
	} else {
		result->state = ytmusic_login_device_poll(&result->login, result->error, sizeof(result->error));
		result->ok = result->state == YTMUSIC_LOGIN_OK || result->state == YTMUSIC_LOGIN_PENDING;
	}
	if (!gui_post(login_result_done, result)) {
		if (result->generation == login_generation) login_busy = false;
		free(result);
	}
	return NULL;
}

static bool login_worker_start(bool begin) {
	if (login_busy) return false;
	login_result_t *result = calloc(1, sizeof(*result));
	if (!result) return false;
	result->begin = begin;
	result->generation = login_generation;
	if (!begin) result->login = device_login;
	login_busy = true;
	pthread_t thread;
	if (pthread_create(&thread, NULL, login_worker, result) != 0) {
		login_busy = false;
		free(result);
		return false;
	}
	pthread_detach(thread);
	return true;
}

static void login_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!login_active) return;
	login_elapsed++;
	if (device_login.expires_secs > 0 && login_elapsed >= device_login.expires_secs) {
		login_stop();
		toast_error("ytmusic_login_code_expired");
		return;
	}
	int interval = device_login.interval_secs > 0 ? device_login.interval_secs : 5;
	if (login_elapsed % interval == 0 && !login_busy && !login_worker_start(false)) {
		login_stop();
		toast_error("ytmusic_login_failed");
	}
}

static void login_begin_done_cb(lv_event_t *e) {
	(void)e;
	if (!ytmusic_oauth_configured()) {
		toast_error("ytmusic_oauth_keys_missing");
		return;
	}
	login_stop();
	login_active = true;
	login_elapsed = 0;
	lv_label_set_text(login_message, tr("ytmusic_requesting_code"));
	lv_label_set_text(login_code_label, "");
	lv_obj_set_hidden(login_qr, true);
	lv_obj_set_hidden(login_url_label, true);
	switch_screen(login_screen);
	if (!login_worker_start(true)) {
		login_stop();
		toast_error("ytmusic_login_failed");
	}
}

static void login_screen_unloaded(lv_event_t *e) {
	(void)e;
	login_stop();
}

// ---------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------

// A card for a playlist or album, a note for a song, the podcast glyph for a
// show: the same glyphs the Streaming page uses, so the two pages read alike.
static const lv_image_dsc_t *item_icon(const ytmusic_item_t *item) {
	if (item->kind == YTMUSIC_ITEM_TRACK) return &icon_music_note;
	if (strncmp(item->browse_id, "MPSP", 4) == 0) return &icon_podcast_list;
	return &icon_music2;
}

static theme_semantic_t item_tone(const ytmusic_item_t *item) {
	if (item->kind == YTMUSIC_ITEM_TRACK) return THEME_SEMANTIC_RED;
	if (strncmp(item->browse_id, "MPSP", 4) == 0) return THEME_SEMANTIC_CYAN;
	return THEME_SEMANTIC_PURPLE;
}

static void fill_item_list(lv_obj_t *list, const ytmusic_item_t *items, int count, lv_event_cb_t cb) {
	lv_obj_clean(list);
	if (count <= 0) {
		settingsrow_add(list, "no_results", NULL, NULL, NULL);
		return;
	}
	for (int i = 0; i < count; i++) {
		char label[YTMUSIC_TITLE_MAX + YTMUSIC_NAME_MAX + 8];
		if (items[i].subtitle[0]) {
			snprintf(label, sizeof(label), "%s  ·  %s", items[i].title, items[i].subtitle);
		} else {
			snprintf(label, sizeof(label), "%s", items[i].title);
		}
		lv_obj_t *row = settingsrow_add(list, label, NULL, cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
		settingsrow_add_icon(row, item_icon(&items[i]), item_tone(&items[i]));
	}
}

// ---------------------------------------------------------------------------
// playing a row
// ---------------------------------------------------------------------------

static void playback_done(void *data) {
	play_result_t *result = data;
	if (!result) return;
	end_busy();
	if (result->ok) {
		player_play_file(result->path);
		player_refresh_now_playing();
		player_sheet_open(true);
	} else {
		toast_error(result->error[0] ? result->error : "ytmusic_playback_failed");
	}
	free(result);
}

static void *playback_worker(void *arg) {
	play_result_t *result = arg;
	result->ok = ytmusic_download_for_playback(&result->track, result->path, sizeof(result->path),
											   result->error, sizeof(result->error));
	if (!gui_post(playback_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void play_item(const ytmusic_item_t *item) {
	if (!item || item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) return;
	if (!begin_busy()) return;
	play_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	snprintf(result->track.id, sizeof(result->track.id), "%s", item->id);
	snprintf(result->track.title, sizeof(result->track.title), "%s", item->title);
	// A row's subtitle is the artist, or the show an episode belongs to.
	snprintf(result->track.artist, sizeof(result->track.artist), "%s", item->subtitle);
	pthread_t thread;
	if (pthread_create(&thread, NULL, playback_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error("ytmusic_playback_failed");
		return;
	}
	pthread_detach(thread);
	gui_notify_popup(tr("downloading"));
}

static bool start_list_request(list_kind_t kind, bool to_tracks, ytmusic_filter_t filter,
							   const char *query, const char *browse_id, const char *title) {
	if (!begin_busy()) return false;
	list_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return false;
	}
	result->kind = kind;
	result->to_tracks = to_tracks;
	result->filter = filter;
	if (query) snprintf(result->query, sizeof(result->query), "%s", query);
	if (browse_id) snprintf(result->browse_id, sizeof(result->browse_id), "%s", browse_id);
	snprintf(result->title, sizeof(result->title), "%s", title ? title : "");
	pthread_mutex_lock(&page_lock);
	result->generation = ++list_generation;
	pthread_mutex_unlock(&page_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, list_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error(kind == LIST_SEARCH ? "ytmusic_search_failed" : "ytmusic_browse_failed");
		return false;
	}
	pthread_detach(thread);
	gui_notify_popup(tr("searching"));
	return true;
}

static void *list_worker(void *arg) {
	list_result_t *result = arg;
	if (result->kind == LIST_SEARCH) {
		result->count = ytmusic_search_items(result->query, result->filter, result->items,
											 YTMUSIC_PAGE_RESULTS, result->error, sizeof(result->error));
	} else {
		result->count = ytmusic_browse_items(result->browse_id, result->items,
											 YTMUSIC_PAGE_RESULTS, result->error, sizeof(result->error));
	}
	if (!gui_post(list_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void list_done(void *data) {
	list_result_t *result = data;
	if (!result) return;
	end_busy();
	pthread_mutex_lock(&page_lock);
	bool current = result->generation == list_generation;
	pthread_mutex_unlock(&page_lock);
	if (!current) {
		free(result);
		return;
	}
	if (result->count < 0) {
		toast_error(result->error[0] ? result->error : "ytmusic_search_failed");
		free(result);
		return;
	}
	int count = result->count;
	if (count > YTMUSIC_PAGE_RESULTS) count = YTMUSIC_PAGE_RESULTS;
	if (result->to_tracks) {
		track_count = count;
		memcpy(track_items, result->items, sizeof(track_items[0]) * (size_t)count);
		lv_label_set_text(tracks_title, tr(result->title[0] ? result->title : "ytmusic_browse"));
		fill_item_list(tracks_list, track_items, track_count, item_clicked_cb);
		switch_screen(tracks_screen);
	} else {
		result_count = count;
		memcpy(result_items, result->items, sizeof(result_items[0]) * (size_t)count);
		lv_label_set_text(results_title, tr(result->title[0] ? result->title : "ytmusic_search_results"));
		fill_item_list(results_list, result_items, result_count, item_clicked_cb);
		switch_screen(results_screen);
	}
	free(result);
}

// A tap in either list: a song plays, a card opens its own page in the tracks
// list. Both lists keep their rows in an array the handler indexes.
static void item_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	const ytmusic_item_t *item = NULL;
	if (lv_obj_get_parent(lv_event_get_target(e)) == results_list) {
		if (index < 0 || index >= result_count) return;
		item = &result_items[index];
	} else {
		if (index < 0 || index >= track_count) return;
		item = &track_items[index];
	}
	if (item->kind == YTMUSIC_ITEM_CONTAINER) {
		start_list_request(LIST_BROWSE, true, YTMUSIC_FILTER_SONGS, NULL, item->browse_id, item->title);
	} else {
		play_item(item);
	}
}

// ---------------------------------------------------------------------------
// explore
// ---------------------------------------------------------------------------

static void *explore_worker(void *arg) {
	explore_result_t *result = arg;
	result->count = ytmusic_explore_sections(result->sections, YTMUSIC_SECTIONS_MAX,
											  result->error, sizeof(result->error));
	if (!gui_post(explore_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void explore_done(void *data) {
	explore_result_t *result = data;
	if (!result) return;
	end_busy();
	pthread_mutex_lock(&page_lock);
	bool current = result->generation == list_generation;
	pthread_mutex_unlock(&page_lock);
	if (!current) {
		free(result);
		return;
	}
	if (result->count <= 0) {
		toast_error(result->error[0] ? result->error : "ytmusic_explore_empty");
		free(result);
		return;
	}
	explore_section_count = result->count;
	memcpy(explore_sections, result->sections, sizeof(explore_sections[0]) * (size_t)explore_section_count);
	lv_obj_clean(explore_list);
	for (int i = 0; i < explore_section_count; i++) {
		lv_obj_t *value = NULL;
		lv_obj_t *row = settingsrow_add(explore_list,
			explore_sections[i].title[0] ? explore_sections[i].title : "ytmusic_explore",
			&value, section_clicked_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
		settingsrow_add_icon(row, &icon_music2, THEME_SEMANTIC_PURPLE);
		if (value) {
			char text[16];
			snprintf(text, sizeof(text), "%d", explore_sections[i].count);
			lv_label_set_text(value, text);
		}
	}
	switch_screen(explore_screen);
	free(result);
}

static void explore_start_cb(lv_event_t *e) {
	(void)e;
	if (!begin_busy()) return;
	explore_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	pthread_mutex_lock(&page_lock);
	result->generation = ++list_generation;
	pthread_mutex_unlock(&page_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, explore_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error("ytmusic_explore_failed");
		return;
	}
	pthread_detach(thread);
	gui_notify_popup(tr("searching"));
}

// One carousel, listed with the cards it holds. Its items came with the page,
// so this opens without another request.
static void section_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= explore_section_count) return;
	const ytmusic_section_t *section = &explore_sections[index];
	result_count = section->count;
	memcpy(result_items, section->items, sizeof(result_items[0]) * (size_t)result_count);
	lv_label_set_text(results_title, tr(section->title[0] ? section->title : "ytmusic_explore"));
	fill_item_list(results_list, result_items, result_count, item_clicked_cb);
	switch_screen(results_screen);
}

// ---------------------------------------------------------------------------
// home
// ---------------------------------------------------------------------------

static void show_search(ytmusic_filter_t filter) {
	search_filter = filter;
	lv_label_set_text(search_title, tr(filter == YTMUSIC_FILTER_PODCASTS ? "ytmusic_podcasts" : "ytmusic_search"));
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	switch_screen(search_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) return;
	char query[YTMUSIC_QUERY_MAX];
	snprintf(query, sizeof(query), "%s", text);
	start_list_request(LIST_SEARCH, false, search_filter, query, NULL, "ytmusic_search_results");
}

static void search_field_cb(lv_event_t *e) {
	(void)e;
	keyboard_set_field(search_keyboard, search_field);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	show_search(YTMUSIC_FILTER_SONGS);
}

static void podcasts_cb(lv_event_t *e) {
	(void)e;
	show_search(YTMUSIC_FILTER_PODCASTS);
}

// "Top songs" is the charts page: the store answers it with the chart rows and
// with the playlists that hold them, and both are worth showing.
static void charts_cb(lv_event_t *e) {
	(void)e;
	start_list_request(LIST_BROWSE, false, YTMUSIC_FILTER_SONGS, NULL, "FEmusic_charts", "ytmusic_charts");
}

// The liked songs are a playlist the account owns, so this one needs a session.
static void favourites_cb(lv_event_t *e) {
	(void)e;
	if (!ytmusic_logged_in()) {
		toast_error("ytmusic_sign_in_for_favourites");
		return;
	}
	start_list_request(LIST_BROWSE, false, YTMUSIC_FILTER_SONGS, NULL, "VLLM", "ytmusic_favourites");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	if (ytmusic_logout()) {
		home_refresh_session_rows();
		toast_success("ytmusic_signed_out");
	}
	else toast_error("ytmusic_session_remove_failed");
}

// The session file is the only sign-in InnerTube accepts, so it has a row of
// its own rather than being a hidden feature of the card.
static void import_session_cb(lv_event_t *e) {
	(void)e;
	char error[192] = "";
	if (ytmusic_import_session_file(page_cfg ? page_cfg->sd_root_path : NULL, error, sizeof(error))) {
		home_refresh_session_rows();
		toast_success("ytmusic_session_imported");
	} else {
		toast_error(error[0] ? error : "ytmusic_session_file_missing");
	}
}

static void build_search_page(void) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(search_screen, false);
	search_title = settingsrow_title(search_screen, page_cfg, "ytmusic_search");
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, page_cfg->screen_width - 2 * page_cfg->padding, 62);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, page_cfg->padding, settingsrow_content_top(page_cfg));
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_cb, LV_EVENT_CLICKED, NULL);
	search_keyboard = keyboard_create(search_screen, page_cfg->screen_width,
		page_cfg->screen_width < 320 ? 144 : 316, search_field, NULL, "ok", search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);

	results_screen = lv_obj_create(NULL);
	lv_obj_add_style(results_screen, &theme_style_screen, 0);
	results_list = settingsrow_page(results_screen, page_cfg, "YouTube Music");
	results_title = settingsrow_page_title(results_screen);
	player_sheet_attach_drag(results_list, true);
	switcher_attach_back_gesture(results_screen);

	tracks_screen = lv_obj_create(NULL);
	lv_obj_add_style(tracks_screen, &theme_style_screen, 0);
	tracks_list = settingsrow_page(tracks_screen, page_cfg, "YouTube Music");
	tracks_title = settingsrow_page_title(tracks_screen);
	player_sheet_attach_drag(tracks_list, true);
	switcher_attach_back_gesture(tracks_screen);

	explore_screen = lv_obj_create(NULL);
	lv_obj_add_style(explore_screen, &theme_style_screen, 0);
	explore_list = settingsrow_page(explore_screen, page_cfg, "ytmusic_explore");
	player_sheet_attach_drag(explore_list, true);
	switcher_attach_back_gesture(explore_screen);
}

static void build_login_page(void) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(login_screen, false);
	settingsrow_title(login_screen, page_cfg, "ytmusic_sign_in");

	lv_obj_t *box = lv_obj_create(login_screen);
	int top = settingsrow_content_top(page_cfg);
	lv_obj_set_size(box, page_cfg->screen_width - 2 * page_cfg->padding,
			 page_cfg->screen_height - top - page_cfg->padding);
	lv_obj_align(box, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, page_cfg->padding, 0);
	lv_obj_set_style_pad_row(box, 10, 0);
	lv_obj_set_scroll_dir(box, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	login_qr = lv_qrcode_create(box);
	// As wide as the box's inner width leaves room for, quiet zone included.
	// A fixed 190 plus its border was wider than a 240-pixel screen's box and
	// was cut off at both sides.
	int qr_size = page_cfg->screen_width - 4 * page_cfg->padding - 2 * YTMUSIC_LOGIN_QUIET;
	if (qr_size > YTMUSIC_LOGIN_QR_SIZE) qr_size = YTMUSIC_LOGIN_QR_SIZE;
	if (qr_size < 80) qr_size = 80;
	lv_qrcode_set_size(login_qr, qr_size);
	lv_qrcode_set_dark_color(login_qr, lv_color_black());
	lv_qrcode_set_light_color(login_qr, lv_color_white());
	lv_obj_set_style_border_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_border_width(login_qr, YTMUSIC_LOGIN_QUIET, 0);
	lv_obj_set_style_bg_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(login_qr, LV_OPA_COVER, 0);
	lv_obj_set_hidden(login_qr, true);

	login_url_label = lv_label_create(box);
	lv_label_set_long_mode(login_url_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_url_label, lv_pct(100));
	lv_obj_set_style_text_align(login_url_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(login_url_label, &font_ui_20, 0);
	lv_label_set_text(login_url_label, "");
	lv_obj_set_hidden(login_url_label, true);

	// The code has to be readable and whole: at 36 px bold with 5 px of letter
	// spacing a device code is wider than the screen, and CLIP cut the first
	// and last characters off -- exactly the ones that have to be typed in.
	login_code_label = lv_label_create(box);
	lv_label_set_long_mode(login_code_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_code_label, lv_pct(100));
	lv_obj_set_style_text_align(login_code_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_code_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_code_label, &font_ui_28, 0);
	lv_obj_set_style_text_letter_space(login_code_label, 3, 0);
	lv_label_set_text(login_code_label, "");

	login_message = lv_label_create(box);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, lv_pct(100));
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_label_set_text(login_message, tr("ytmusic_requesting_code"));

	lv_obj_add_event_cb(login_screen, login_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(login_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	settings_screen = lv_obj_create(NULL);
	lv_obj_add_style(settings_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "ytmusic_settings");
	sign_in_row = settingsrow_action(container, "ytmusic_sign_in", login_begin_done_cb, NULL);
	settingsrow_add_icon(sign_in_row, &icon_import, THEME_SEMANTIC_CYAN);
	sign_out_row = settingsrow_action(container, "ytmusic_sign_out", logout_cb, NULL);
	settingsrow_add_icon(sign_out_row, &icon_log_out, THEME_SEMANTIC_RED);
	lv_obj_t *import_row = settingsrow_action(container, "ytmusic_import_session", import_session_cb, NULL);
	settingsrow_add_icon(import_row, &icon_import, THEME_SEMANTIC_PURPLE);
	settingsrow_add_caption(import_row, "ytmusic_import_session_note");
	home_refresh_session_rows();
	player_sheet_attach_drag(container, true);
	switcher_attach_back_gesture(settings_screen);
}

// The rows, or the no-network notice in their place. Rebuilt on every entry
// into the page, because the Wi-Fi can have been turned on or off while the
// user was in the settings that did it.
static void home_build(void) {
	if (!home_container) {
		return;
	}
	lv_obj_clean(home_container);

	if (!wifi_is_connected()) {
		// The same notice as the podcast and Tidal pages: what is missing, on
		// the page that needs it, with the row that leads where it is fixed.
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0); // normal colour: an instruction
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_hor(note, 4, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	lv_obj_t *row = settingsrow_add(home_container, "ytmusic_search", NULL, search_cb, NULL);
	settingsrow_add_icon(row, &icon_search, THEME_SEMANTIC_CYAN);
	row = settingsrow_add(home_container, "ytmusic_explore", NULL, explore_start_cb, NULL);
	settingsrow_add_icon(row, &icon_music2, THEME_SEMANTIC_PURPLE);
	row = settingsrow_add(home_container, "ytmusic_charts", NULL, charts_cb, NULL);
	settingsrow_add_icon(row, &icon_music2, THEME_SEMANTIC_RED);
	row = settingsrow_add(home_container, "ytmusic_favourites", NULL, favourites_cb, NULL);
	settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_RED);
	row = settingsrow_add(home_container, "ytmusic_podcasts", NULL, podcasts_cb, NULL);
	settingsrow_add_icon(row, &icon_podcast_list, THEME_SEMANTIC_CYAN);
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	home_build();
}

void ytmusicpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	ytmusic_screen = lv_obj_create(NULL);
	lv_obj_add_style(ytmusic_screen, &theme_style_screen, 0);
	home_container = settingsrow_page(ytmusic_screen, cfg, "YouTube Music");
	settingsrow_title_corner_slots(settingsrow_page_title(ytmusic_screen), cfg, 1);
	ytmusic_corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	home_build();
	lv_obj_add_event_cb(ytmusic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	player_sheet_attach_drag(home_container, true);
	build_search_page();
	build_login_page();
	build_settings_page(cfg);
	switcher_attach_back_gesture(ytmusic_screen);
}
