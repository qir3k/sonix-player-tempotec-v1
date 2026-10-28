#include "ytmusicpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/streamkeep.h"
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
static lv_obj_t *source_screen;
static lv_obj_t *source_list;
static lv_obj_t *source_value_label;
static ytmusic_source_t source_items[YTMUSIC_SOURCES_MAX];
static int source_item_count;
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

// The rows of the home page: which of them are there, and in what order. Both
// are the page's own setting, kept in the config as the numbers below -- the
// order as one list, the switched-off ones as another -- so somebody who opens
// Top songs every day does not scroll past Explore to reach it, and somebody
// who never opens Explore does not see it at all.
enum {
	YT_HOME_SEARCH = 0,
	YT_HOME_EXPLORE,
	YT_HOME_CHARTS,
	YT_HOME_FAVOURITES,
	YT_HOME_PODCASTS,
	YT_HOME_ROWS,
};
static const char *const YT_HOME_SLUGS[YT_HOME_ROWS] = {
	"ytmusic_search",
	"ytmusic_explore",
	"ytmusic_charts",
	"ytmusic_favourites",
	"ytmusic_podcasts",
};
static int yt_home_order[YT_HOME_ROWS];
static bool yt_home_hidden[YT_HOME_ROWS];

// The three settings this page keeps for what it plays: how good the stream
// should be, which country's catalogue it comes from, and when to stop.
static lv_obj_t *quality_pills[2];
static settingsrow_duration_t yt_sleep_row;
#define YT_REGION_MAX 8
static lv_obj_t *region_screen;
static lv_obj_t *region_list;
static lv_obj_t *region_value_label;
static ytmusic_region_t region_items[YT_REGION_MAX];
static int region_item_count;
static lv_obj_t *order_screen;
static lv_obj_t *order_rows;

static void home_refresh_session_rows(void);
static void home_build(void);
// The three dots a row that plays carries instead of a chevron; defined with the
// playing code below, used where the rows are built.
static void row_kebab_add(lv_obj_t *row, const ytmusic_item_t *item);

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

// ---------------------------------------------------------------------------
// row artwork
// ---------------------------------------------------------------------------
//
// A row is drawn with the picture its entry came with -- an album card's
// sleeve, a song's cover -- rather than a fixed glyph, so this tab reads like
// the Tidal one. The loader is the shared one (see qobuzart.h): twelve slots,
// the download and the decode on its own thread, and each image kept on the
// card under .local/qobuz-art named by the MD5 of its URL, so a second look at
// the same list downloads nothing.
//
// Two lists carry rows -- a search's or a chart's results, and the contents of
// a card -- and each keeps its own state, because both stay alive: coming back
// from a track finds the results list where it was left, and freeing a hidden
// list's pixels would leave its rows pointing at nothing. The twelve slots are
// shared, so they belong to the list that is on screen; leaving one hands its
// requests back, and returning asks for them again (the card's cache answers
// at once).
#define YTMUSIC_ROW_THUMB bp_pick(38, 60)

typedef struct {
	lv_obj_t *list;        // the container the rows live in
	ytmusic_item_t *items; // what the rows were built from, in row order
	int count;
	lv_obj_t *thumbs[YTMUSIC_PAGE_RESULTS]; // the cover image of each row, NULL when the entry has none
	// The glyph each row was built with, put back while its picture is not
	// there: a row with no cover keeps the icon it would have had anyway, and
	// the same one it had before this page drew artwork at all.
	const lv_image_dsc_t *placeholders[YTMUSIC_PAGE_RESULTS];
	cover_image_t images[YTMUSIC_PAGE_RESULTS];
	bool has_image[YTMUSIC_PAGE_RESULTS];
	// A row that asked and got nothing -- a dead URL, an image that will not
	// decode, a host that does not answer -- is not asked again: a freed slot
	// restarts the pass over every row, and one broken row would otherwise keep
	// taking a slot from the rows that can be served.
	bool failed[YTMUSIC_PAGE_RESULTS];
} yt_art_list_t;

static yt_art_list_t art_results, art_tracks;
static yt_art_list_t *art_active;
// The token claiming the shared slots for this page: only its address matters,
// and it differs from the other pages'. See qobuzart.h.
static const char ytmusic_art_owner;
static int art_slot_row[QOBUZART_SLOTS]; // row of the active list in each slot, -1 = free
static lv_timer_t *art_timer;

static yt_art_list_t *art_state_for(lv_obj_t *list) {
	if (list == results_list) {
		return &art_results;
	}
	if (list == tracks_list) {
		return &art_tracks;
	}
	return NULL;
}

// Back to the glyph the row was built with. The placeholder is a themed icon,
// so the row reads the same as every other row of the page until its picture
// lands -- and again if one never does.
static void art_show_placeholder(yt_art_list_t *state, int row) {
	lv_obj_t *image = state ? state->thumbs[row] : NULL;
	if (!image) {
		return;
	}
	const lv_image_dsc_t *glyph = state->placeholders[row] ? state->placeholders[row] : &icon_music2;
	lv_image_set_src(image, glyph);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(image, 0, 0);
	lv_obj_set_style_clip_corner(image, false, 0);
}

static void art_release_slots(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		if (art_slot_row[slot] >= 0) {
			qobuzart_release(&ytmusic_art_owner, slot);
			art_slot_row[slot] = -1;
		}
	}
}

static bool art_any_pending(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		if (art_slot_row[slot] >= 0) {
			return true;
		}
	}
	return false;
}

static void art_wake(void) {
	if (art_timer) {
		lv_timer_resume(art_timer);
	}
}

// The pictures of a list that is no longer on screen are given back. Its rows
// stay -- coming back finds the list where it was left -- so each one goes back
// to its glyph first and the pixels are freed with nothing pointing at them.
//
// Held only while the list showing them is up: twenty-four decoded covers are
// tens of kilobytes apiece on the card's side of the screen, and the list that
// is not being looked at has no use for them. Returning asks for them again,
// which the loader's cache on the card answers at once.
static void art_give_back_pictures(yt_art_list_t *state) {
	for (int row = 0; row < YTMUSIC_PAGE_RESULTS; row++) {
		if (state->thumbs[row] && state->has_image[row]) {
			art_show_placeholder(state, row);
		}
		state->has_image[row] = false;
		cover_free(&state->images[row]);
	}
}

static void art_request_visible(void);

// Finished covers are collected here, in the same shape as the player's own
// cover loader: the thread works, the list draws, and whatever is ready appears
// on the next pass.
static void art_poll_cb(lv_timer_t *timer) {
	if (!art_active || !art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() resumes it
		return;
	}

	bool freed = false;
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = art_slot_row[slot];
		if (row < 0 || row >= YTMUSIC_PAGE_RESULTS) {
			continue;
		}
		lv_obj_t *image = art_active->thumbs[row];
		if (!image) {
			art_slot_row[slot] = -1;
			freed = true;
			continue;
		}
		cover_image_t cover;
		bool finished = false;
		if (!qobuzart_take(&ytmusic_art_owner, slot, &cover, &finished)) {
			if (finished) {
				art_active->failed[row] = true;
				art_slot_row[slot] = -1; // nothing to show: the slot goes back
				freed = true;
			}
			continue;
		}

		cover_free(&art_active->images[row]);
		art_active->images[row] = cover;
		art_active->has_image[row] = true;
		// The placeholder was recoloured by the theme; a sleeve keeps its own
		// colours, or it comes out a solid square.
		lv_obj_remove_style(image, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(image, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(image, 6, 0);
		lv_obj_set_style_clip_corner(image, true, 0);
		lv_image_set_src(image, &art_active->images[row].dsc);
		art_slot_row[slot] = -1;
		freed = true;
	}

	// A freed slot is handed out again at once. There are more rows than slots,
	// so a pass serves what it can and returns; without this the rows beyond the
	// twelfth would wait for the next scroll, and never be asked for at all if
	// the last scroll happened while every slot was busy.
	if (freed) {
		art_request_visible();
	}
}

// Asks for the covers of the visible rows plus a margin above and below, so a
// slow scroll finds them already there. Called on activation and on every
// scroll.
static void art_request_visible(void) {
	if (!art_active || !art_active->list) {
		return;
	}
	lv_obj_t *list = art_active->list;
	int32_t view_top = lv_obj_get_scroll_y(list);
	int32_t view_height = lv_obj_get_height(list);
	int32_t margin = view_height / 2;

	uint32_t children = lv_obj_get_child_count(list);
	for (uint32_t c = 0; c < children && c < YTMUSIC_PAGE_RESULTS; c++) {
		int row = (int)c;
		lv_obj_t *image = art_active->thumbs[row];
		if (!image || art_active->has_image[row] || art_active->failed[row]) {
			continue;
		}
		if (row >= art_active->count || !art_active->items[row].thumbnail[0]) {
			continue;
		}

		lv_obj_t *widget = lv_obj_get_child(list, row);
		int32_t y = lv_obj_get_y(widget);
		int32_t h = lv_obj_get_height(widget);
		if (y + h < view_top - margin || y > view_top + view_height + margin) {
			continue; // too far away to be worth a request
		}

		bool queued = false;
		for (int slot = 0; slot < QOBUZART_SLOTS && !queued; slot++) {
			queued = art_slot_row[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS && free_slot < 0; slot++) {
			if (art_slot_row[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return; // all busy: the rest on the next pass
		}

		art_slot_row[free_slot] = row;
		art_wake();
		qobuzart_request(&ytmusic_art_owner, free_slot, art_active->items[row].thumbnail, YTMUSIC_ROW_THUMB);
	}
}

// The list that is on screen owns the slots, and the pictures -- the one it
// replaced gives its own back.
static void art_activate(yt_art_list_t *state, lv_obj_t *list, ytmusic_item_t *items, int count) {
	if (art_active && art_active != state) {
		art_release_slots();
		art_give_back_pictures(art_active);
	}
	state->list = list;
	state->items = items;
	state->count = count;
	art_active = state;
	art_request_visible();
}

static void art_deactivate(void) {
	if (!art_active) {
		return;
	}
	art_release_slots();
	art_give_back_pictures(art_active);
	art_active = NULL;
}

// A page with no rows of its own: the covers are given back while it is up.
static void no_art_loaded_cb(lv_event_t *e) {
	(void)e;
	art_deactivate();
}

// The rows of one list are about to be rebuilt and their pictures with them.
// Called before lv_obj_clean(), which is what makes freeing the pixels safe.
static void art_forget_rows(yt_art_list_t *state) {
	if (!state) {
		return;
	}
	if (art_active == state) {
		art_release_slots();
	}
	for (int row = 0; row < YTMUSIC_PAGE_RESULTS; row++) {
		state->thumbs[row] = NULL;
		state->placeholders[row] = NULL;
		state->has_image[row] = false;
		state->failed[row] = false;
		cover_free(&state->images[row]);
	}
}

static void art_scrolled_cb(lv_event_t *e) {
	(void)e;
	art_request_visible();
}

static void results_loaded_cb(lv_event_t *e) {
	(void)e;
	art_activate(&art_results, results_list, result_items, result_count);
}

static void tracks_loaded_cb(lv_event_t *e) {
	(void)e;
	art_activate(&art_tracks, tracks_list, track_items, track_count);
}

static void fill_item_list(lv_obj_t *list, const ytmusic_item_t *items, int count, lv_event_cb_t cb) {
	yt_art_list_t *art = art_state_for(list);
	art_forget_rows(art);
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
		// A row that plays, not one that opens something: no chevron. The
		// chevron means "this goes somewhere", and a song's tap starts it here
		// instead -- the same as a track on the Tidal page. The cover slot, the
		// glyph and the wrapping below are the same either way.
		lv_obj_t *row = items[i].kind == YTMUSIC_ITEM_TRACK
							? settingsrow_action(list, label, cb, (void *)(intptr_t)i)
							: settingsrow_add(list, label, NULL, cb, (void *)(intptr_t)i);
		// A song also gets the menu a Tidal track has: the row's tap plays it,
		// the dots offer what a tap cannot -- playing it now, or downloading it
		// to the card whether or not it is the song in the player.
		if (items[i].kind == YTMUSIC_ITEM_TRACK) {
			row_kebab_add(row, &items[i]);
		}
		// An entry with artwork gets the shared cover slot; one without keeps
		// the glyph it would have had anyway.
		if (art && items[i].thumbnail[0]) {
			// The row's own glyph stands in until its sleeve lands, and comes
			// back whenever the picture is given up: a song keeps its note, a
			// show its podcast glyph, and no row changes shape on the way.
			art->placeholders[i] = item_icon(&items[i]);
			art->thumbs[i] = settingsrow_add_art(row, art->placeholders[i], YTMUSIC_ROW_THUMB);
		} else {
			settingsrow_add_icon(row, item_icon(&items[i]), item_tone(&items[i]));
		}
		// After the cover, not before: the name's room is worked out from what
		// sits beside it, and asked first it started at the cover's own left
		// edge -- the text and the sleeve drawn on top of each other.
		settingsrow_name_lines(row, 2);
	}
	// A list that is already on screen -- a second search, a card opened from a
	// card -- asks for its covers here; the screen-loaded callback covers the
	// other case, where the list is about to be shown for the first time.
	if (art) {
		art->list = list;
		art->items = (ytmusic_item_t *)items;
		art->count = count;
		if (art_active == art) {
			art_request_visible();
		}
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
	// The row's cover goes with the track: the downloader fetches it beside the
	// audio, which is where the player looks for a file's artwork.
	snprintf(result->track.thumbnail, sizeof(result->track.thumbnail), "%s", item->thumbnail);
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

// ---------------------------------------------------------------------------
// the row menu
//
// A song's row taps straight into playing it, so there is no page to open and
// no chevron to open it with. What a tap cannot do lives behind the three dots
// instead, in a popover anchored to the row: the Tidal page's shape.
// ---------------------------------------------------------------------------

static void track_from_item(const ytmusic_item_t *item, ytmusic_track_t *track) {
	memset(track, 0, sizeof(*track));
	snprintf(track->id, sizeof(track->id), "%s", item->id);
	snprintf(track->title, sizeof(track->title), "%s", item->title);
	// A row's subtitle is the artist, or the show an episode belongs to.
	snprintf(track->artist, sizeof(track->artist), "%s", item->subtitle);
	// The row's cover goes with the track: the downloader fetches it beside the
	// audio, which is where the player looks for a file's artwork.
	snprintf(track->thumbnail, sizeof(track->thumbnail), "%s", item->thumbnail);
}

// Downloading without playing. The same cache file is made, and then handed to
// the keep worker instead of to the player, so it ends up in
// <card>/Music/YouTube Music/ with its tags and sleeve.
typedef struct {
	ytmusic_track_t track;
	char path[512];
	char error[192];
	bool ok;
} save_result_t;

static void save_done(void *data) {
	save_result_t *result = data;
	if (!result) return;
	end_busy();
	if (!result->ok) toast_error(result->error[0] ? result->error : "ytmusic_playback_failed");
	free(result);
}

static void *save_worker(void *arg) {
	save_result_t *result = arg;
	result->ok = ytmusic_download_for_playback(&result->track, result->path, sizeof(result->path),
											   result->error, sizeof(result->error));
	if (result->ok && !streamkeep_keep(result->path, true)) {
		// No card, or one the computer is using: the keep worker records why
		// without a toast of its own, so the reason is read back here.
		char why[192] = "";
		result->ok = false;
		if (streamkeep_last_error(why, sizeof(why)) && why[0]) {
			snprintf(result->error, sizeof(result->error), "%s", why);
		}
	}
	if (!gui_post(save_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void row_menu_save(void *user) {
	const ytmusic_item_t *item = user;
	if (!item || item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) return;
	if (!begin_busy()) return;
	save_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	track_from_item(item, &result->track);
	pthread_t thread;
	if (pthread_create(&thread, NULL, save_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error("ytmusic_playback_failed");
		return;
	}
	pthread_detach(thread);
	gui_notify_popup(tr("downloading"));
}

static void row_menu_play(void *user) {
	play_item((const ytmusic_item_t *)user);
}

static void row_menu_cb(lv_event_t *e) {
	const ytmusic_item_t *item = lv_event_get_user_data(e);
	if (!item) return;
	popover_item_t items[] = {
		{"ytmusic_play", row_menu_play, (void *)item, false},
		{"trackmenu_keep_on_card", row_menu_save, (void *)item, false},
	};
	popover_show(lv_event_get_target(e), items, 2);
}

static void row_kebab_add(lv_obj_t *row, const ytmusic_item_t *item) {
	lv_obj_t *button = lv_btn_create(row);
	lv_obj_set_size(button, bp_pick(26, 34), bp_pick(28, 34));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, row_menu_cb, LV_EVENT_CLICKED, (void *)item);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_ellipsis_vertical);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
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
	lv_obj_add_event_cb(search_screen, no_art_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(search_screen);

	results_screen = lv_obj_create(NULL);
	lv_obj_add_style(results_screen, &theme_style_screen, 0);
	results_list = settingsrow_page(results_screen, page_cfg, "YouTube Music");
	results_title = settingsrow_page_title(results_screen);
	player_sheet_attach_drag(results_list, true);
	// The covers belong to the list that is on screen, and these two carry rows.
	lv_obj_add_event_cb(results_screen, results_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(results_list, art_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(results_screen);

	tracks_screen = lv_obj_create(NULL);
	lv_obj_add_style(tracks_screen, &theme_style_screen, 0);
	tracks_list = settingsrow_page(tracks_screen, page_cfg, "YouTube Music");
	tracks_title = settingsrow_page_title(tracks_screen);
	player_sheet_attach_drag(tracks_list, true);
	lv_obj_add_event_cb(tracks_screen, tracks_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(tracks_list, art_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(tracks_screen);

	explore_screen = lv_obj_create(NULL);
	lv_obj_add_style(explore_screen, &theme_style_screen, 0);
	explore_list = settingsrow_page(explore_screen, page_cfg, "ytmusic_explore");
	player_sheet_attach_drag(explore_list, true);
	// Its rows are carousel names, not entries with artwork: whoever was
	// holding the slots gives them back while this page is up.
	lv_obj_add_event_cb(explore_screen, no_art_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
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

// The value beside the source row, and whether it is the chosen one.
static void source_refresh(void) {
	if (!source_value_label) return;
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	const char *shown = active;
	for (int i = 0; i < source_item_count; i++) {
		if (strcmp(source_items[i].value, active) == 0) {
			shown = tr(source_items[i].label);
			break;
		}
	}
	// An instance pinned by hand and no longer in the list still says what it
	// is, without the scheme: the row's value is a host name.
	const char *host = strstr(shown, "://");
	lv_label_set_text(source_value_label, host ? host + 3 : shown);
}

static void source_row_build(void);

static void source_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= source_item_count) return;
	if (!ytmusic_source_select(source_items[index].value)) {
		toast_error("ytmusic_source_invalid");
		return;
	}
	toast_success("ytmusic_source_saved");
	source_refresh();
	if (source_list) lv_obj_clean(source_list);
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_row_build();
}

static void source_row_build(void) {
	if (!source_list) return;
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	for (int i = 0; i < source_item_count; i++) {
		lv_obj_t *value = NULL;
		settingsrow_add(source_list, tr(source_items[i].label), &value, source_pick_cb,
						(void *)(intptr_t)i);
		if (value && strcmp(source_items[i].value, active) == 0) {
			lv_label_set_text(value, tr("on"));
		}
	}
}

static void source_loaded_cb(lv_event_t *e) {
	(void)e;
	if (source_list) lv_obj_clean(source_list);
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_row_build();
}

static void open_source_cb(lv_event_t *e) {
	(void)e;
	switch_screen(source_screen);
}

static void build_source_page(gui_config_t *cfg) {
	source_screen = lv_obj_create(NULL);
	lv_obj_add_style(source_screen, &theme_style_screen, 0);
	source_list = settingsrow_page(source_screen, cfg, "ytmusic_source");
	player_sheet_attach_drag(source_list, true);
	lv_obj_add_event_cb(source_screen, source_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(source_screen);
}

// ---------------------------------------------------------------------------
// the arrangement of the home page
// ---------------------------------------------------------------------------

// Read again whenever the home page is built: a change made on the arrange page
// is then what the page shows the next time it is looked at, without a restart.
static void yt_home_order_load(void) {
	for (int i = 0; i < YT_HOME_ROWS; i++) {
		yt_home_order[i] = i;
		yt_home_hidden[i] = false;
	}
	const char *saved = config_get("ytmusic", "home_order", "");
	if (saved[0]) {
		bool seen[YT_HOME_ROWS] = {false};
		int at = 0;
		const char *p = saved;
		while (*p && at < YT_HOME_ROWS) {
			while (*p == ',' || *p == ' ') p++;
			if (!*p) break;
			int id = atoi(p);
			while (*p >= '0' && *p <= '9') p++;
			if (id >= 0 && id < YT_HOME_ROWS && !seen[id]) {
				seen[id] = true;
				yt_home_order[at++] = id;
			}
		}
		// A row added by a later build has no place in the saved order yet.
		for (int id = 0; id < YT_HOME_ROWS; id++) {
			if (!seen[id] && at < YT_HOME_ROWS) yt_home_order[at++] = id;
		}
	}
	const char *hidden = config_get("ytmusic", "home_hidden", "");
	const char *p = hidden;
	while (*p) {
		while (*p == ',' || *p == ' ') p++;
		if (!*p) break;
		int id = atoi(p);
		while (*p >= '0' && *p <= '9') p++;
		if (id >= 0 && id < YT_HOME_ROWS) yt_home_hidden[id] = true;
	}
}

static void yt_home_order_save(void) {
	char order[64] = "", hidden[64] = "";
	for (int slot = 0; slot < YT_HOME_ROWS; slot++) {
		size_t used = strlen(order);
		snprintf(order + used, sizeof(order) - used, "%s%d", used ? "," : "", yt_home_order[slot]);
	}
	bool any = false;
	for (int id = 0; id < YT_HOME_ROWS; id++) {
		if (!yt_home_hidden[id]) continue;
		size_t used = strlen(hidden);
		snprintf(hidden + used, sizeof(hidden) - used, "%s%d", any ? "," : "", id);
		any = true;
	}
	config_set("ytmusic", "home_order", order);
	config_set("ytmusic", "home_hidden", hidden);
	config_save();
}

static void yt_order_move_cb(lv_event_t *e);
static void yt_order_toggle_cb(lv_event_t *e);
static void region_pick_cb(lv_event_t *e);

static void yt_order_rebuild(void) {
	if (!order_rows) return;
	lv_obj_clean(order_rows);
	for (int pos = 0; pos < YT_HOME_ROWS; pos++) {
		int id = yt_home_order[pos];
		lv_obj_t *row = lv_obj_create(order_rows);
		lv_obj_set_size(row, lv_pct(100), bp_pick(40, 56));
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_style_radius(row, bp_tile_radius(), 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, bp_pick(6, 12), 0);
		lv_obj_set_style_pad_gap(row, bp_pick(1, 5), 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *name = lv_label_create(row);
		lv_label_set_text(name, tr(YT_HOME_SLUGS[id]));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(name, 1);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22, 0);

		// Up and down, in the list's own order: a row moves one place at a time
		// and the switch beside it is whether the home page shows it at all.
		for (int dir = 0; dir < 2; dir++) {
			lv_obj_t *button = lv_btn_create(row);
			lv_obj_set_size(button, bp_pick(28, 40), bp_pick(30, 40));
			lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(button, 0, 0);
			lv_obj_set_style_shadow_width(button, 0, 0);
			lv_obj_set_style_pad_all(button, 0, 0);
			lv_obj_add_event_cb(button, yt_order_move_cb, LV_EVENT_CLICKED,
								(void *)(intptr_t)((dir ? 0 : 0x100) | pos));
			lv_obj_t *icon = lv_image_create(button);
			lv_image_set_src(icon, &icon_chevron_up);
			lv_image_set_rotation(icon, dir ? 1800 : 0);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			lv_obj_center(icon);
			if ((dir == 0 && pos == 0) || (dir == 1 && pos == YT_HOME_ROWS - 1)) {
				lv_obj_add_state(button, LV_STATE_DISABLED);
			}
		}

		lv_obj_t *toggle = lv_switch_create(row);
		lv_obj_set_size(toggle, bp_pick(34, 48), bp_pick(20, 28));
		if (!yt_home_hidden[id]) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		}
		lv_obj_add_event_cb(toggle, yt_order_toggle_cb, LV_EVENT_VALUE_CHANGED,
							(void *)(intptr_t)id);
	}
	lv_obj_update_layout(order_rows);
}

static void yt_order_move_cb(lv_event_t *e) {
	int encoded = (int)(intptr_t)lv_event_get_user_data(e);
	int pos = encoded & 0xff;
	int target = pos + ((encoded & 0x100) ? -1 : 1);
	if (pos < 0 || pos >= YT_HOME_ROWS || target < 0 || target >= YT_HOME_ROWS) return;
	int swap = yt_home_order[pos];
	yt_home_order[pos] = yt_home_order[target];
	yt_home_order[target] = swap;
	yt_home_order_save();
	yt_order_rebuild();
	home_build();
}

static void yt_order_toggle_cb(lv_event_t *e) {
	int id = (int)(intptr_t)lv_event_get_user_data(e);
	if (id < 0 || id >= YT_HOME_ROWS) return;
	yt_home_hidden[id] = !lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	yt_home_order_save();
	home_build();
}

static void order_loaded_cb(lv_event_t *e) {
	(void)e;
	yt_home_order_load();
	yt_order_rebuild();
}

static void open_order_cb(lv_event_t *e) {
	(void)e;
	switch_screen(order_screen);
}

static void build_order_page(gui_config_t *cfg) {
	order_screen = lv_obj_create(NULL);
	lv_obj_add_style(order_screen, &theme_style_screen, 0);
	order_rows = settingsrow_page(order_screen, cfg, "ytmusic_arrange");
	player_sheet_attach_drag(order_rows, true);
	lv_obj_add_event_cb(order_screen, order_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(order_screen);
}

// ---------------------------------------------------------------------------
// the region page
// ---------------------------------------------------------------------------

static void region_refresh(void) {
	if (!region_value_label) return;
	char region[YTMUSIC_REGION_MAX], language[YTMUSIC_REGION_MAX];
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	for (int i = 0; i < region_item_count; i++) {
		if (strcmp(region_items[i].region, region) == 0 &&
			strcmp(region_items[i].language, language) == 0) {
			lv_label_set_text(region_value_label, tr(region_items[i].label));
			return;
		}
	}
	// A pair written into the config by hand still says what it is.
	char both[24];
	snprintf(both, sizeof(both), "%s · %s", region, language);
	lv_label_set_text(region_value_label, both);
}

static void region_row_build(void) {
	if (!region_list) return;
	lv_obj_clean(region_list);
	char region[YTMUSIC_REGION_MAX], language[YTMUSIC_REGION_MAX];
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	for (int i = 0; i < region_item_count; i++) {
		lv_obj_t *value = NULL;
		settingsrow_add(region_list, tr(region_items[i].label), &value, region_pick_cb,
						(void *)(intptr_t)i);
		if (value && strcmp(region_items[i].region, region) == 0 &&
			strcmp(region_items[i].language, language) == 0) {
			lv_label_set_text(value, tr("on"));
		}
	}
}

static void region_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= region_item_count) return;
	if (!ytmusic_region_select(region_items[index].region, region_items[index].language)) {
		toast_error("ytmusic_region_invalid");
		return;
	}
	toast_success("ytmusic_region_saved");
	region_refresh();
	region_row_build();
}

static void region_loaded_cb(lv_event_t *e) {
	(void)e;
	region_item_count = ytmusic_regions(region_items, YT_REGION_MAX);
	region_row_build();
}

static void open_region_cb(lv_event_t *e) {
	(void)e;
	switch_screen(region_screen);
}

static void build_region_page(gui_config_t *cfg) {
	region_screen = lv_obj_create(NULL);
	lv_obj_add_style(region_screen, &theme_style_screen, 0);
	region_list = settingsrow_page(region_screen, cfg, "ytmusic_region");
	player_sheet_attach_drag(region_list, true);
	lv_obj_add_event_cb(region_screen, region_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(region_screen);
}

// ---------------------------------------------------------------------------
// quality and the sleep timer
// ---------------------------------------------------------------------------

static void yt_quality_refresh(void) {
	if (quality_pills[0]) settingsrow_pill_active(quality_pills[0], ytmusic_quality_high());
	if (quality_pills[1]) settingsrow_pill_active(quality_pills[1], !ytmusic_quality_high());
}

static void yt_quality_pick_cb(lv_event_t *e) {
	ytmusic_quality_set((int)(intptr_t)lv_event_get_user_data(e) != 0);
	yt_quality_refresh();
}

static void yt_sleep_refresh(void) {
	settingsrow_duration_set_minutes(&yt_sleep_row, sleeptimer_minutes(SLEEPTIMER_MUSIC));
	settingsrow_duration_expanded(&yt_sleep_row, sleeptimer_enabled(SLEEPTIMER_MUSIC));
	settingsrow_duration_repaint(&yt_sleep_row);
}

// The music transport's own timer, the one the card and Tidal use: a YouTube
// Music song plays through the same player, so it is the same stretch counting
// down and the same switch that stops it.
static void yt_sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, lv_obj_has_state(yt_sleep_row.toggle, LV_STATE_CHECKED));
	yt_sleep_refresh();
}

static void yt_sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, settingsrow_duration_minutes(&yt_sleep_row));
}

static void build_settings_page(gui_config_t *cfg) {
	build_source_page(cfg);
	build_region_page(cfg);
	build_order_page(cfg);
	settings_screen = lv_obj_create(NULL);
	lv_obj_add_style(settings_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "ytmusic_settings");

	// What this page plays, before what it fetches through.
	settingsrow_toggle_duration(container, "sleep_timer", yt_sleep_toggle_cb, yt_sleep_wheel_cb,
								&yt_sleep_row);
	yt_sleep_refresh();
	lv_obj_t *pill_box = NULL;
	settingsrow_pills(container, "ytmusic_quality", &pill_box);
	quality_pills[0] = settingsrow_pill(pill_box, "ytmusic_quality_high", 1, yt_quality_pick_cb);
	quality_pills[1] = settingsrow_pill(pill_box, "ytmusic_quality_standard", 0, yt_quality_pick_cb);
	yt_quality_refresh();
	lv_obj_t *region_row =
		settingsrow_add(container, "ytmusic_region", &region_value_label, open_region_cb, NULL);
	settingsrow_add_caption(region_row, "ytmusic_region_note");
	region_item_count = ytmusic_regions(region_items, YT_REGION_MAX);
	region_refresh();
	lv_obj_t *order_row = settingsrow_add(container, "ytmusic_arrange", NULL, open_order_cb, NULL);
	settingsrow_add_caption(order_row, "ytmusic_arrange_note");

	lv_obj_t *source_row =
		settingsrow_add(container, "ytmusic_source", &source_value_label, open_source_cb, NULL);
	settingsrow_add_caption(source_row, "ytmusic_source_note");
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_refresh();
	sign_in_row = settingsrow_action(container, "ytmusic_sign_in", login_begin_done_cb, NULL);
	settingsrow_add_icon(sign_in_row, &icon_import, THEME_SEMANTIC_CYAN);
	// Google's device token is refused by the music endpoints -- a signed-in
	// library page comes back with no rows and no error -- so the row says what
	// it does not do, and the session import below is what Favourites needs.
	settingsrow_add_caption(sign_in_row, "ytmusic_sign_in_note");
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

	// The rows in the order the user arranged them, with the switched-off ones
	// left out -- see the arrange page behind the settings' "Home rows" row.
	static void (*const handlers[YT_HOME_ROWS])(lv_event_t *) = {
		search_cb, explore_start_cb, charts_cb, favourites_cb, podcasts_cb,
	};
	static const lv_image_dsc_t *const icons[YT_HOME_ROWS] = {
		&icon_search, &icon_music2, &icon_music2, &icon_music_note, &icon_podcast_list,
	};
	static const theme_semantic_t tones[YT_HOME_ROWS] = {
		THEME_SEMANTIC_CYAN,
		THEME_SEMANTIC_PURPLE,
		THEME_SEMANTIC_RED,
		THEME_SEMANTIC_RED,
		THEME_SEMANTIC_CYAN,
	};
	yt_home_order_load();
	for (int slot = 0; slot < YT_HOME_ROWS; slot++) {
		int id = yt_home_order[slot];
		if (yt_home_hidden[id]) continue;
		lv_obj_t *row = settingsrow_add(home_container, YT_HOME_SLUGS[id], NULL, handlers[id], NULL);
		settingsrow_add_icon(row, icons[id], tones[id]);
	}
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	art_deactivate();
	home_build();
}

void ytmusicpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	// Where a downloaded cover is kept. Called here as much as for the shared
	// loader's benefit: with no card named it has nowhere to write and every
	// list keeps its placeholders, which is the honest answer.
	qobuzart_set_root(storage_sd_root());
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
	// Created paused: the first cover request resumes it, and it pauses itself
	// again when the last one lands or fails.
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	if (art_timer) {
		lv_timer_pause(art_timer);
	}
	switcher_attach_back_gesture(ytmusic_screen);
}
