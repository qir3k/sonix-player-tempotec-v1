#include "coversearch.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/albumart.h"
#include "src/system/library/coverart.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/device_state.h"

lv_obj_t *coversearch_screen;

#define ROW_HEIGHT bp_pick(56, 92)
#define ROW_RADIUS bp_pick(10, 12)
#define APPLY_W bp_pick(140, 230)
#define APPLY_H bp_pick(40, 64)

static lv_obj_t *title_label;
static lv_obj_t *list;	  // the albums
static lv_obj_t *preview; // one picture
static lv_obj_t *preview_image;
static lv_obj_t *apply_bar; // Apply, under the picture

static char file_path[1024];
static coverart_hit_t *hits;
static int hit_count;
static int previewing = -1; // the hit whose picture is up, -1 on the list

// The bytes of the picture up, for Apply to write; and the pixels shown.
static uint8_t *picture;
static size_t picture_len;
static cover_image_t shown;

// One search or one picture at a time. `generation` moves on whenever the page
// is left or the answer is no longer wanted, so a thread that finishes late
// finds out and puts its work away instead of showing it.
static bool busy;
static int generation;
static int picture_side; // the square the picture is drawn in

static void free_picture(void) {
	free(picture);
	picture = NULL;
	picture_len = 0;
	lv_image_set_src(preview_image, NULL);
	cover_free(&shown);
}

static void show_list(void) {
	previewing = -1;
	generation++;
	free_picture();
	lv_label_set_text(title_label, tr("coverart_results"));
	lv_obj_set_hidden(preview, true);
	lv_obj_set_hidden(apply_bar, true);
	lv_obj_set_hidden(list, false);
}

// The chevron and the swipe-back from the picture go back to the list.
static bool back_guard(void) {
	if (previewing < 0) {
		return false;
	}
	show_list();
	return true;
}

static bool back_peek(void) { return previewing >= 0; }

// ---------------------------------------------------------------------------
// One picture: downloaded and decoded off the interface thread
// ---------------------------------------------------------------------------

typedef struct {
	int generation;
	int index;
	char url[320];
	int side;
	uint8_t *data;
	size_t len;
	cover_image_t image;
	bool ok;
} picture_job_t;

static void picture_ready(void *user) {
	picture_job_t *job = user;
	busy = false;
	toast_busy_end();
	if (job->generation != generation) {
		goto discard;
	}
	if (!job->ok) {
		toast_error("coverart_download_failed");
		goto discard;
	}

	free_picture();
	picture = job->data;
	picture_len = job->len;
	job->data = NULL;
	shown = job->image;
	memset(&job->image, 0, sizeof(job->image));
	previewing = job->index;
	lv_image_set_src(preview_image, &shown.dsc);
	lv_label_set_text(title_label, hits[job->index].album);
	lv_obj_set_hidden(list, true);
	lv_obj_set_hidden(preview, false);
	lv_obj_set_hidden(apply_bar, false);

discard:
	free(job->data);
	cover_free(&job->image);
	free(job);
}

static void *picture_worker(void *arg) {
	picture_job_t *job = arg;
	job->ok = coverart_fetch(job->url, &job->data, &job->len) &&
			  cover_load_image_memory(job->data, job->len, job->side, job->side, COVER_FIT_CONTAIN, &job->image);
	if (!gui_post(picture_ready, job)) {
		free(job->data);
		cover_free(&job->image);
		free(job);
	}
	return NULL;
}

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (busy) {
		toast_busy_dismissable("coverart_loading");
		return;
	}
	if (index < 0 || index >= hit_count) {
		return;
	}
	picture_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = generation;
	job->index = index;
	snprintf(job->url, sizeof(job->url), "%s", hits[index].url);
	job->side = picture_side;

	pthread_t thread;
	if (pthread_create(&thread, NULL, picture_worker, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
	busy = true;
	toast_busy_dismissable("coverart_loading");
}

static void apply_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || previewing < 0 || !picture) {
		return;
	}
	if (!coverart_save(file_path, picture, picture_len)) {
		toast_error("coverart_not_saved");
		return;
	}
	show_list();
	back_btn_cb(NULL); // back into the player
	player_refresh_now_playing();
	toast_success("coverart_saved");
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

static void add_row(int index) {
	const coverart_hit_t *h = &hits[index];

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, bp_pick(10, 16), 0);
	lv_obj_set_style_pad_ver(row, bp_pick(6, 10), 0);
	lv_obj_set_style_pad_column(row, bp_pick(8, 12), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Album over artist, with the year on the right.
	lv_obj_t *names = lv_obj_create(row);
	lv_obj_remove_style_all(names);
	lv_obj_set_height(names, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(names, 1);
	lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(names, bp_pick(2, 4), 0);
	lv_obj_set_scrollable(names, false);
	lv_obj_set_event_bubble(names, true);

	lv_obj_t *album = lv_label_create(names);
	lv_label_set_text(album, h->album[0] ? h->album : "-");
	lv_label_set_long_mode(album, LV_LABEL_LONG_DOT);
	lv_obj_set_width(album, lv_pct(100));
	lv_obj_set_height(album, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(album, &theme_style_text, 0);
	lv_obj_set_style_text_font(album, &font_ui_24, 0);

	lv_obj_t *artist = lv_label_create(names);
	lv_label_set_text(artist, h->artist);
	lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
	lv_obj_set_width(artist, lv_pct(100));
	lv_obj_set_height(artist, lv_font_get_line_height(&font_ui_20));
	lv_obj_add_style(artist, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(artist, &font_ui_20, 0);

	lv_obj_t *year = lv_label_create(row);
	lv_label_set_text(year, h->year);
	lv_obj_add_style(year, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(year, &font_ui_20, 0);
	lv_obj_set_event_bubble(year, true);
}

static void open_list(coverart_hit_t *hits_in, int count) {
	free(hits);
	hits = hits_in;
	hit_count = count;

	lv_obj_clean(list);
	for (int i = 0; i < hit_count; i++) {
		add_row(i);
	}
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
	show_list();

	player_sheet_close(false);
	switch_screen(coversearch_screen);
	switcher_set_player_return(coversearch_screen);
}

// Left, whichever way: the albums and the picture go.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	show_list();
	lv_obj_clean(list);
	free(hits);
	hits = NULL;
	hit_count = 0;
}

// ---------------------------------------------------------------------------
// Wi-Fi switched on for a search
//
// With Wi-Fi off the question offers to switch it on instead of searching; the
// search then starts by itself once the link has an address, for the file that
// was playing when the question was answered.
// ---------------------------------------------------------------------------

#define WIFI_POLL_MS 500
#define WIFI_WAIT_MS 30000

static lv_timer_t *wifi_timer;
static uint32_t wifi_since;
static char wifi_path[1024];

static void search_run(void);

static bool wifi_online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0] != '\0';
}

static void wifi_wait_end(void) {
	if (wifi_timer) {
		lv_timer_delete(wifi_timer);
		wifi_timer = NULL;
	}
}

static void wifi_tick_cb(lv_timer_t *t) {
	(void)t;
	device_state_t state;
	device_state_get(&state);
	// Another track, or Wi-Fi switched off again: the search is no longer wanted.
	if (!wifi_get_enabled() || strcmp(state.current_file, wifi_path) != 0) {
		wifi_wait_end();
		toast_busy_end();
		return;
	}
	if (wifi_online()) {
		wifi_wait_end();
		toast_busy_end();
		search_run();
		return;
	}
	if (lv_tick_elaps(wifi_since) >= WIFI_WAIT_MS) {
		wifi_wait_end();
		toast_busy_end();
		toast_error("coverart_wifi_failed");
	}
}

static void enable_wifi_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !player_playing_local_file()) {
		return;
	}
	snprintf(wifi_path, sizeof(wifi_path), "%s", state.current_file);
	wifi_set_enabled(true);
	wifi_since = lv_tick_get();
	if (!wifi_timer) {
		wifi_timer = lv_timer_create(wifi_tick_cb, WIFI_POLL_MS, NULL);
	}
	toast_busy_dismissable("coverart_wifi_connecting");
}

// ---------------------------------------------------------------------------
// The search
// ---------------------------------------------------------------------------

typedef struct {
	char path[1024];
	char album[256];
	char artist[256];
	coverart_result_t result;
	coverart_hit_t *hits;
	int count;
} search_job_t;

static void search_done(void *user) {
	search_job_t *job = user;
	busy = false;
	toast_busy_end();

	switch (job->result) {
	case COVERART_OK: {
		// The list is for the track it was asked about; with another playing it
		// is no longer wanted.
		device_state_t state;
		device_state_get(&state);
		if (strcmp(state.current_file, job->path) == 0) {
			snprintf(file_path, sizeof(file_path), "%s", job->path);
			open_list(job->hits, job->count);
			job->hits = NULL;
		}
		break;
	}
	case COVERART_NOT_FOUND:
		toast_plain("coverart_not_found");
		break;
	case COVERART_NO_TAGS:
		toast_error("coverart_no_tags");
		break;
	default:
		toast_error("coverart_search_failed");
		break;
	}
	free(job->hits);
	free(job);
}

static void *search_worker(void *arg) {
	search_job_t *job = arg;
	job->result = coverart_search(job->album, job->artist, &job->hits, &job->count);
	if (!gui_post(search_done, job)) {
		free(job->hits);
		free(job);
	}
	return NULL;
}

static void search_run(void) {
	if (busy) {
		toast_busy_dismissable("coverart_searching");
		return;
	}
	if (!wifi_online()) {
		toast_error("coverart_no_wifi");
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !player_playing_local_file()) {
		return;
	}

	search_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", state.current_file);
	snprintf(job->album, sizeof(job->album), "%s", state.metadata.album);
	snprintf(job->artist, sizeof(job->artist), "%s", state.metadata.album_artist[0] ? state.metadata.album_artist : state.metadata.artist);
	coverart_set_contact(config_get("coverart", "contact", ""));

	pthread_t thread;
	if (pthread_create(&thread, NULL, search_worker, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
	busy = true;
	toast_busy_dismissable("coverart_searching");
}

void coversearch_start(void) {
	if (busy) {
		toast_busy_dismissable("coverart_searching");
		return;
	}
	if (wifi_timer) {
		toast_busy_dismissable("coverart_wifi_connecting");
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !player_playing_local_file()) {
		return;
	}
	// A picture in the tags comes before any cover file in the folder, so one
	// saved there would never be shown.
	albumart_t art;
	if (albumart_load_candidate(state.current_file, 0, &art)) {
		albumart_free(&art);
		toast_plain("coverart_has_embedded");
		return;
	}
	if (!wifi_online()) {
		// Off, and there is a radio to turn on: offer that in place of the search.
		if (!wifi_get_enabled() && wifi_available()) {
			confirm_show("coverart_find_title", "coverart_wifi_off_note", "coverart_enable_wifi", enable_wifi_cb, NULL);
			return;
		}
		toast_error("coverart_no_wifi");
		return;
	}
	search_run();
}

// ---------------------------------------------------------------------------
// Who MusicBrainz is told is asking
//
// A page of its own, reached from Music settings: one line to type, kept as
// [coverart] contact. Empty keeps the search to the Store.
// ---------------------------------------------------------------------------

#define CONTACT_MAX 99

static lv_obj_t *contact_screen;
static lv_obj_t *contact_field;
static keyboard_t *contact_keyboard;
static lv_obj_t *contact_value; // the row on the settings page

lv_obj_t *coversearch_contact_screen(void) { return contact_screen; }

void coversearch_contact_row(lv_obj_t *parent) {
	settingsrow_add(parent, "coverart_contact", &contact_value, switch_screen_cb, contact_screen);
	if (contact_value) {
		lv_obj_set_width(contact_value, lv_pct(42));
		lv_obj_set_style_text_font(contact_value, bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_20, 0);
		lv_obj_set_style_text_align(contact_value, LV_TEXT_ALIGN_RIGHT, 0);
		lv_label_set_long_mode(contact_value, LV_LABEL_LONG_DOT);
		lv_label_set_text(contact_value, config_get("coverart", "contact", ""));
	}
}

static void contact_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(contact_field, config_get("coverart", "contact", ""));
	keyboard_reset(contact_keyboard);
}

static void contact_accept_cb(lv_event_t *e) {
	(void)e;
	const char *typed = lv_textarea_get_text(contact_field);
	coverart_set_contact(typed ? typed : "");
	config_set("coverart", "contact", coverart_contact());
	config_save();
	if (contact_value) {
		lv_label_set_text(contact_value, coverart_contact());
	}
	toast_success("coverart_contact_saved");
	back_btn_cb(NULL);
}

static void contact_build(gui_config_t *cfg) {
	contact_screen = lv_obj_create(NULL);
	lv_obj_add_style(contact_screen, &theme_style_screen, 0);
	settingsrow_title(contact_screen, cfg, "coverart_contact_title");
	int top = settingsrow_content_top(cfg);

	contact_field = lv_textarea_create(contact_screen);
	lv_textarea_set_one_line(contact_field, true);
	lv_textarea_set_max_length(contact_field, CONTACT_MAX);
	lv_textarea_set_placeholder_text(contact_field, tr("coverart_contact_hint"));
	lv_obj_set_size(contact_field, (int)cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 62);
	lv_obj_set_scrollbar_mode(contact_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(contact_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(contact_field, &theme_style_card, 0);
	lv_obj_set_style_radius(contact_field, 12, 0);
	lv_obj_set_style_border_width(contact_field, 0, 0);
	lv_obj_set_style_shadow_width(contact_field, 0, 0);
	lv_obj_set_style_pad_all(contact_field, cfg->screen_width < 320 ? 8 : 14, 0);
	lv_obj_set_style_text_font(contact_field, &font_ui_24, 0);
	keyboard_style_caret(contact_field);

	lv_obj_t *note = lv_label_create(contact_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, (int)cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + (cfg->screen_width < 320 ? 52 : 78));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("coverart_contact_note"));
	lv_obj_set_hidden(note, cfg->screen_width < 320);

	contact_keyboard = keyboard_create(contact_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, contact_field, NULL, "ok", contact_accept_cb, NULL);
	lv_obj_add_event_cb(contact_screen, contact_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(contact_screen);
}

void coversearch_init(gui_config_t *cfg) {
	lv_obj_add_style(coversearch_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(coversearch_screen, cfg, "coverart_results");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	int height = cfg->screen_height - content_top;

	list = lv_obj_create(coversearch_screen);
	lv_obj_set_size(list, lv_pct(100), height);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_top(list, 0, 0);
	lv_obj_set_style_pad_bottom(list, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list, bp_pick(5, 8), 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);
	switcher_attach_back_gesture(list);

	// The picture, square, above a bar holding Apply.
	int bar_h = APPLY_H + 2 * cfg->padding;
	int room = height - bar_h - 2 * cfg->padding;
	int wide = (int)cfg->screen_width - 2 * cfg->padding;
	picture_side = wide < room ? wide : room;
	preview = lv_obj_create(coversearch_screen);
	lv_obj_set_size(preview, lv_pct(100), height - bar_h);
	lv_obj_align(preview, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(preview, 0, 0);
	lv_obj_set_style_border_width(preview, 0, 0);
	lv_obj_set_style_radius(preview, 0, 0);
	lv_obj_set_style_pad_all(preview, cfg->padding, 0);
	lv_obj_set_scrollbar_mode(preview, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_event_bubble(preview, true);
	switcher_attach_back_gesture(preview);

	preview_image = lv_image_create(preview);
	lv_obj_align(preview_image, LV_ALIGN_TOP_MID, 0, 0);

	apply_bar = lv_obj_create(coversearch_screen);
	lv_obj_remove_style_all(apply_bar);
	lv_obj_set_size(apply_bar, lv_pct(100), bar_h);
	lv_obj_align(apply_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_scrollable(apply_bar, false);

	lv_obj_t *apply = lv_btn_create(apply_bar);
	lv_obj_set_size(apply, APPLY_W, APPLY_H);
	lv_obj_center(apply);
	lv_obj_add_style(apply, &theme_style_card, 0);
	lv_obj_add_style(apply, &theme_style_accent_bg, 0);
	lv_obj_add_style(apply, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(apply, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(apply, 0, 0);
	lv_obj_set_style_shadow_width(apply, 0, 0);
	lv_obj_add_event_cb(apply, apply_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *apply_label = lv_label_create(apply);
	lv_label_set_text(apply_label, tr("coverart_apply"));
	lv_obj_set_style_text_font(apply_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(apply_label, lv_color_white(), 0);
	lv_obj_center(apply_label);

	contact_build(cfg);
	show_list();

	switcher_set_back_guard(coversearch_screen, back_guard);
	switcher_set_back_guard_peek(coversearch_screen, back_peek);
	lv_obj_add_event_cb(coversearch_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
