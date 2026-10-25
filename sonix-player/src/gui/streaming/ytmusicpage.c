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
#include "src/system/core/lang.h"
#include "src/system/streaming/ytmusic.h"

#define YTMUSIC_PAGE_RESULTS 30
#define YTMUSIC_LOGIN_QR_SIZE 190
#define YTMUSIC_LOGIN_QUIET 10

lv_obj_t *ytmusic_screen;

static gui_config_t *page_cfg;
static lv_obj_t *search_screen;
static lv_obj_t *results_screen;
static lv_obj_t *results_list;
static lv_obj_t *results_title;
static lv_obj_t *search_field;
static lv_obj_t *sign_in_row;
static lv_obj_t *signed_in_row;
static lv_obj_t *sign_out_row;
static lv_obj_t *login_screen;
static lv_obj_t *login_qr;
static lv_obj_t *login_code_label;
static lv_obj_t *login_url_label;
static lv_obj_t *login_message;
static keyboard_t *search_keyboard;
static pthread_mutex_t search_lock = PTHREAD_MUTEX_INITIALIZER;
static bool search_busy;
static ytmusic_track_t displayed_tracks[YTMUSIC_PAGE_RESULTS];
static int displayed_track_count;
static ytmusic_device_login_t device_login;
static lv_timer_t *login_timer;
static bool login_active;
static bool login_busy;
static int login_elapsed;
static unsigned login_generation;

typedef struct {
	int count;
	char title[64];
	ytmusic_track_t tracks[YTMUSIC_PAGE_RESULTS];
	char error[192];
} search_result_t;

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

static void home_refresh_session_rows(void) {
	bool logged_in = ytmusic_logged_in();
	if (sign_in_row) lv_obj_set_hidden(sign_in_row, logged_in);
	if (signed_in_row) lv_obj_set_hidden(signed_in_row, !logged_in);
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
		lv_label_set_text(login_message, "Scan the QR code, or enter this code on Google.");
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
		toast_error("Add [ytmusic] client_id and client_secret to streaming-keys.ini, then restart.");
		return;
	}
	login_stop();
	login_active = true;
	login_elapsed = 0;
	lv_label_set_text(login_message, "Requesting a Google sign-in code…");
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

static void playback_done(void *data) {
	play_result_t *result = data;
	if (!result) return;
	pthread_mutex_lock(&search_lock);
	search_busy = false;
	pthread_mutex_unlock(&search_lock);
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
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
		free(result);
	}
	return NULL;
}

static void play_track_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= displayed_track_count) return;
	pthread_mutex_lock(&search_lock);
	if (search_busy) {
		pthread_mutex_unlock(&search_lock);
		return;
	}
	play_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		pthread_mutex_unlock(&search_lock);
		toast_error("ytmusic_out_of_memory");
		return;
	}
	result->track = displayed_tracks[index];
	search_busy = true;
	pthread_mutex_unlock(&search_lock);
	gui_notify_popup(tr("downloading"));
	pthread_t thread;
	if (pthread_create(&thread, NULL, playback_worker, result) != 0) {
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
		free(result);
		toast_error("ytmusic_playback_failed");
		return;
	}
	pthread_detach(thread);
}

static void show_search(void) {
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	switch_screen(search_screen);
}

static void search_done(void *data) {
	search_result_t *result = data;
	if (!result) return;
	pthread_mutex_lock(&search_lock);
	search_busy = false;
	pthread_mutex_unlock(&search_lock);
	if (result->count < 0) {
		toast_error(result->error[0] ? result->error : "ytmusic_search_failed");
		free(result);
		return;
	}
	lv_obj_clean(results_list);
	lv_label_set_text(results_title, tr(result->title[0] ? result->title : "YouTube Music"));
	displayed_track_count = result->count < 0 ? 0 : result->count;
	if (displayed_track_count > YTMUSIC_PAGE_RESULTS) displayed_track_count = YTMUSIC_PAGE_RESULTS;
	if (displayed_track_count) memcpy(displayed_tracks, result->tracks,
								  sizeof(displayed_tracks[0]) * (size_t)displayed_track_count);
	if (!result->count) {
		settingsrow_add(results_list, "no_results", NULL, NULL, NULL);
	} else {
		for (int i = 0; i < result->count; i++) {
			char label[YTMUSIC_TITLE_MAX + YTMUSIC_NAME_MAX + 8];
			snprintf(label, sizeof(label), "%s  ·  %s", result->tracks[i].title,
					 result->tracks[i].artist[0] ? result->tracks[i].artist : "YouTube Music");
			lv_obj_t *row = settingsrow_add(results_list, label, NULL, play_track_cb, (void *)(intptr_t)i);
			settingsrow_name_lines(row, 2);
			settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_RED);
		}
	}
	switch_screen(results_screen);
	free(result);
}

static void *search_worker(void *arg) {
	char *query = arg;
	search_result_t *result = calloc(1, sizeof(*result));
	if (result) {
		snprintf(result->title, sizeof(result->title), "%s", "Search results");
		result->count = ytmusic_search_tracks(query, result->tracks, YTMUSIC_PAGE_RESULTS,
												 result->error, sizeof(result->error));
		if (!gui_post(search_done, result)) {
			free(result);
			pthread_mutex_lock(&search_lock);
			search_busy = false;
			pthread_mutex_unlock(&search_lock);
		}
	} else {
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
	}
	memset(query, 0, strlen(query));
	free(query);
	return NULL;
}

static void *charts_worker(void *arg) {
	(void)arg;
	search_result_t *result = calloc(1, sizeof(*result));
	if (result) {
		snprintf(result->title, sizeof(result->title), "%s", "Top Songs Charts");
		result->count = ytmusic_browse_chart_tracks(result->tracks, YTMUSIC_PAGE_RESULTS,
													 result->error, sizeof(result->error));
		if (!gui_post(search_done, result)) {
			free(result);
			pthread_mutex_lock(&search_lock);
			search_busy = false;
			pthread_mutex_unlock(&search_lock);
		}
	} else {
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
	}
	return NULL;
}

static void charts_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&search_lock);
	if (search_busy) {
		pthread_mutex_unlock(&search_lock);
		return;
	}
	search_busy = true;
	pthread_mutex_unlock(&search_lock);
	gui_notify_popup(tr("searching"));
	pthread_t thread;
	if (pthread_create(&thread, NULL, charts_worker, NULL) != 0) {
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
		toast_error("ytmusic_browse_failed");
		return;
	}
	pthread_detach(thread);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&search_lock);
	if (search_busy) {
		pthread_mutex_unlock(&search_lock);
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		pthread_mutex_unlock(&search_lock);
		return;
	}
	char *query = strdup(text);
	if (!query) {
		pthread_mutex_unlock(&search_lock);
		toast_error("ytmusic_out_of_memory");
		return;
	}
	search_busy = true;
	pthread_mutex_unlock(&search_lock);
	gui_notify_popup(tr("searching"));
	pthread_t thread;
	if (pthread_create(&thread, NULL, search_worker, query) != 0) {
		pthread_mutex_lock(&search_lock);
		search_busy = false;
		pthread_mutex_unlock(&search_lock);
		free(query);
		toast_error("ytmusic_search_failed");
		return;
	}
	pthread_detach(thread);
}

static void search_field_cb(lv_event_t *e) {
	(void)e;
	keyboard_set_field(search_keyboard, search_field);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	show_search();
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	if (ytmusic_logout()) {
		home_refresh_session_rows();
		toast_success("ytmusic_signed_out");
	}
	else toast_error("ytmusic_session_remove_failed");
}

static void build_search_page(void) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(search_screen, false);
	settingsrow_title(search_screen, page_cfg, "Search YouTube Music");
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
}

static void build_login_page(void) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(login_screen, false);
	settingsrow_title(login_screen, page_cfg, "Sign in with Google");

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
	lv_qrcode_set_size(login_qr, YTMUSIC_LOGIN_QR_SIZE);
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

	login_code_label = lv_label_create(box);
	lv_label_set_long_mode(login_code_label, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(login_code_label, lv_pct(100));
	lv_obj_set_style_text_align(login_code_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_code_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_code_label, &font_ui_36_bold, 0);
	lv_obj_set_style_text_letter_space(login_code_label, 5, 0);
	lv_label_set_text(login_code_label, "");

	login_message = lv_label_create(box);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, lv_pct(100));
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_label_set_text(login_message, "Requesting sign-in code…");

	lv_obj_add_event_cb(login_screen, login_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(login_screen);
}

void ytmusicpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	ytmusic_screen = lv_obj_create(NULL);
	lv_obj_add_style(ytmusic_screen, &theme_style_screen, 0);
	lv_obj_t *home = settingsrow_page(ytmusic_screen, cfg, "YouTube Music");
	lv_obj_t *row = settingsrow_add(home, "Search YouTube Music", NULL, search_cb, NULL);
	settingsrow_add_icon(row, &icon_search, THEME_SEMANTIC_CYAN);
	row = settingsrow_add(home, "Top Songs Charts", NULL, charts_cb, NULL);
	settingsrow_add_icon(row, &icon_music2, THEME_SEMANTIC_RED);
	sign_in_row = settingsrow_action(home, "Sign in with Google", login_begin_done_cb, NULL);
	row = sign_in_row;
	settingsrow_add_icon(row, &icon_import, THEME_SEMANTIC_CYAN);
	signed_in_row = settingsrow_action(home, "Signed in with Google", NULL, NULL);
	settingsrow_add_icon(signed_in_row, &icon_check, THEME_SEMANTIC_GREEN);
	sign_out_row = settingsrow_action(home, "Sign out of YouTube Music", logout_cb, NULL);
	settingsrow_add_icon(sign_out_row, &icon_log_out, THEME_SEMANTIC_RED);
	home_refresh_session_rows();
	player_sheet_attach_drag(home, true);
	build_search_page();
	build_login_page();
	switcher_attach_back_gesture(ytmusic_screen);
}
