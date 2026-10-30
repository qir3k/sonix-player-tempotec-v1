#include "artworkpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/addons/addons.h"
#include "src/system/core/lang.h"
#include "src/system/library/albumart.h"
#include "src/system/library/artsearch.h"
#include "src/system/library/tagwrite.h"
#include "src/system/net/wifi.h"

// The page the lyrics search has, for the picture instead of the words: the
// track is looked up in two public catalogues, what came back is listed with
// its picture in the row, and tapping one writes it beside the file.
//
// Two things are different from the lyrics page. The answers can be LOOKED at
// before one is chosen, so each row carries its own small picture, downloaded
// with the search on the same worker -- LVGL is only touched from this thread,
// so the bytes come back and are decoded here. And applying one writes a file
// on the card rather than words on the card, so it can fail for a card reason
// as well as a network one, and the player is asked to read its sleeve again
// the moment it succeeds.

lv_obj_t *artworkpage_screen;

#define ROW_RADIUS bp_pick(10, 12)
#define ROW_THUMB bp_pick(46, 64)
#define ROW_HEIGHT bp_pick(62, 92)

static lv_obj_t *title_label;
static lv_obj_t *list;
static lv_obj_t *note; // the one line in the middle while there are no rows

// The rows' pictures, decoded on the interface thread when a search lands.
// Owned here and freed when the list is rebuilt or the page is left: at this
// size a cover is a few hundred kilobytes, and eight of them are worth keeping
// account of on a device with this much memory.
static cover_image_t thumb_images[ARTSEARCH_MAX];

static artsearch_hit_t hits[ARTSEARCH_MAX];
static int hit_count;
static char track_path[1024];
// Whether the picture chosen on this page goes into the file's own tags rather
// than beside it. Decided once, when the page opened.
static bool writing_tags;

// One search, on a worker: the catalogue replies, then a small picture for each
// answer, which is what the rows draw.
typedef struct {
	char path[1024];
	char title[256];
	char artist[256];
	char album[256];
	artsearch_hit_t hits[ARTSEARCH_MAX];
	int count;
	void *thumbs[ARTSEARCH_MAX];
	size_t thumb_sizes[ARTSEARCH_MAX];
	char error[96];
} search_job_t;

// One picture being written: beside the file as a folder cover, or into the
// file's own tags when `into_tags` -- the second is the only way to change the
// sleeve of a file that carries one, and it is a module of its own.
typedef struct {
	char path[1024];
	char url[ARTSEARCH_URL_MAX];
	char error[96];
	bool into_tags;
	bool ok;
} apply_job_t;

static bool searching;
static bool applying;

static void apply_cb(lv_event_t *e);

static void set_note(const char *text, bool dim) {
	if (!note) {
		return;
	}
	lv_label_set_text(note, text);
	lv_obj_set_style_text_font(note, dim ? &font_ui_22 : &font_ui_24, 0);
	lv_obj_set_hidden(note, false);
}

static void note_hide(void) {
	if (note) {
		lv_obj_set_hidden(note, true);
	}
}

static void list_clear(void) {
	lv_obj_clean(list);
	for (int i = 0; i < ARTSEARCH_MAX; i++) {
		cover_free(&thumb_images[i]);
	}
	hit_count = 0;
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

static lv_obj_t *make_row(const artsearch_hit_t *hit, const cover_image_t *image, int index) {
	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, bp_pick(8, 16), 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, bp_pick(8, 12), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(row, apply_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *thumb = lv_image_create(row);
	lv_obj_set_size(thumb, ROW_THUMB, ROW_THUMB);
	lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_radius(thumb, 6, 0);
	lv_obj_set_style_clip_corner(thumb, true, 0);
	lv_obj_set_event_bubble(thumb, true);
	if (image && image->pixels) {
		lv_image_set_src(thumb, &image->dsc);
	} else {
		// No picture for this answer: the note it would have carried instead,
		// so the row keeps its shape and its picture slot.
		lv_image_set_src(thumb, &icon_music_note);
		lv_obj_add_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
	}

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(texts, bp_pick(2, 4), 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);

	lv_obj_t *album = lv_label_create(texts);
	lv_label_set_text(album, hit->album[0] ? hit->album : tr("artwork_results"));
	lv_label_set_long_mode(album, LV_LABEL_LONG_DOT);
	lv_obj_set_width(album, lv_pct(100));
	lv_obj_set_height(album, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(album, &theme_style_text, 0);
	lv_obj_set_style_text_font(album, &font_ui_22, 0);

	char detail[320];
	if (hit->year > 0) {
		snprintf(detail, sizeof(detail), "%s  ·  %d  ·  %s", hit->artist, hit->year, hit->source);
	} else {
		snprintf(detail, sizeof(detail), "%s  ·  %s", hit->artist, hit->source);
	}
	lv_obj_t *sub = lv_label_create(texts);
	lv_label_set_text(sub, detail);
	lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(sub, lv_pct(100));
	lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_18, 0);

	return row;
}

// ---------------------------------------------------------------------------
// writing the chosen one
// ---------------------------------------------------------------------------

static void apply_done(void *user) {
	apply_job_t *job = user;
	applying = false;
	toast_busy_end();
	if (!job->ok) {
		toast_error(job->error[0] ? job->error : "artwork_failed");
		free(job);
		return;
	}
	free(job);
	// The file beside the track changed, so the sleeve on screen is the old
	// one: the player is told to read it again, and the page goes back to it.
	player_cover_reload();
	toast_success("artwork_saved");
	back_btn_cb(NULL);
}

static void *apply_worker(void *arg) {
	apply_job_t *job = arg;
	char saved[1200];
	// Replacing is the point here: the row was chosen over the picture that
	// was already there, or there was none. Which of the two writes happens is
	// not re-decided here -- the page settled it when it opened, and a card
	// that changed under the search must not turn a tag write into a cover file
	// quietly.
	if (job->into_tags) {
		job->ok = artsearch_apply_tagged(job->path, job->url, job->error, sizeof(job->error));
	} else {
		job->ok = artsearch_apply(job->path, job->url, true, saved, sizeof(saved), job->error,
								  sizeof(job->error));
	}
	if (!gui_post(apply_done, job)) {
		free(job);
	}
	return NULL;
}

static void apply_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= hit_count) {
		return;
	}
	if (applying || searching) {
		toast_busy_dismissable(applying ? "artwork_saving" : "artwork_searching");
		return;
	}
	apply_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("artwork_failed");
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", track_path);
	snprintf(job->url, sizeof(job->url), "%s", hits[index].full_url);
	job->into_tags = writing_tags;
	applying = true;
	toast_busy("artwork_saving");
	pthread_t thread;
	if (pthread_create(&thread, NULL, apply_worker, job) != 0) {
		applying = false;
		toast_busy_end();
		toast_error("artwork_failed");
		free(job);
		return;
	}
	pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// the search
// ---------------------------------------------------------------------------

static void search_done(void *user) {
	search_job_t *job = user;
	searching = false;
	toast_busy_end();
	list_clear();
	hit_count = job->count > 0 ? job->count : 0;
	if (hit_count <= 0) {
		set_note(tr(job->error[0] ? job->error : "artwork_none"), true);
		free(job);
		return;
	}
	memcpy(hits, job->hits, sizeof(hits[0]) * (size_t)hit_count);
	for (int i = 0; i < hit_count; i++) {
		// The row's own copy, decoded here: the worker only fetched the bytes.
		if (job->thumbs[i]) {
			cover_load_image_memory(job->thumbs[i], job->thumb_sizes[i], ROW_THUMB, ROW_THUMB,
									COVER_FIT_COVER, &thumb_images[i]);
			free(job->thumbs[i]);
		}
		make_row(&hits[i], &thumb_images[i], i);
	}
	note_hide();
	lv_label_set_text(title_label, tr("artwork_results"));
	settingsrow_title_realign(title_label);
	free(job);
}

static void *search_worker(void *arg) {
	search_job_t *job = arg;
	job->count = artsearch_find(job->artist, job->album, job->title, job->hits, ARTSEARCH_MAX,
								job->error, sizeof(job->error));
	for (int i = 0; i < job->count; i++) {
		artsearch_fetch(job->hits[i].thumb_url, &job->thumbs[i], &job->thumb_sizes[i], NULL, 0);
	}
	if (!gui_post(search_done, job)) {
		for (int i = 0; i < ARTSEARCH_MAX; i++) {
			free(job->thumbs[i]);
		}
		free(job);
	}
	return NULL;
}

void artworkpage_open(const char *path, const char *title, const char *artist, const char *album) {
	if (!path || !path[0]) {
		toast_error("artwork_no_folder");
		return;
	}
	// A picture inside the file's own tags is the one the player draws and the
	// one a folder cover cannot override: replacing it means rewriting the tag.
	// With the artwork plugin switched on that is exactly what happens; without
	// it, saying so before the search is better than after it -- the answer
	// would be a file nobody ever sees. An OGG or an MP4 is refused on the same
	// grounds, by the writer's own answer about what it can rewrite.
	albumart_t embedded;
	writing_tags = albumart_load_candidate(path, 0, &embedded);
	if (writing_tags) {
		albumart_free(&embedded);
		if (!addons_enabled("artwork")) {
			toast_error("artwork_off");
			return;
		}
		if (!tagwrite_supported(path)) {
			toast_error("artwork_tag_unsupported");
			return;
		}
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		toast_error("artwork_no_wifi");
		return;
	}
	if (searching || applying) {
		toast_busy_dismissable(searching ? "artwork_searching" : "artwork_saving");
		return;
	}
	snprintf(track_path, sizeof(track_path), "%s", path);
	list_clear();
	lv_label_set_text(title_label, tr("find_artwork"));
	settingsrow_title_realign(title_label);
	set_note(tr("artwork_searching"), false);

	search_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("artwork_failed");
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", path);
	snprintf(job->title, sizeof(job->title), "%s", title ? title : "");
	snprintf(job->artist, sizeof(job->artist), "%s", artist ? artist : "");
	snprintf(job->album, sizeof(job->album), "%s", album ? album : "");

	searching = true;
	toast_busy("artwork_searching");
	player_sheet_close(false);
	switch_screen(artworkpage_screen);
	switcher_set_player_return(artworkpage_screen);

	pthread_t thread;
	if (pthread_create(&thread, NULL, search_worker, job) != 0) {
		searching = false;
		toast_busy_end();
		toast_error("artwork_failed");
		free(job);
		return;
	}
	pthread_detach(thread);
}

// Left, whichever way: the pictures go with the rows.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	list_clear();
	note_hide();
}

void artworkpage_init(gui_config_t *cfg) {
	lv_obj_add_style(artworkpage_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(artworkpage_screen, cfg, "find_artwork");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	int height = cfg->screen_height - content_top;

	list = lv_obj_create(artworkpage_screen);
	lv_obj_set_size(list, lv_pct(100), height);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_top(list, 0, 0);
	lv_obj_set_style_pad_bottom(list, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list, bp_pick(6, 8), 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);
	switcher_attach_back_gesture(list);

	note = lv_label_create(artworkpage_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_24, 0);
	lv_obj_align(note, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_hidden(note, true);

	switcher_attach_back_gesture(artworkpage_screen);
	lv_obj_add_event_cb(artworkpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
