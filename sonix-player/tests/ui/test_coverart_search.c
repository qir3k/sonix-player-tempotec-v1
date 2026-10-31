/*
 * Cover art search -- the page the albums found are picked from, and the
 * cover.jpg Apply writes -- on the real renderer at the V1's 240x320.
 *
 * The network is substituted as in test_coverart.c, the playing track and the
 * decoder are stubs. What is checked on the screen is what only exists with
 * one: that every row of the list, the picture and the Apply button stay on the
 * glass, that a row downloads its picture off the interface thread and shows
 * it, and that Apply writes the cover beside the track, goes back to the player
 * and has it reload.
 */
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coversearch.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/albumart.h"
#include "src/system/library/coverart.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/device_state.h"

/* ---- fonts, as in the other page tests ---------------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold, font_ui_72;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
		&font_ui_64_bold, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }

/* ---- the parts of the shell the page leans on, substituted -------------- */

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

static char last_toast[128];
void toast_error(const char *text) { snprintf(last_toast, sizeof(last_toast), "error:%s", text ? text : ""); }
void toast_plain(const char *text) { snprintf(last_toast, sizeof(last_toast), "plain:%s", text ? text : ""); }
void toast_success(const char *text) { snprintf(last_toast, sizeof(last_toast), "success:%s", text ? text : ""); }
void toast_busy_dismissable(const char *text) { snprintf(last_toast, sizeof(last_toast), "busy:%s", text ? text : ""); }
void toast_busy_end(void) {}

lv_obj_t *player_screen;
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void back_btn_cb(lv_event_t *e) {
	(void)e;
	switch_screen(player_screen);
}
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
static lv_obj_t *player_return;
void switcher_set_player_return(lv_obj_t *screen) { player_return = screen; }
static bool (*back_guard)(void);
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	(void)screen;
	back_guard = handler;
}
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	(void)screen;
	(void)peek;
}
void player_sheet_close(bool animate) { (void)animate; }

static int refreshed;
void player_refresh_now_playing(void) { refreshed++; }
bool player_playing_local_file(void) { return true; }

/* The playing track, and Wi-Fi. */
static char playing[512];
static bool wifi_up = true;
static bool has_embedded;

static bool wifi_enabled = true;
static bool radio_there = true;
bool wifi_get_enabled(void) { return wifi_enabled; }
bool wifi_available(void) { return radio_there; }
static int wifi_switched_on;
void wifi_set_enabled(bool on) {
	wifi_enabled = on;
	wifi_switched_on += on ? 1 : 0;
}

/* The question that offers to switch Wi-Fi on, answered by the test. */
static char asked_title[64];
static void (*asked_ok)(void *);
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)message;
	(void)ok_label;
	(void)user;
	snprintf(asked_title, sizeof(asked_title), "%s", title);
	asked_ok = on_ok;
}

/* The keyboard under the contact field: only its accept key matters here. */
static lv_event_cb_t keyboard_accept;
keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field, const lv_image_dsc_t *accept_icon,
							const char *accept_text, lv_event_cb_t on_accept, void *user) {
	(void)parent;
	(void)width;
	(void)height;
	(void)field;
	(void)accept_icon;
	(void)accept_text;
	(void)user;
	keyboard_accept = on_accept;
	return (keyboard_t *)&keyboard_accept;
}
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void keyboard_style_caret(lv_obj_t *field) { (void)field; }

void device_state_get(device_state_t *out) {
	memset(out, 0, sizeof(*out));
	snprintf(out->current_file, sizeof(out->current_file), "%s", playing);
	snprintf(out->metadata.album, sizeof(out->metadata.album), "Album");
	snprintf(out->metadata.artist, sizeof(out->metadata.artist), "The Band");
}
void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = wifi_up ? WIFI_STATE_CONNECTED : WIFI_STATE_OFF;
	if (wifi_up) {
		snprintf(out->ip, sizeof(out->ip), "192.168.1.20");
	}
}
bool albumart_load_candidate(const char *filepath, int index, albumart_t *out) {
	(void)filepath;
	(void)index;
	memset(out, 0, sizeof(*out));
	return has_embedded;
}
void albumart_free(albumart_t *art) { (void)art; }

/* Posts from the worker threads are collected here and run on the test's own
 * thread, which is the interface thread. */
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*posted_cb)(void *);
static void *posted_user;
bool gui_post(void (*cb)(void *), void *user) {
	pthread_mutex_lock(&posted_lock);
	posted_cb = cb;
	posted_user = user;
	pthread_mutex_unlock(&posted_lock);
	return true;
}
static void run_posted(void) {
	for (int i = 0; i < 2000; i++) {
		pthread_mutex_lock(&posted_lock);
		void (*cb)(void *) = posted_cb;
		void *user = posted_user;
		posted_cb = NULL;
		pthread_mutex_unlock(&posted_lock);
		if (cb) {
			cb(user);
			return;
		}
		usleep(5000);
	}
	assert(!"the worker never answered");
}

/* A square grey picture of the size asked for. */
bool cover_load_image_memory(const void *data, size_t size, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	(void)data;
	(void)size;
	(void)fit;
	memset(out, 0, sizeof(*out));
	out->pixels = calloc((size_t)box_w * (size_t)box_h, 2);
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)box_w;
	out->dsc.header.h = (uint32_t)box_h;
	out->dsc.header.stride = (uint32_t)box_w * 2;
	out->dsc.data = out->pixels;
	out->dsc.data_size = (uint32_t)box_w * (uint32_t)box_h * 2;
	return true;
}
void cover_free(cover_image_t *img) {
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

/* ---- the network, substituted ------------------------------------------- */

static const char JPEG[] = "\xFF\xD8\xFF\xE0 a picture";
static int requests;

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	requests++;
	const char *body = JPEG;
	size_t len = sizeof(JPEG) - 1;
	static const char ITUNES[] =
		"{\"results\":["
		"{\"collectionName\":\"Greatest Hits\",\"artistName\":\"The Band\",\"releaseDate\":\"2011-01-01T08:00:00Z\","
		"\"artworkUrl100\":\"https://is1.mzstatic.com/image/a/100x100bb.jpg\"},"
		"{\"collectionName\":\"Album\",\"artistName\":\"The Band\",\"releaseDate\":\"2005-05-05T07:00:00Z\","
		"\"artworkUrl100\":\"https://is1.mzstatic.com/image/b/100x100bb.jpg\"}]}";
	if (strstr(url, "itunes.apple.com")) {
		body = ITUNES;
		len = strlen(ITUNES);
	}
	*status_out = 200;
	*out = malloc(len + 1);
	memcpy(*out, body, len);
	(*out)[len] = '\0';
	if (out_len) {
		*out_len = len;
	}
	return true;
}

const char *http_last_error(void) { return NULL; }
void http_set_user_agent_suffix(const char *suffix) { (void)suffix; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && w + 4 < out_size; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
			*p == '_' || *p == '.' || *p == '~') {
			out[w++] = (char)*p;
		} else {
			w += (size_t)snprintf(out + w, out_size - w, "%%%02X", *p);
		}
	}
	out[w] = '\0';
	return out;
}

/* ---- the strings, in every language -------------------------------------- */

static void check_strings(const char *language_dir) {
	static const char *const KEYS[] = {
		"trackmenu_find_cover", "coverart_results", "coverart_apply", "coverart_searching", "coverart_loading",
		"coverart_not_found", "coverart_no_tags", "coverart_search_failed", "coverart_download_failed",
		"coverart_not_saved", "coverart_saved", "coverart_no_wifi", "coverart_has_embedded",
		"coverart_find_title", "coverart_wifi_off_note", "coverart_enable_wifi", "coverart_wifi_connecting",
		"coverart_wifi_failed", "coverart_contact", "coverart_contact_title", "coverart_contact_hint", "coverart_contact_note",
		"coverart_contact_saved",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		size_t n = strlen(entry->d_name);
		if (n <= 4 || strcmp(entry->d_name + n - 4, ".ini") != 0) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(n - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
		}
		languages++;
	}
	closedir(dir);
	assert(languages == 10);
	assert(lang_set("English"));
}

/* ---- small helpers ------------------------------------------------------- */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *button_of_label(lv_obj_t *root, const char *text) {
	lv_obj_t *label = find_label(root, text);
	assert(label);
	lv_obj_t *button = lv_obj_get_parent(label);
	while (button && !lv_obj_check_type(button, &lv_button_class)) {
		button = lv_obj_get_parent(button);
	}
	assert(button);
	return button;
}

/* Nothing drawn may leave the panel sideways; downwards only inside a
 * container that scrolls that way. */
static void check_within_panel_in(lv_obj_t *obj, gui_config_t *cfg, const char *what, bool scrolls) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	bool overflow_x = area.x1 < 0 || area.x2 >= (int32_t)cfg->screen_width;
	bool overflow_y = area.y1 < 0 || area.y2 >= (int32_t)cfg->screen_height;
	if (overflow_x || (overflow_y && !scrolls)) {
		fprintf(stderr, "%s: an object at %d,%d..%d,%d is outside the %dx%d panel\n", what, area.x1, area.y1, area.x2,
				area.y2, (int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	if (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) {
		scrolls = true;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel_in(lv_obj_get_child(obj, i), cfg, what, scrolls);
	}
}

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("SONIX_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	static char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* ---- the page ------------------------------------------------------------- */

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/sonix-coverart-ui-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	check_strings(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	player_screen = lv_obj_create(NULL);
	coversearch_screen = lv_obj_create(NULL);
	coversearch_init(&cfg);

	snprintf(playing, sizeof(playing), "%s/01 - Song.flac", card);

	/* Artwork in the tags comes before any cover file: nothing is searched. */
	has_embedded = true;
	requests = 0;
	coversearch_start();
	assert(strcmp(last_toast, "plain:coverart_has_embedded") == 0 && requests == 0);
	has_embedded = false;

	/* No Wi-Fi: said so, nothing asked. */
	wifi_up = false;
	coversearch_start();
	assert(strcmp(last_toast, "error:coverart_no_wifi") == 0 && requests == 0);
	wifi_up = true;

	/* Wi-Fi off, with a radio to turn on: the question is put in place of the
	 * search, and answering it switches Wi-Fi on and waits for the link; the
	 * search starts by itself once it has an address. */
	wifi_up = false;
	wifi_enabled = false;
	requests = 0;
	coversearch_start();
	assert(strcmp(asked_title, "coverart_find_title") == 0 && asked_ok && requests == 0);
	asked_ok(NULL);
	assert(wifi_switched_on == 1 && strcmp(last_toast, "busy:coverart_wifi_connecting") == 0);
	lv_tick_inc(600);
	lv_timer_handler();
	assert(requests == 0); /* not connected yet */
	wifi_up = true;
	lv_tick_inc(600);
	lv_timer_handler();
	lv_screen_load(player_screen);
	run_posted();
	assert(lv_screen_active() == coversearch_screen);
	assert(requests == 1);
	lv_screen_load(player_screen);
	wifi_enabled = true;

	/* Wi-Fi off and nothing to turn on: told to connect. */
	wifi_up = false;
	wifi_enabled = false;
	radio_there = false;
	coversearch_start();
	assert(strcmp(last_toast, "error:coverart_no_wifi") == 0);
	radio_there = true;
	wifi_enabled = true;
	wifi_up = true;

	/* The search runs on a thread of its own and opens the list when it is
	 * done, over the player. */
	lv_screen_load(player_screen);
	coversearch_start();
	assert(lv_screen_active() == player_screen);
	run_posted();
	assert(lv_screen_active() == coversearch_screen);
	assert(player_return == coversearch_screen);

	/* The list: the album asked for first, each row finger-sized and on the
	 * glass. */
	lv_obj_update_layout(coversearch_screen);
	check_within_panel_in(coversearch_screen, &cfg, "Albums found", false);
	dump_snapshot(coversearch_screen, "v1-coverart-list");
	assert(find_label(coversearch_screen, tr("coverart_results")));
	lv_obj_t *year = find_label(coversearch_screen, "2005");
	assert(year);
	lv_obj_t *first = lv_obj_get_parent(year);
	assert(lv_obj_check_type(first, &lv_button_class));
	assert(lv_obj_get_height(first) >= 44);
	assert(lv_obj_get_width(first) >= (int32_t)cfg.screen_width - 3 * (int32_t)cfg.padding);
	assert(find_label(coversearch_screen, "2011"));

	/* A row downloads and shows its picture, with Apply under it; back
	 * returns to the list rather than leaving the page. */
	lv_obj_send_event(first, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == coversearch_screen);
	run_posted();
	lv_obj_update_layout(coversearch_screen);
	check_within_panel_in(coversearch_screen, &cfg, "Cover preview", false);
	dump_snapshot(coversearch_screen, "v1-coverart-preview");
	lv_obj_t *apply = button_of_label(coversearch_screen, tr("coverart_apply"));
	assert(!lv_obj_is_hidden(apply));
	assert(back_guard && back_guard());
	assert(lv_obj_is_hidden(lv_obj_get_parent(apply)));
	assert(!back_guard());

	/* Apply: the picture saved as cover.jpg beside the track, back in the
	 * player, and the player told to reload. */
	lv_obj_send_event(first, LV_EVENT_CLICKED, NULL);
	run_posted();
	lv_obj_send_event(apply, LV_EVENT_CLICKED, NULL);
	char cover[700];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", card);
	const char *saved = read_file(cover);
	assert(saved && strcmp(saved, JPEG) == 0);
	assert(refreshed == 1);
	assert(strcmp(last_toast, "success:coverart_saved") == 0);
	assert(lv_screen_active() == player_screen);
	char part[720];
	snprintf(part, sizeof(part), "%s.part", cover);
	assert(access(part, F_OK) != 0);

	/* The contact page: what is typed is kept, trimmed, and the page leaves. */
	lv_obj_t *contact_page = coversearch_contact_screen();
	assert(contact_page);
	lv_screen_load(contact_page);
	lv_obj_send_event(contact_page, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(contact_page);
	check_within_panel_in(contact_page, &cfg, "MusicBrainz contact", false);
	dump_snapshot(contact_page, "v1-coverart-contact");
	lv_obj_t *field = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(contact_page); i++) {
		lv_obj_t *child = lv_obj_get_child(contact_page, i);
		if (lv_obj_check_type(child, &lv_textarea_class)) {
			field = child;
		}
	}
	assert(field && keyboard_accept);
	lv_textarea_set_text(field, "  me@example.org ");
	keyboard_accept(NULL);
	assert(strcmp(config_get("coverart", "contact", ""), "me@example.org") == 0);
	assert(strcmp(last_toast, "success:coverart_contact_saved") == 0);
	assert(lv_screen_active() == player_screen);

	unlink(cover);
	unlink(config);
	rmdir(card);
	lv_deinit();
	printf("PASS cover art search (%s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720");
	return 0;
}
