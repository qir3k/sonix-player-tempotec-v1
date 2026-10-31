#include "radiopage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/system/addons/addons.h"
#include "src/gui/shell/azindex.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/region.h"
#include "src/system/library/library.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/streaming/radio.h"
#include "src/system/net/wifi.h"

lv_obj_t *radiopage_screen;
lv_obj_t *radiolist_screen;
lv_obj_t *radiosearch_screen;

// The list is windowed like every other long list here: a fixed pool of row
// widgets rides the scroll position. Two hundred stations is well past the
// point where building a widget per entry is felt on this hardware.
//
// 100 px was the row the 480-pixel panels drew: at the V1's 240x320 that
// pitch left two rows and a sliver of a third on screen, and in the terms
// lists -- languages, countries, genres -- where a row is one line of text
// and a chevron, most of each row was air. The height now follows the board
// the way the other lists do it (medialist.c's bp_pick(44, 100) is the same
// idea): 76 px still holds a station's two lines, the name over its quality
// badge, with the paddings and radius the rest of the V1's rows use.
#define ROW_HEIGHT bp_pick(76, 100)
#define ROW_GAP bp_pick(6, 8)
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS bp_pick(10, 12)
#define ROW_POOL 10

// The terms lists -- languages, countries, genres -- hold one line of text and
// a chevron, so on the V1 they take a row of their own: at the station height
// they were mostly air too, and they are the lists that read as too big. The
// station rows keep the height above, which their two lines need. On the
// regular panels both stay the 100 px they have always been.
#define TERM_ROW_HEIGHT bp_pick(56, 100)
#define TERM_ROW_GAP bp_pick(4, 8)
#define TERM_ROW_PITCH (TERM_ROW_HEIGHT + TERM_ROW_GAP)

#define POLL_MS 300

// What the list page is showing.
typedef enum {
	LIST_TERMS,	   // languages, countries or genres
	LIST_STATIONS, // the stations under one of those, or a search's hits
	LIST_FAVOURITES,
	LIST_RECENT,
	LIST_CUSTOM, // the stations written down in radio.txt
	LIST_FLYLO,  // Flylo's curated chill-hop picks
} list_mode_t;

// The lists that come from the card or from a fixed set rather than from the
// directory: no request is in flight for them, and they are reloaded rather
// than polled.
#define LIST_IS_STORED(m) ((m) == LIST_FAVOURITES || (m) == LIST_RECENT || (m) == LIST_CUSTOM || (m) == LIST_FLYLO)

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;	   // the station's own art, at the left of the row
	lv_obj_t *name;
	lv_obj_t *detail;  // the line under the name, on station rows
	lv_obj_t *quality; // ...the stream's quality badge on it
	lv_obj_t *bitrate; // ...the bitrate spelled out beside the badge
	lv_obj_t *code;	   // ...and the country code, on a search's hits
	lv_obj_t *chevron;
	lv_obj_t *menu_btn; // the ellipsis, on the favourites list only
	lv_obj_t *playmark; // the accent bar on the station that is loaded, radio.txt only
	int index;
	int pool;		  // this row's place in the pool, and so in the coverloader's slots
	cover_image_t art; // the decoded icon, owned here, or empty
	char art_url[256]; // the station favicon this row is showing, so a scroll does not re-stat the card
	char art_path[512]; // the cache file art was decoded from, or ""
	int art_state;	   // 0 nothing asked, 1 out with the loader, 2 settled
	bool art_refresh;  // the list was reopened: look for an icon that landed since
} row_t;

// The station's icon at the left of its row, the way every other list here
// shows a thumb: the same 36 px the music lists use, inside the 76 px row. The
// two lines of text beside it need the rest, and the playmark's gutter already
// owns the first 14 px.
#define RADIO_ICON_SIZE bp_pick(36, 36)

// The mark on the loaded station, as the track lists draw it on the track that
// is playing (medialist.c).
#define PLAYMARK_WIDTH bp_pick(4, 6)
#define PLAYMARK_HEIGHT bp_pick(36, 52)
#define PLAYMARK_INSET 4 // from the row's left edge

// The band of the row the mark owns, and what its right edge costs the text.
// It used to live inside ROW_PAD_HOR alone, which left the bar's rounded end
// running under the first letters of the name on every row that showed it: the
// mark now takes a gutter out of the row's LEFT padding and nothing out of the
// right, so the bar has its own column and the name, the badge and the bitrate
// all start clear of it -- with the row still the full width it was.
#define PLAYMARK_GUTTER (PLAYMARK_INSET + PLAYMARK_WIDTH + 4)
#define ROW_PAD_HOR bp_pick(10, 14)

// radio.txt's headings: a line of dim text over the first station of each
// group, in the gap the group's rows are pushed down by. Only that list has
// them, so only that list's rows are placed from a table rather than at a
// fixed pitch.
#define HEADING_HEIGHT bp_pick(44, 56)
static int custom_row_y[RADIO_CUSTOM_MAX]; // where each station row sits
static int custom_list_height;
static lv_obj_t *heading_labels[RADIO_CUSTOM_MAX];
static int heading_count;

// The radio.txt line the rows were last marked for, -1 for none.
static int marked_custom = -1;
// How many more poll ticks the Flylo list should keep looking for icons that
// its own worker is still fetching. Zero for every other list.
static int flylo_art_ticks;

// The station the other lists were last marked for -- the directory's, the
// starred and the recent ones: its uuid, or its address when it has none.
// Empty when no station is loaded.
static char marked_station[RADIO_URL_MAX];

static gui_config_t *g_cfg;

// --- the hub ---
//
// One list of the places the page offers, in an order and with a visibility the
// user chooses in Radio settings. Recently played used to be a corner button
// beside the heart and the search; it is a row among the categories now, where
// it belongs -- it is a list of stations, like the others.
#define HUB_ROWS 8
enum {
	HUB_RECENT = 0,
	HUB_LANGUAGE,
	HUB_COUNTRY,
	HUB_GENRE,
	HUB_CUSTOM,
	HUB_RECOMMENDED,
	// The starred stations, added last -- after Recommended, not between it and
	// Custom. The ids are what the saved hidden mask is indexed by, so a row
	// added in the middle would move every bit after it: a player that had
	// hidden Recommended would find Favourites hidden instead and Recommended
	// back on. The order the page shows them in is hub_order[] and is set
	// below.
	HUB_FAVOURITES,
	// Flylo's curated picks, appended for the same reason Favourites was: the
	// ids index the saved hidden mask, so a row added in the middle would move
	// every bit after it.
	HUB_FLYLO,
};

static const char *const HUB_SLUGS[HUB_ROWS] = {"recent", "language", "country", "genre", "custom", "recommended",
												"favourites", "flylo"};
static const char *const HUB_LABELS[HUB_ROWS] = {"radio_recently_played", "language", "radio_country", "genre",
												 "radio_custom", "radio_recommended", "favourites", "radio_flylo_favs"};

// The order a page that has never been arranged starts in: the starred stations
// are the second thing offered, right under what was played last, and
// Recommended is beside them.
static const int HUB_DEFAULT_ORDER[HUB_ROWS] = {HUB_RECENT, HUB_FAVOURITES, HUB_FLYLO, HUB_RECOMMENDED,
											  HUB_LANGUAGE, HUB_COUNTRY, HUB_GENRE, HUB_CUSTOM};

static lv_obj_t *hub_container;
static lv_obj_t *hub_wifi_warning;
static lv_obj_t *hub_wifi_row;
static lv_obj_t *hub_rows[HUB_ROWS];
static lv_obj_t *hub_search_btn;
static lv_obj_t *hub_settings_btn;

static int hub_order[HUB_ROWS];
static bool hub_hidden[HUB_ROWS];

// --- Radio settings ---
static lv_obj_t *radio_settings_screen;
static settingsrow_duration_t radio_sleep_row;
static lv_obj_t *radio_region_value;	 // the row that shows the chosen country
static lv_obj_t *radio_quality_pills[3]; // any bitrate, 128 and up, 320 and up

// The arrangement of the hub's rows, on a page of its own (Radio settings >
// List reorder).
static lv_obj_t *radio_order_screen;
static lv_obj_t *radio_order_rows;

// --- The region ---
//
// The country the lists are kept to, chosen from the one shared list in
// region.h: Worldwide first, then one row per language the firmware ships.
// Nothing is fetched to draw it, so the page opens with no network at all, and
// the choice it writes is the same one the podcasts and the YouTube Music
// settings read. What it buys is that a Polish listener is not offered a
// station in Texas unless they ask for one.
static lv_obj_t *region_screen;
static lv_obj_t *region_container;
static region_t region_rows[REGION_LIST_MAX];
static int region_count;

// --- the search page ---
static lv_obj_t *search_field;
static lv_obj_t *search_history_list;
static lv_obj_t *search_history_empty;
static keyboard_t *search_keyboard;
static lv_timer_t *hub_timer;

// --- the list ---
static lv_obj_t *list_view;
static lv_obj_t *list_body;
static lv_obj_t *list_message; // "Loading...", "Nothing here", the error
static lv_obj_t *list_title;
static lv_obj_t *sort_btn;	// A-Z / Z-A, on the three category lists
static lv_obj_t *sort_icon;
static azindex_t *list_index; // the letters down the right edge, same lists
static row_t rows[ROW_POOL];
static lv_timer_t *list_timer;

static list_mode_t list_mode;
static radio_browse_t list_browse;

// The back step inside the page. The categories (languages, countries, genres)
// and their stations live on the same screen: entering a genre fills the list
// with stations without changing screen, so as far as the global history is
// concerned nothing happened, so the chevron would leave for the radio's main
// page instead of returning to the genre list. Same arrangement as the Qobuz,
// Tidal and podcast lists: a guard that consumes the step inside the page, plus
// its peek for the swipe.
static bool stations_from_terms;	 // the listed stations came from a category
static char terms_title[96];		 // the category list's title, to return to it
static bool list_back_has_step(void);
static bool list_back_guard(void);
static unsigned last_job_serial;

static radio_term_t terms[300];
static int term_count;


// Four pages held at once. A country can hold thousands of stations, so the
// list is paged rather than capped: the next hundred are asked for when the
// user scrolls near the bottom. Four hundred rows is not a ceiling anyone
// reaches by dragging a finger, and it is under half a megabyte.
#define RADIO_MAX_HELD (4 * RADIO_PAGE_SIZE)
static radio_station_t stations[RADIO_MAX_HELD];
static int station_count;

// What the current station list is, so the next page can be asked for.
static char page_value[128];	  // the term, or the search text
static bool page_is_search;
static bool page_is_recommended; // the list is the region's most played
static int page_offset;			  // where the next page starts, in the server's terms
static bool page_more;			  // the last page came back full: there may be more
static bool page_loading;		  // a page is in flight
static bool page_append;		  // ...and it is a continuation, not a fresh list

static void list_window_update(void);
static void list_reload_stored(void);
static bool list_has_headings(void);
static const char *stored_empty_message(void);

// ---------------------------------------------------------------------------

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine with a network and no wlan0 to ask
	// about, so the gate would be permanently shut and the page untestable.
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void hide(lv_obj_t *o) {
	if (o) {
		lv_obj_set_hidden(o, true);
	}
}
static void show(lv_obj_t *o) {
	if (o) {
		lv_obj_set_hidden(o, false);
	}
}

// ---------------------------------------------------------------------------
// the list page
// ---------------------------------------------------------------------------

static int list_count(void) {
	switch (list_mode) {
	case LIST_TERMS:
		return term_count;
	case LIST_STATIONS:
	case LIST_FAVOURITES:
	default:
		return station_count;
	}
}

// What an empty list of stations should say.
//
// "Nothing found" is the wrong answer when the list is empty because of a cut
// the user chose in Radio settings: the directory answered, the answer was
// thrown away. The filtered sentence says which two settings to look at.
static const char *empty_stations_message(void) {
	return (radio_region()[0] || radio_min_bitrate() > 0) ? "radio_no_stations_filtered" : "radio_no_stations_found";
}

static void set_message(const char *text) {
	if (!list_message) {
		return;
	}
	if (!(text && text[0])) {
		hide(list_message);
		return;
	}
	lv_label_set_text(list_message, tr(text));
	show(list_message);

	// Vertically centred in the part of the panel the list would have used.
	// It used to sit at a fixed height from the top, which was right for the
	// one message it was drawn for and wrong for the ones that followed: a
	// single line of "Nothing found" reads as something left behind, and the
	// two sentences about the filters were worse -- off the middle, with no
	// list under them to explain the space.
	lv_obj_update_layout(list_view);
	int32_t view;
	if (list_has_headings()) {
		view = custom_list_height;
	} else {
		view = lv_obj_get_content_height(list_view);
	}
	int32_t here = lv_obj_get_height(list_message);
	int32_t above = (view - here) / 2;
	lv_obj_align(list_message, LV_ALIGN_TOP_MID, 0, above > 0 ? above : 0);
}

// ---------------------------------------------------------------------------
// The quality badge
//
// One glyph in four colours, from what the directory says the stream is: MP3
// or AAC, and a low or a high bitrate. High starts at 192 kbps for MP3 and at
// 160 for AAC, which holds as much at a lower rate. A station whose codec is
// something else, or whose bitrate the directory does not know, wears none: a
// colour that means "unknown" would sit on most of the rows and say nothing.
// ---------------------------------------------------------------------------

typedef enum {
	QUALITY_NONE = -1,
	QUALITY_MP3_LOW,
	QUALITY_MP3_HIGH,
	QUALITY_AAC_LOW,
	QUALITY_AAC_HIGH,
	QUALITY_COUNT,
} station_quality_t;

// Light theme, then dark: the same four, a step lighter on the dark cards.
static const uint32_t QUALITY_COLOURS[2][QUALITY_COUNT] = {
	{0xC47A5A, 0x6A9B78, 0xB56F7C, 0x5B9696},
	{0xD99A7A, 0x82B894, 0xD18A98, 0x78B5B5},
};

// Whether `codec` names AAC in any of the directory's spellings: "AAC",
// "AAC+", "aac", "HE-AAC".
static bool codec_is_aac(const char *codec) {
	for (const char *c = codec; c[0] && c[1] && c[2]; c++) {
		if (strncasecmp(c, "AAC", 3) == 0) {
			return true;
		}
	}
	return false;
}

static station_quality_t station_quality(const radio_station_t *s) {
	if (s->bitrate <= 0) {
		return QUALITY_NONE;
	}
	if (strncasecmp(s->codec, "MP3", 3) == 0) {
		return s->bitrate >= 192 ? QUALITY_MP3_HIGH : QUALITY_MP3_LOW;
	}
	if (codec_is_aac(s->codec)) {
		return s->bitrate >= 160 ? QUALITY_AAC_HIGH : QUALITY_AAC_LOW;
	}
	return QUALITY_NONE;
}

// ---------------------------------------------------------------------------
// The category lists in order
//
// Languages, countries and genres are listed alphabetically, either way round,
// in the music library's own collation: accents folded, a leading article
// skipped when the library skips them. The letters of the A-Z strip come from
// the same collation, so each points at the rows it names.
// ---------------------------------------------------------------------------

// Which of the three lists run Z-A, one bit per radio_browse_t, kept across
// restarts like the track lists' direction.
static bool terms_desc(radio_browse_t kind) {
	return (config_get_int("radio", "sort_desc", 0) & (1L << kind)) != 0;
}

static void terms_set_desc(radio_browse_t kind, bool desc) {
	long mask = config_get_int("radio", "sort_desc", 0);
	mask = desc ? (mask | (1L << kind)) : (mask & ~(1L << kind));
	config_set_int("radio", "sort_desc", mask);
	config_save();
}

// The sort keys of the terms being sorted: qsort takes no context.
static char (*sort_keys)[LIBRARY_SORT_KEY_MAX];

static int term_order(const void *a, const void *b) {
	int x = *(const int *)a;
	int y = *(const int *)b;
	int order = strcmp(sort_keys[x], sort_keys[y]);
	return order ? order : x - y;
}

// Puts terms[] in order and tells the strip where each letter starts.
static void terms_sort(void) {
	bool desc = terms_desc(list_browse);
	int first[LIBRARY_INDEX_BUCKETS];
	for (int i = 0; i < LIBRARY_INDEX_BUCKETS; i++) {
		first[i] = -1;
	}

	sort_keys = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*sort_keys));
	int *order = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*order));
	radio_term_t *sorted = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*sorted));
	if (!sort_keys || !order || !sorted) {
		free(sort_keys);
		free(order);
		free(sorted);
		sort_keys = NULL;
		azindex_set_rows(list_index, NULL, 0, false);
		return;
	}

	for (int i = 0; i < term_count; i++) {
		library_sort_key(terms[i].label, sort_keys[i], sizeof(sort_keys[i]));
		order[i] = i;
	}
	qsort(order, (size_t)term_count, sizeof(*order), term_order);

	for (int i = 0; i < term_count; i++) {
		int from = order[desc ? term_count - 1 - i : i];
		sorted[i] = terms[from];
		int bucket = library_index_slot(library_index_letter_of_key(sort_keys[from]));
		// The first row of a letter is where it starts reading down, whichever
		// way round the list runs.
		if (first[bucket] < 0) {
			first[bucket] = i;
		}
	}
	memcpy(terms, sorted, (size_t)term_count * sizeof(*terms));

	free(sort_keys);
	free(order);
	free(sorted);
	sort_keys = NULL;

	azindex_set_rows(list_index, first, term_count, desc);
}

// The corner button and the strip belong to the category lists only: a list
// of stations is in the directory's order, most played first, and has no
// letters to jump between.
static void sort_button_update(void) {
	if (!sort_btn) {
		return;
	}
	if (list_mode == LIST_TERMS) {
		lv_image_set_src(sort_icon, terms_desc(list_browse) ? &icon_sort_za : &icon_sort_az);
		lv_obj_set_hidden(sort_btn, false);
	} else {
		lv_obj_set_hidden(sort_btn, true);
		azindex_set_rows(list_index, NULL, 0, false);
	}
}

static const char *station_key(const radio_station_t *s) { return s->uuid[0] ? s->uuid : s->url; }

// Reads which station is loaded, playing or stopped. True when the answer
// differs from the one the rows were marked with.
static bool marked_station_update(void) {
	radio_station_t now;
	char key[sizeof(marked_station)] = "";
	if (radio_current_station(&now)) {
		snprintf(key, sizeof(key), "%s", station_key(&now));
	}
	if (strcmp(key, marked_station) == 0) {
		return false;
	}
	snprintf(marked_station, sizeof(marked_station), "%s", key);
	return true;
}

// radio.txt marks its line by position, since the same address can be written
// down twice; every other list marks the station by who it is, so the one
// playing is found again in a search, in the starred and in the recent alike.
static void row_update_playmark(row_t *row) {
	bool marked = false;
	if (row->index >= 0 && row->index < list_count()) {
		if (list_mode == LIST_CUSTOM) {
			marked = row->index == marked_custom;
		} else if (list_mode != LIST_TERMS && marked_station[0]) {
			marked = strcmp(station_key(&stations[row->index]), marked_station) == 0;
		}
	}
	if (marked) {
		show(row->playmark);
	} else {
		hide(row->playmark);
	}
}

static bool list_has_headings(void) { return list_mode == LIST_CUSTOM && heading_count > 0; }

// One row's height and pitch for the list showing. Every place that places,
// measures or windows a row asks these two, so the pooled buttons, the A-Z
// strip and the scroll arithmetic cannot disagree about how tall a row is.
static int row_height(void) { return list_mode == LIST_TERMS ? TERM_ROW_HEIGHT : ROW_HEIGHT; }
static int row_pitch(void) { return list_mode == LIST_TERMS ? TERM_ROW_PITCH : ROW_PITCH; }

static int row_y(int index) {
	if (list_has_headings() && index >= 0 && index < RADIO_CUSTOM_MAX) {
		return custom_row_y[index];
	}
	return index * row_pitch();
}

static int list_height(int count) {
	if (list_has_headings()) {
		return custom_list_height;
	}
	return count > 0 ? count * row_pitch() : row_pitch();
}

// The headings of radio.txt, made again with the list. The labels belong to
// this list alone, so any other list clears them.
static void headings_layout(void) {
	for (int i = 0; i < heading_count; i++) {
		lv_obj_delete(heading_labels[i]);
	}
	heading_count = 0;
	custom_list_height = 0;
	if (list_mode != LIST_CUSTOM) {
		return;
	}

	int line = lv_font_get_line_height(&font_ui_22);
	int width = lv_obj_get_style_width(list_body, LV_PART_MAIN);
	int y = 0;
	for (int i = 0; i < station_count && i < RADIO_CUSTOM_MAX; i++) {
		char text[RADIO_NAME_MAX];
		if (radio_custom_heading(i, text, sizeof(text))) {
			lv_obj_t *label = lv_label_create(list_body);
			lv_label_set_text(label, text);
			lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
			lv_obj_set_width(label, width - 2 * ROW_PAD_HOR);
			lv_obj_add_style(label, &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(label, &font_ui_22, 0);
			// Close over its own rows rather than midway in the gap, so it
			// reads as their title and not as the previous group's last line.
			lv_obj_set_pos(label, PLAYMARK_GUTTER / 2, y + HEADING_HEIGHT - line - 10);
			lv_obj_set_clickable(label, false);
			heading_labels[heading_count++] = label;
			y += HEADING_HEIGHT;
		}
		custom_row_y[i] = y;
		y += ROW_PITCH;
	}
	custom_list_height = y > 0 ? y : ROW_PITCH;
}

// The station's icon, if this one has played here before and its picture is
// still in <card>/.local/radio-art. There is no download in this path: the list
// binds ten rows on the UI thread and a network round trip there is the one
// thing the page must not do. A station that has never played shows the same
// radio glyph the player uses for a stream with no artwork, and the real icon
// appears the next time the list is built after that station plays once.
static int radio_icon_placeholder_scale(void) {
	int side = LV_MAX((int)icon_radio_player.header.w, (int)icon_radio_player.header.h);
	return side > 0 ? (LV_SCALE_NONE * RADIO_ICON_SIZE) / side : LV_SCALE_NONE;
}

static void row_art_placeholder(row_t *row) {
	lv_image_set_src(row->icon, &icon_radio_player);
	lv_image_set_scale(row->icon, (uint32_t)radio_icon_placeholder_scale());
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// Takes the picture off the widget before freeing it: LVGL keeps drawing from
// the buffer it was last handed, and the row is rebuilt under a finger during a
// scroll. Then lets the loader forget the request, whatever stage it was at.
static void row_art_drop(row_t *row) {
	if (!row->icon) {
		return; // a row before its widgets exist: nothing to drop
	}
	lv_image_set_src(row->icon, NULL);
	coverloader_release(COVERLOADER_RADIO_BASE + row->pool);
	cover_free(&row->art);
	row->art_url[0] = '\0';
	row->art_path[0] = '\0';
	row->art_state = 0;
}

static void row_art_bind(row_t *row, const radio_station_t *station) {
	// The station's favicon URL, not the file: the row is rebound on every
	// scroll tick, and the two stats radio_station_art_path() makes must not
	// run ten times a frame while a finger moves. Scrolling to a station whose
	// picture is in hand, or whose search has already been made, touches
	// nothing; only opening the list again (art_refresh) looks at the card once
	// more, for an icon the station wrote when it played since.
	bool same = strcmp(station->favicon, row->art_url) == 0;
	if (same && (row->art.pixels || !row->art_refresh)) {
		row->art_refresh = false;
		if (row->art.pixels) {
			lv_image_set_src(row->icon, &row->art.dsc);
			lv_image_set_scale(row->icon, LV_SCALE_NONE);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
		} else {
			row_art_placeholder(row);
		}
		return;
	}

	// A different station, the first bind, or a reopened list with no picture:
	// drop whatever the row held, then ask for the new picture when there is
	// one to ask for.
	row->art_refresh = false;
	row_art_drop(row);
	snprintf(row->art_url, sizeof(row->art_url), "%s", station->favicon);
	radio_station_art_path(station->favicon, row->art_path, sizeof(row->art_path));
	row_art_placeholder(row);
	if (row->art_path[0]) {
		coverloader_request_image_file(COVERLOADER_RADIO_BASE + row->pool, row->art_path, RADIO_ICON_SIZE);
		row->art_state = 1;
	}
}

static void row_art_collect(row_t *row) {
	if (row->art_state != 1) {
		return;
	}
	cover_image_t image = {0};
	bool finished = false;
	bool found = coverloader_take(COVERLOADER_RADIO_BASE + row->pool, &image, &finished);
	if (!finished) {
		return;
	}
	row->art_state = 2;
	if (!found) {
		return;
	}
	// Off the widget first: LVGL draws from the buffer it was handed, and freeing
	// it while it is still the icon's source is a use-after-free on the next
	// frame. (Usually empty -- a request is only made with nothing in hand --
	// but the row is a pooled widget and this is the cheap guard.)
	if (row->art.pixels) {
		lv_image_set_src(row->icon, NULL);
	}
	cover_free(&row->art);
	row->art = image;
	// Bound to a station when it lands, and not hidden: the row may have been
	// hidden or rebound while the decode was out, and its new station's art
	// must not be replaced by this one.
	if (row->index >= 0 && !lv_obj_is_hidden(row->button)) {
		lv_image_set_src(row->icon, &row->art.dsc);
		lv_image_set_scale(row->icon, LV_SCALE_NONE);
		lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
	}
}

// Runs on the list's poll timer: a finished picture is put up without waiting
// for the next bind.
static void rows_art_update(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_art_collect(&rows[i]);
	}
}

// The page was opened again: a station that played while it was closed has
// written its icon to the cache since, so each row looks at the card once more
// on its next bind. Not done for a page appended to (scrolling), where nothing
// has had time to change and ten re-stats would only cost the scroll.
static void rows_art_refresh(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].art_refresh = true;
	}
}

static void row_bind(row_t *row, int index) {
	row->index = index;
	row_update_playmark(row);

	if (index < 0 || index >= list_count()) {
		hide(row->button);
		return;
	}

	show(row->button);
	lv_obj_set_y(row->button, row_y(index));
	// The pool is shared between the station rows and the terms rows, so the
	// height is set on every bind rather than once when the pool is built.
	lv_obj_set_height(row->button, row_height());

	if (list_mode == LIST_TERMS) {
		hide(row->icon); // a language or a country has no picture
		lv_label_set_text(row->name, terms[index].label);
		hide(row->detail);
		show(row->chevron);
		hide(row->menu_btn);
		return;
	}

	show(row->icon);
	const radio_station_t *s = &stations[index];
	row_art_bind(row, s);
	lv_label_set_text(row->name, s->name);

	// Under the name: the quality badge, and on a search's hits the country
	// code, since a search crosses every country and the same name can be a
	// station in three of them. The line goes when there is neither.
	station_quality_t quality = station_quality(s);
	if (quality != QUALITY_NONE) {
		uint32_t colour = QUALITY_COLOURS[theme_is_dark() ? 1 : 0][quality];
		lv_obj_set_style_image_recolor(row->quality, lv_color_hex(colour), 0);
		show(row->quality);
	} else {
		hide(row->quality);
	}
	// The bitrate beside the badge: "320 kbps", and nothing at all when the
	// directory does not know one -- a blank space there would read as a
	// station that is low quality rather than one that is unlisted.
	if (s->bitrate > 0) {
		lv_label_set_text_fmt(row->bitrate, "%d kbps", s->bitrate);
		show(row->bitrate);
	} else {
		hide(row->bitrate);
	}
	bool with_code = page_is_search && list_mode == LIST_STATIONS && s->country[0];
	if (with_code) {
		lv_label_set_text(row->code, s->country);
		show(row->code);
	} else {
		hide(row->code);
	}
	if (quality != QUALITY_NONE || s->bitrate > 0 || with_code) {
		show(row->detail);
	} else {
		hide(row->detail);
	}

	// A station that cannot be played is drawn faint: still there, still
	// tappable (it says why), but plainly not one of the ones that will. Since
	// the row carries the name alone, the fading is the only sign -- the reason
	// in full comes from the popup on tap.
	bool unplayable = radio_station_problem(s) != NULL;
	lv_obj_set_style_opa(row->button, unplayable ? LV_OPA_50 : LV_OPA_COVER, 0);

	hide(row->chevron);

	// The ellipsis is only on the favourites: it is the one list where a row
	// can be taken away, and a menu with nothing in it on the others would be
	// a button that does nothing.
	if (list_mode == LIST_FAVOURITES) {
		show(row->menu_btn);
	} else {
		hide(row->menu_btn);
	}
}

// `keep_scroll` is what tells a page appended to the bottom from a list
// replaced outright: jumping back to the top after loading more would undo
// the scroll that asked for it.
static void list_rebuild_keep(bool keep_scroll) {
	int count = list_count();
	marked_station_update(); // the rows below are marked from it

	headings_layout();
	lv_obj_set_height(list_body, list_height(count));
	if (!keep_scroll) {
		lv_obj_scroll_to_y(list_view, 0, LV_ANIM_OFF);
	}

	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2; // force every row to rebind
	}
	list_window_update();
}

static void list_rebuild(void) { list_rebuild_keep(false); }

static void list_window_update(void) {
	int count = list_count();
	if (count <= 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(list_view);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / row_pitch()) - 1;
	if (list_has_headings()) {
		// The first row whose bottom is below the top of the view, and one
		// before it, as the fixed pitch would give.
		first = 0;
		while (first < count && custom_row_y[first] + ROW_HEIGHT <= scroll) {
			first++;
		}
		first--;
	}
	if (first + ROW_POOL > count) {
		first = count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < count ? index : -1);
	}
}

// Asks for the next hundred. Called when the scroll comes within a screenful
// of the bottom, and does nothing when there is nothing more to ask for.
static void list_load_more(void) {
	if (page_loading || !page_more || list_mode != LIST_STATIONS) {
		return;
	}
	if (station_count >= RADIO_MAX_HELD) {
		page_more = false;
		return;
	}

	page_loading = true;
	page_append = true;
	last_job_serial = radio_job_serial();

	// page_offset, not station_count: the two drift apart as soon as one
	// station is dropped -- for being https-only, for being below the quality
	// bar, for being a duplicate -- and asking from the wrong place means rows
	// repeated or skipped.
	if (page_is_search) {
		radio_request_search(page_value, page_offset);
	} else if (page_is_recommended) {
		radio_request_recommended(page_offset);
	} else {
		radio_request_stations(list_browse, page_value, page_offset);
	}
}

static void list_scroll_cb(lv_event_t *e) {
	(void)e;

	// A sideways drag that is pulling the player in moves this list too, and
	// acting on that movement would ask the directory for another page every
	// time the user reached for the player. Same guard the other list pages
	// use.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	list_window_update();
	if (list_mode == LIST_TERMS) {
		azindex_flash(list_index);
	}

	// Within a screenful of the end is close enough: the request takes a
	// moment, and arriving at the last row to find the list simply stops is
	// exactly what this replaces.
	int scroll = lv_obj_get_scroll_y(list_view);
	int viewport = lv_obj_get_height(list_view);
	if (scroll + 2 * viewport >= station_count * row_pitch()) {
		list_load_more();
	}
}

static void row_clicked_cb(lv_event_t *e) {
	// The release at the end of a sideways drag lands on whatever row the
	// finger happens to be over, so without this pulling the player in would
	// start that station. Same guard every other list page carries.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *button = lv_event_get_current_target(e);

	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			index = rows[i].index;
			break;
		}
	}
	if (index < 0 || index >= list_count()) {
		return;
	}

	if (list_mode == LIST_TERMS) {
		const char *value = terms[index].name;
		snprintf(terms_title, sizeof(terms_title), "%s", lv_label_get_text(list_title));
		lv_label_set_text(list_title, terms[index].label);
		settingsrow_title_realign(list_title);
		stations_from_terms = true;

		list_mode = LIST_STATIONS;
		station_count = 0;
		term_count = 0;
		page_is_search = false;
		page_is_recommended = false;
		page_offset = 0;
		page_more = false;
		page_append = false;
		page_loading = true;
		snprintf(page_value, sizeof(page_value), "%s", value);
		sort_button_update();
		list_rebuild();
		set_message("loading");

		last_job_serial = radio_job_serial();
		radio_request_stations(list_browse, page_value, 0);
		return;
	}

	// A station that cannot be played says why, here, instead of being started
	// and failing silently.
	const char *problem = radio_station_problem(&stations[index]);
	if (problem) {
		gui_notify_popup(problem);
		return;
	}

	// Otherwise start it and go to the player: connecting takes a moment, and
	// the player is where that moment is explained. The list goes with it, for
	// previous and next to walk; radio.txt has its own.
	if (list_mode != LIST_CUSTOM) {
		radio_set_list(stations, station_count);
	}
	player_pause_for_loading();
	if (radio_play(&stations[index])) {
		player_refresh_now_playing();
		player_sheet_open(true);
	} else {
		gui_notify_popup("radio_cannot_start_the_station");
	}
}

// ---------------------------------------------------------------------------
// Removing a favourite
//
// The uuid rather than the row index: the confirmation runs after the card has
// closed, and by then the list may have been rebuilt underneath it.
// ---------------------------------------------------------------------------

static char menu_uuid[40];
static char menu_name[RADIO_NAME_MAX];

static void remove_confirmed(void *user) {
	(void)user;
	if (!menu_uuid[0]) {
		return;
	}

	radio_fav_remove(menu_uuid);
	menu_uuid[0] = '\0';

	if (list_mode == LIST_FAVOURITES) {
		list_reload_stored();
		list_rebuild();
		set_message(station_count ? NULL : stored_empty_message());
	}

	// The player's own star is about the same station.
	player_refresh_now_playing();
}

static void menu_remove_action(void *user) {
	(void)user;
	if (!menu_uuid[0]) {
		return;
	}
	confirm_show("remove_from_favourites", menu_name, "remove", remove_confirmed, NULL);
}

static void menu_btn_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);

	lv_obj_t *button = lv_event_get_current_target(e);
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].menu_btn == button) {
			index = rows[i].index;
			break;
		}
	}
	if (index < 0 || index >= station_count) {
		return;
	}

	snprintf(menu_uuid, sizeof(menu_uuid), "%s", stations[index].uuid);
	snprintf(menu_name, sizeof(menu_name), "%s", stations[index].name);

	static const popover_item_t items[] = {
		{"remove_from_favourites", menu_remove_action, NULL},
	};
	popover_show(button, items, 1);
}

// Flylo's curated chill-hop and lo-fi picks. The addresses are the public
// stream URLs for each station; the names are what shows on the list.
//
// These are not a file on the card and not a network directory -- they are
// built into the page, so the list is always there and never needs a refresh.
//
// `id` is what gives a station here an identity of its own: favourites and the
// recent list are kept by uuid, and a station with none could not be starred at
// all -- the button did nothing. `icon` is the station's own logo, downloaded
// the first time the list is opened (radio_art_prefetch) rather than only when
// it has been listened to.
typedef struct {
	const char *id;
	const char *name;
	const char *url;
	const char *icon;
} flylo_station_t;

static const flylo_station_t FLYLO_STATIONS[] = {
	{"ilove-chillhop", "I Love Chillhop", "https://streams.ilovemusic.de/iloveradio17.mp3",
	 "https://ilovemusic.de/favicon.png"},
	{"ilove-chillhop-2", "ChillHop", "http://stream01.iloveradio.de/iloveradio17.mp3",
	 "https://ilovemusic.de/favicon.png"},
	{"fluxfm-chillhop", "Flux Fm Chillhop", "https://streams.fluxfm.de/Chillhop/mp3-320/streams.fluxfm.de/",
	 "https://www.fluxfm.de/assets/favicons/apple-icon-180x180.png"},
	{"le-mellotron", "Le Mellotron", "https://listen.radioking.com/radio/477719/stream/534044",
	 "https://cdn.prod.website-files.com/62288d6c9e6c33efb4e67d74/"
	 "622895c09e6c33d606e6e822_Icone-webclip.png"},
	{"zeno-lofi", "Lo-Fi Hip-Hop Radio", "https://stream.zeno.fm/0r0xa792kwzuv",
	 "https://zeno.fm/icon-192x192.png"},
};
#define FLYLO_STATION_COUNT (int)(sizeof(FLYLO_STATIONS) / sizeof(FLYLO_STATIONS[0]))

// The two lists that come out of the database rather than off the network, and
// the one that is built into the page.
static void list_reload_stored(void) {
	term_count = 0;
	station_count = 0;

	int max = (int)(sizeof(stations) / sizeof(stations[0]));
	int n;
	switch (list_mode) {
	case LIST_RECENT:
		n = radio_recent_count();
		break;
	case LIST_CUSTOM:
		n = radio_custom_count();
		break;
	case LIST_FLYLO:
		n = FLYLO_STATION_COUNT;
		break;
	default:
		n = radio_fav_count();
		break;
	}
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		bool got = false;
		switch (list_mode) {
		case LIST_RECENT:
			got = radio_recent_get(i, &stations[station_count]);
			break;
		case LIST_CUSTOM:
			got = radio_custom_get(i, &stations[station_count]);
			break;
		case LIST_FLYLO:
			// Flylo stations are not in the directory, so nothing has ever given
			// them a uuid -- and a favourite is stored by one, which is why the
			// star did nothing here. The id in the table is theirs, stable for
			// as long as the entry is.
			memset(&stations[station_count], 0, sizeof(stations[station_count]));
			snprintf(stations[station_count].uuid, sizeof(stations[station_count].uuid),
					 "flylo:%s", FLYLO_STATIONS[i].id);
			snprintf(stations[station_count].name, sizeof(stations[station_count].name), "%s",
					 FLYLO_STATIONS[i].name);
			snprintf(stations[station_count].url, sizeof(stations[station_count].url), "%s",
					 FLYLO_STATIONS[i].url);
			snprintf(stations[station_count].favicon, sizeof(stations[station_count].favicon), "%s",
					 FLYLO_STATIONS[i].icon);
			got = true;
			break;
		default:
			got = radio_fav_get(i, &stations[station_count]);
			break;
		}
		if (got) {
			station_count++;
		}
	}
	if (list_mode == LIST_CUSTOM) {
		marked_custom = radio_custom_current_index();
	}

	// The built-in list's icons are nobody's download yet: a station writes its
	// own only when it has been played, and these have just appeared on a page.
	// Asked for here, off this thread, and the rows look at the card again for
	// the next few seconds so the pictures arrive without leaving the page.
	if (station_count > 0) {
		const char *missing[16];
		char cached[512];
		int n = 0;
		for (int i = 0; i < station_count && n < 16; i++) {
			if (stations[i].favicon[0] &&
				!radio_station_art_path(stations[i].favicon, cached, sizeof(cached))) {
				missing[n++] = stations[i].favicon;
			}
		}
		if (n > 0) {
			radio_art_prefetch(missing, n);
			flylo_art_ticks = 30; // about ten seconds of the list's poll
		}
	}
}

// What an empty stored list should say.
//
// The two tags live in an array rather than in the return statement so that
// tools/extract_strings.py can find them (its TABLES list). They reach tr()
// through set_message(), which is a sink the extractor discovers on its own --
// but it discovers it by reading the call sites, and what this function returns
// is not something a call site can be read for. A tag only named here would end
// up in no language file and be printed raw.
static const char *const STORED_EMPTY[] = {"radio_no_favourite_stations", "radio_recent_empty", "radio_custom_empty",
										   "radio_custom_no_file"};

static const char *stored_empty_message(void) {
	if (list_mode == LIST_FLYLO) {
		// Flylo's list is hardcoded and never empty: if we ever get here,
		// something went wrong, and a blank page is less confusing than the
		// wrong sentence.
		return NULL;
	}
	if (list_mode == LIST_RECENT) {
		return STORED_EMPTY[1];
	}
	if (list_mode == LIST_CUSTOM) {
		// An empty list and a missing file are different problems and want
		// different sentences: one says the lines could not be read, the other
		// says where to put the file.
		return STORED_EMPTY[radio_custom_file_present() ? 2 : 3];
	}
	return STORED_EMPTY[0];
}

// Watches the worker while a request is in flight.
static void list_poll_cb(lv_timer_t *timer) {
	(void)timer;

	if (lv_screen_active() != radiolist_screen) {
		return;
	}

	// Station icons land here, on the same beat as the rest of the list's
	// bookkeeping: the decode happened off the UI thread and this only swaps
	// the finished picture in.
	rows_art_update();

	// The Flylo list is still waiting for pictures somebody else is fetching:
	// once a second each row looks at the card again, until the window closes
	// or the page goes away. Cheap -- a stat or two per row, and only for a
	// list that was opened without a single icon in the cache.
	if (flylo_art_ticks > 0) {
		flylo_art_ticks--;
		if (flylo_art_ticks % 3 == 0) {
			rows_art_refresh();
		}
	}

	// The mark follows the station, which changes from the player and the
	// side keys as well as from here.
	if (list_mode == LIST_CUSTOM) {
		int now = radio_custom_current_index();
		if (now != marked_custom) {
			marked_custom = now;
			for (int i = 0; i < ROW_POOL; i++) {
				row_update_playmark(&rows[i]);
			}
		}
	} else if (list_mode != LIST_TERMS && marked_station_update()) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_update_playmark(&rows[i]);
		}
	}

	if (LIST_IS_STORED(list_mode)) {
		return;
	}

	unsigned serial = radio_job_serial();
	if (serial == last_job_serial) {
		return;
	}
	last_job_serial = serial;

	bool appending = page_append;
	page_append = false;
	page_loading = false;

	if (radio_job_state() != RADIO_JOB_OK) {
		if (!appending) {
			set_message("radio_directory_unreachable");
		}
		page_more = false;
		return;
	}

	if (list_mode == LIST_TERMS) {
		term_count = radio_get_terms(terms, (int)(sizeof(terms) / sizeof(terms[0])));
		station_count = 0;
		page_more = false;
		terms_sort();
		list_rebuild();
		set_message(term_count ? NULL : "nothing_to_show");
		return;
	}

	int room = RADIO_MAX_HELD - (appending ? station_count : 0);
	if (room > RADIO_PAGE_SIZE) {
		room = RADIO_PAGE_SIZE;
	}

	int got = 0;
	if (room > 0) {
		got = radio_get_stations(appending ? &stations[station_count] : stations, room);
	}
	if (appending && got > 0) {
		// A station that is already on the list stays where it is. The
		// directory holds duplicates nobody merged, and its paging can hand the
		// same stream back at the top of the next page -- either way a second
		// copy under the first is a bug the user sees as rows that go nowhere.
		radio_station_t *fresh = &stations[station_count];
		int kept = 0;
		for (int i = 0; i < got; i++) {
			bool listed = false;
			for (int j = 0; j < station_count && !listed; j++) {
				listed = radio_station_same(&stations[j], &fresh[i]);
			}
			for (int j = 0; j < kept && !listed; j++) {
				listed = radio_station_same(&fresh[j], &fresh[i]);
			}
			if (!listed) {
				if (kept != i) {
					fresh[kept] = fresh[i];
				}
				kept++;
			}
		}
		got = kept;
	}

	// Whether there is more is about what the server sent, not what survived
	// the filter: a full page with one https-only station in it comes back as
	// ninety-nine, and reading that as "the end" would stop the list there.
	int raw = radio_last_raw_count();
	page_offset = (appending ? page_offset : 0) + raw;
	page_more = (raw >= RADIO_PAGE_SIZE) && (station_count + got < RADIO_MAX_HELD);

	station_count = appending ? station_count + got : got;
	term_count = 0;

	if (station_count > 0) {
		const char *missing[16];
		char cached[512];
		int n = 0;
		for (int i = 0; i < station_count && n < 16; i++) {
			if (stations[i].favicon[0] &&
				!radio_station_art_path(stations[i].favicon, cached, sizeof(cached))) {
				missing[n++] = stations[i].favicon;
			}
		}
		if (n > 0) {
			radio_art_prefetch(missing, n);
			flylo_art_ticks = 30;
		}
	}

	list_rebuild_keep(appending);
	set_message(station_count ? NULL : empty_stations_message());
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	// A station played while this page was away wrote its icon to the cache
	// then; reopening is the moment the rows should look for it.
	rows_art_refresh();
	// Both stored lists can change from the player while this page is closed:
	// the star adds and removes favourites, and every station played lands in
	// the recents.
	if (LIST_IS_STORED(list_mode)) {
		list_reload_stored();
		list_rebuild();
		set_message(station_count ? NULL : stored_empty_message());
	} else {
		// A rebuild, not only a window update, so the refreshed rows are bound
		// again. Keeping the scroll makes the difference invisible when there
		// is nothing new.
		list_rebuild_keep(true);
	}
}

static void sort_clicked_cb(lv_event_t *e) {
	(void)e;
	if (list_mode != LIST_TERMS) {
		return;
	}
	terms_set_desc(list_browse, !terms_desc(list_browse));
	sort_button_update();
	terms_sort();
	list_rebuild();
}

// The badges' colours are hand-set per row, so a change of theme binds the rows
// on screen again.
static void list_theme_refresh(void) {
	if (list_body) {
		list_window_update();
	}
}

static void build_list_page(gui_config_t *cfg) {
	lv_obj_add_style(radiolist_screen, &theme_style_screen, 0);
	list_title = settingsrow_title(radiolist_screen, cfg, "radio");

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	list_view = lv_obj_create(radiolist_screen);
	lv_obj_set_size(list_view, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(list_view, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list_view, 0, 0);
	lv_obj_set_style_border_width(list_view, 0, 0);
	lv_obj_set_style_radius(list_view, 0, 0);
	lv_obj_set_style_pad_hor(list_view, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_view, 0, 0);
	lv_obj_set_scroll_dir(list_view, LV_DIR_VER);
	lv_obj_add_event_cb(list_view, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	// The list is a page surface like any other: dragging left on it pulls the
	// player in. Attached to the list and not only to the screen behind it,
	// because the list covers the screen and would otherwise eat the drag.
	player_sheet_attach_drag(list_view, true);

	list_body = lv_obj_create(list_view);
	lv_obj_set_width(list_body, width);
	lv_obj_set_height(list_body, row_pitch());
	lv_obj_set_pos(list_body, 0, 0);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	lv_obj_set_event_bubble(list_body, true);

	list_message = lv_label_create(list_view);
	lv_label_set_text(list_message, "");
	lv_obj_set_style_text_align(list_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_message, &font_ui_24, 0);
	// Wrapped and inside the margins. Its place is worked out when it is shown
	// (see set_message): the list area is scrollable, so an alignment against
	// the label's parent would centre it against whatever the content happens
	// to be rather than against the screen the reader is looking at.
	lv_label_set_long_mode(list_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_message, cfg->screen_width - 4 * cfg->padding);
	hide(list_message);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, ROW_PAD_HOR, 0);
		// The mark's gutter comes out of the left alone (see PLAYMARK_GUTTER).
		lv_obj_set_style_pad_left(row->button, PLAYMARK_GUTTER, 0);
		lv_obj_set_style_pad_ver(row->button, bp_pick(8, 10), 0);
		lv_obj_set_style_pad_column(row->button, bp_pick(10, 14), 0);
		hide(row->button);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		// The station's icon, the first thing in the row, exactly as every
		// other list here puts a thumb before its name. It starts clear of the
		// playmark's gutter and the text column takes what is left.
		row->pool = i;
		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, RADIO_ICON_SIZE, RADIO_ICON_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);
		lv_obj_add_style(row->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
		// The same soft corner the Tidal, Qobuz and YouTube rows draw their
		// covers with. A station's logo is a picture like theirs, and a square
		// among rounded sleeves reads as a different kind of row rather than as
		// a different kind of station.
		lv_obj_set_style_radius(row->icon, 6, 0);
		lv_obj_set_style_clip_corner(row->icon, true, 0);
		lv_obj_set_clickable(row->icon, false);
		lv_obj_set_event_bubble(row->icon, true);
		row_art_placeholder(row);

		// The name, and under it the quality badge -- one small glyph, not the
		// codec and the bitrate spelled out -- with the country code beside it on
		// a search's hits. The station's own icon leads it, when it has one. A
		// station that cannot be played stays faint, and the popup says why.
		lv_obj_t *text = lv_obj_create(row->button);
		lv_obj_set_flex_grow(text, 1);
		lv_obj_set_height(text, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(text, 0, 0);
		lv_obj_set_style_border_width(text, 0, 0);
		lv_obj_set_style_pad_all(text, 0, 0);
		lv_obj_set_scrollable(text, false);
		lv_obj_set_event_bubble(text, true);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(text, 4, 0);

		row->name = lv_label_create(text);
		lv_label_set_long_mode(row->name, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->name, lv_pct(100));
		// One line, and the height is what says so. LV_LABEL_LONG_DOT wraps
		// first and only puts the dots in when the wrapped text runs past the
		// height it is given -- so with the height left to the content there is
		// never anything to cut, and a long station name takes as many lines as
		// it needs, through the row underneath it.
		lv_obj_set_height(row->name, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->name, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->name, &font_ui_24, 0);

		row->detail = lv_obj_create(text);
		lv_obj_remove_style_all(row->detail);
		lv_obj_set_size(row->detail, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(row->detail, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->detail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row->detail, 8, 0);
		lv_obj_set_scrollable(row->detail, false);
		lv_obj_set_clickable(row->detail, false);
		lv_obj_set_event_bubble(row->detail, true);
		hide(row->detail);

		// White, tinted per row with the colour of its quality.
		row->quality = lv_image_create(row->detail);
		lv_image_set_src(row->quality, &icon_radio_quality);
		lv_obj_set_style_image_recolor_opa(row->quality, LV_OPA_COVER, 0);
		lv_obj_set_clickable(row->quality, false);
		hide(row->quality);

		// The badge says "an MP3 of this kind" and nothing more; the number
		// says what it actually is. Both, on every row that has one, because
		// "320" against "128" is the one difference a listener can act on
		// without playing anything.
		row->bitrate = lv_label_create(row->detail);
		lv_label_set_text(row->bitrate, "");
		lv_obj_add_style(row->bitrate, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->bitrate, &font_ui_20, 0);
		hide(row->bitrate);

		row->code = lv_label_create(row->detail);
		lv_label_set_text(row->code, "");
		lv_obj_add_style(row->code, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->code, &font_ui_20, 0);
		hide(row->code);

		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, bp_pick(36, 52), bp_pick(36, 52));
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_add_event_cb(row->menu_btn, menu_btn_cb, LV_EVENT_CLICKED, NULL);
		hide(row->menu_btn);

		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		// Scale the dots icon to fit the compact button; on the regular layout
		// it sits at its natural size and centres itself.
		settingsrow_scale_corner_icon(dots, cfg);
		lv_obj_center(dots);

		row->chevron = lv_image_create(row->button);
		lv_image_set_src(row->chevron, &icon_chevron_right);
		lv_obj_add_style(row->chevron, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(row->chevron, LV_OPA_60, 0);
		settingsrow_scale_corner_icon(row->chevron, cfg);

		// Outside the flex layout, in the row's own left padding: it marks the
		// row without moving anything on it.
		row->playmark = lv_obj_create(row->button);
		lv_obj_set_ignore_layout(row->playmark, true);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - PLAYMARK_GUTTER, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_set_scrollable(row->playmark, false);
		lv_obj_set_clickable(row->playmark, false);
		hide(row->playmark);

		row->index = -1;
	}

	// A-Z / Z-A in the corner the title leaves free, and the strip of letters
	// down the right edge: the category lists only (see sort_button_update).
	sort_btn = lv_btn_create(radiolist_screen);
	settingsrow_place_corner_button(sort_btn, cfg, 0);
	lv_obj_set_style_bg_opa(sort_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sort_btn, 0, 0);
	lv_obj_set_style_shadow_width(sort_btn, 0, 0);
	lv_obj_set_style_pad_all(sort_btn, 0, 0);
	lv_obj_add_event_cb(sort_btn, sort_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(sort_btn, true);

	sort_icon = lv_image_create(sort_btn);
	lv_image_set_src(sort_icon, &icon_sort_az);
	lv_obj_add_style(sort_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(sort_icon, cfg);
	lv_obj_center(sort_icon);

	// The strip only ever has rows on the terms lists (sort_button_update), so
	// it jumps by their pitch.
	list_index = azindex_create(radiolist_screen, cfg, list_view, TERM_ROW_PITCH, list_window_update);

	switcher_attach_back_gesture(list_view);
	switcher_set_back_guard(radiolist_screen, list_back_guard);
	switcher_set_back_guard_peek(radiolist_screen, list_back_has_step);
	lv_obj_add_event_cb(radiolist_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	list_timer = lv_timer_create(list_poll_cb, POLL_MS, NULL);
	theme_register_refresh(list_theme_refresh);
}

// ---------------------------------------------------------------------------
// the hub
// ---------------------------------------------------------------------------

// The Music page's corner button, to the pixel: this page has to look like
// that one, not like a cousin of it.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(radiopage_screen);
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

	return button;
}

static bool list_back_has_step(void) { return list_mode == LIST_STATIONS && stations_from_terms; }

static bool list_back_guard(void) {
	if (!list_back_has_step()) {
		return false; // leave the page, as usual
	}
	// Back to the category list that was entered from: the same request
	// open_browse makes, with the title that list had.
	stations_from_terms = false;
	list_mode = LIST_TERMS;
	term_count = 0;
	station_count = 0;
	page_is_search = false;
	page_is_recommended = false;
	page_more = false;
	page_loading = false;
	page_append = false;
	lv_label_set_text(list_title, terms_title);
	settingsrow_title_realign(list_title);
	sort_button_update();
	list_rebuild();
	set_message("loading");
	last_job_serial = radio_job_serial();
	radio_request_terms(list_browse);
	return true;
}

static void open_browse(radio_browse_t kind, const char *title) {
	list_browse = kind;
	stations_from_terms = false;
	list_mode = LIST_TERMS;
	term_count = 0;
	station_count = 0;
	page_is_search = false;
	page_is_recommended = false;
	page_more = false;
	page_loading = false;
	page_append = false;

	lv_label_set_text(list_title, tr(title));
	settingsrow_title_realign(list_title);
	sort_button_update();
	list_rebuild();
	set_message("loading");

	last_job_serial = radio_job_serial();
	radio_request_terms(kind);

	switch_screen(radiolist_screen);
}

static void language_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_LANGUAGES, "language");
}

static void country_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_COUNTRIES, "radio_country");
}

static void genre_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_GENRES, "genre");
}

// The region's most played stations: the recommendation list. It is a station
// list like any other once it is asked for, paged the same way and stepped
// along by previous and next the same way.
static void open_recommended(void) {
	stations_from_terms = false;
	list_mode = LIST_STATIONS;
	term_count = 0;
	station_count = 0;
	page_is_search = false;
	page_is_recommended = true;
	page_value[0] = '\0';
	page_offset = 0;
	page_more = false;
	page_loading = true;
	page_append = false;

	lv_label_set_text(list_title, tr("radio_recommended"));
	settingsrow_title_realign(list_title);
	sort_button_update();
	list_rebuild();
	set_message("loading");

	last_job_serial = radio_job_serial();
	radio_request_recommended(0);

	switch_screen(radiolist_screen);
}

static void recommended_cb(lv_event_t *e) {
	(void)e;
	open_recommended();
}

static void open_stored(list_mode_t mode, const char *title) {
	stations_from_terms = false;
	list_mode = mode;
	page_is_search = false;
	page_is_recommended = false;
	page_more = false;
	page_loading = false;
	page_append = false;
	lv_label_set_text(list_title, tr(title));
	settingsrow_title_realign(list_title);
	sort_button_update();
	list_reload_stored();
	list_rebuild();
	set_message(station_count ? NULL : stored_empty_message());
	switch_screen(radiolist_screen);
}

static void flylo_cb(lv_event_t *e) {
	(void)e;
	open_stored(LIST_FLYLO, "radio_flylo_favs");
}

static void favourites_cb(lv_event_t *e) {
	(void)e;
	open_stored(LIST_FAVOURITES, "favourites");
}

static void recent_cb(lv_event_t *e) {
	(void)e;
	open_stored(LIST_RECENT, "radio_recently_played");
}

static void search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	switch_screen(radiosearch_screen);
}

// The header gear: straight to this page's own settings. It runs the same
// guards switch_screen_cb does -- a swipe that started on the button is a
// swipe, not a tap -- with the screen as its data rather than through the
// event's, since the settings screen is not an lv_obj_t this callback can be
// handed before it exists (see radiopage_init).
static void radio_settings_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}
	switch_screen(radio_settings_screen);
}

// The file is read again every time the page is opened, so a card edited under
// the player shows what is on it now rather than what it held at boot.
static void custom_cb(lv_event_t *e) {
	(void)e;
	radio_custom_reload();
	open_stored(LIST_CUSTOM, "radio_custom");
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------
// the hub's order and which of its rows there are
//
// Written in the config as the slugs, not as numbers: a build that gains a row
// later appends it rather than mistaking an old index for it. The hidden set is
// a bit mask, one bit per row, for the same reason -- its bits are the ids in
// this file and go nowhere else.
// ---------------------------------------------------------------------------

static void hub_layout_load(void) {
	for (int i = 0; i < HUB_ROWS; i++) {
		hub_order[i] = HUB_DEFAULT_ORDER[i];
		hub_hidden[i] = false;
	}

	const char *saved = config_get("radio", "hub_order", "");
	if (!saved || !saved[0]) {
		// Never arranged: the default order, already in place.
		long mask = config_get_int("radio", "hub_hidden", 0);
		for (int i = 0; i < HUB_ROWS; i++) {
			hub_hidden[i] = ((mask >> i) & 1) != 0;
		}
		return;
	}
	bool seen[HUB_ROWS] = {false};
	int at = 0;
	const char *p = saved ? saved : "";
	while (*p && at < HUB_ROWS) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		const char *start = p;
		while (*p && *p != ',') {
			p++;
		}
		size_t len = (size_t)(p - start);
		while (len > 0 && start[len - 1] == ' ') {
			len--;
		}
		for (int id = 0; id < HUB_ROWS && len; id++) {
			if (!seen[id] && strlen(HUB_SLUGS[id]) == len && strncmp(HUB_SLUGS[id], start, len) == 0) {
				seen[id] = true;
				hub_order[at++] = id;
				break;
			}
		}
	}
	// Anything the file does not name keeps its place at the end, so a row
	// added in a later build shows up instead of disappearing.
	for (int id = 0; id < HUB_ROWS; id++) {
		if (!seen[id] && at < HUB_ROWS) {
			hub_order[at++] = id;
		}
	}

	long mask = config_get_int("radio", "hub_hidden", 0);
	for (int i = 0; i < HUB_ROWS; i++) {
		hub_hidden[i] = ((mask >> i) & 1) != 0;
	}
}

static void hub_layout_save(void) {
	char order[64];
	size_t used = 0;
	for (int slot = 0; slot < HUB_ROWS && used < sizeof(order); slot++) {
		used += (size_t)snprintf(order + used, sizeof(order) - used, "%s%s", slot ? "," : "",
								 HUB_SLUGS[hub_order[slot]]);
	}
	config_set("radio", "hub_order", order);

	long mask = 0;
	for (int i = 0; i < HUB_ROWS; i++) {
		if (hub_hidden[i]) {
			mask |= 1L << i;
		}
	}
	config_set_int("radio", "hub_hidden", mask);
	config_save();
}

// Puts the rows in the chosen order and hides the ones switched off. Called
// from the hub's refresh, which also knows whether there is a network at all.
static void hub_layout_apply(bool connected) {
	int position = 0;
	for (int slot = 0; slot < HUB_ROWS; slot++) {
		int id = hub_order[slot];
		lv_obj_t *row = hub_rows[id];
		if (!row) {
			continue;
		}
		if (!connected || hub_hidden[id]) {
			hide(row);
			continue;
		}
		show(row);
		lv_obj_move_to_index(row, position++);
	}
}

// Shows or hides what needs a network, from whether there is one.
static void hub_refresh(void) {
	bool connected = wifi_is_connected();

	// With no network the page is the notice and nothing else. The starred and
	// recent lists do come out of the database rather than the directory, but
	// every station in them is a stream: offering them over a page that says
	// the Wi-Fi is off leads to a list nothing in which can be played.
	if (connected) {
		show(hub_search_btn);
		hide(hub_wifi_warning);
		hide(hub_wifi_row);
	} else {
		hide(hub_search_btn);
		show(hub_wifi_warning);
		show(hub_wifi_row);
	}
	// Settings needs no network: the sleep timer and the arrangement of this
	// very page are as useful off a card as on one.
	show(hub_settings_btn);

	hub_layout_apply(connected);

	// The heading gets the width the buttons are not using.
	settingsrow_title_corner_slots(settingsrow_page_title(radiopage_screen), g_cfg, connected ? 3 : 1);
}

static void hub_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != radiopage_screen) {
		return;
	}
	hub_refresh();
}

static void hub_loaded_cb(lv_event_t *e) {
	(void)e;
	// The order or the visibility may have changed while the settings page was
	// up, and the file is the authority: read it again rather than trust the
	// copy taken at startup.
	hub_layout_load();
	hub_refresh();
}

// ---------------------------------------------------------------------------
// Radio settings
//
// Five things, and all of them are about this page rather than about the
// stations: a sleep timer of its own (a stretch set for a station should not be
// counting over an album in the morning), where in the world the lists are kept
// to, how good a stream has to be before it is worth listing, and -- on a page
// of its own, behind one row -- the list on the page: which rows there are, and
// in what order.
//
// The arrangement used to be here, under these rows: five switches and ten
// arrows in the middle of a settings page, with the two things somebody
// actually changes (the timer, the region) above them and the list they were
// arranging nowhere in sight. It is a page of its own now, with a row that says
// so, and this page is four rows long again.
// ---------------------------------------------------------------------------

static void radio_region_label(const char *country, char *out, size_t size);

static void radio_region_refresh(void) {
	if (!radio_region_value) {
		return;
	}
	char label[96];
	radio_region_label(radio_region(), label, sizeof(label));
	lv_label_set_text(radio_region_value, label);
	lv_obj_set_style_text_align(radio_region_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(radio_region_value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void radio_quality_refresh(void) {
	// One lit pill per bar, tried as the exact levels the pills set: a value
	// from an older config that is neither (a hand-edited quality_min, say)
	// leaves every pill dark rather than claiming a bar it is not.
	static const int LEVELS[] = {0, 128, 320};
	int min = radio_min_bitrate();
	for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
		if (radio_quality_pills[i]) {
			settingsrow_pill_active(radio_quality_pills[i], min == LEVELS[i]);
		}
	}
}

static void radio_quality_pick_cb(lv_event_t *e) {
	radio_set_min_bitrate((int)(intptr_t)lv_event_get_user_data(e));
	radio_quality_refresh();
}

static void radio_sleep_refresh(void) {
	settingsrow_duration_set_minutes(&radio_sleep_row, sleeptimer_minutes(SLEEPTIMER_RADIO));
	settingsrow_duration_expanded(&radio_sleep_row, sleeptimer_enabled(SLEEPTIMER_RADIO));
	settingsrow_duration_repaint(&radio_sleep_row);
}

static void radio_arrange_move_cb(lv_event_t *e);
static void radio_arrange_toggle_cb(lv_event_t *e);

static void radio_sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_RADIO, lv_obj_has_state(radio_sleep_row.toggle, LV_STATE_CHECKED));
	radio_sleep_refresh();
}

static void radio_sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_RADIO, settingsrow_duration_minutes(&radio_sleep_row));
}

static void radio_arrange_rebuild(void) {
	if (!radio_order_rows) {
		return;
	}
	lv_obj_clean(radio_order_rows);
	for (int pos = 0; pos < HUB_ROWS; pos++) {
		int id = hub_order[pos];
		lv_obj_t *row = lv_obj_create(radio_order_rows);
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
		lv_label_set_text(name, tr(HUB_LABELS[id]));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(name, 1);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22, 0);

		for (int dir = 0; dir < 2; dir++) {
			lv_obj_t *button = lv_btn_create(row);
			lv_obj_set_size(button, bp_pick(28, 40), bp_pick(30, 40));
			lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(button, 0, 0);
			lv_obj_set_style_shadow_width(button, 0, 0);
			lv_obj_set_style_pad_all(button, 0, 0);
			lv_obj_add_event_cb(button, radio_arrange_move_cb, LV_EVENT_CLICKED,
							 (void *)(intptr_t)((dir ? 0 : 0x100) | pos));
			lv_obj_t *icon = lv_image_create(button);
			lv_image_set_src(icon, &icon_chevron_up);
			lv_image_set_rotation(icon, dir ? 1800 : 0);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			lv_obj_center(icon);
			if ((dir == 0 && pos == 0) || (dir == 1 && pos == HUB_ROWS - 1)) {
				lv_obj_add_state(button, LV_STATE_DISABLED);
			}
		}

		lv_obj_t *toggle = lv_switch_create(row);
		lv_obj_set_size(toggle, bp_pick(34, 48), bp_pick(20, 28));
		if (!hub_hidden[id]) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		}
		lv_obj_add_event_cb(toggle, radio_arrange_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
	}
	lv_obj_update_layout(radio_order_rows);
}

static void radio_arrange_move_cb(lv_event_t *e) {
	int encoded = (int)(intptr_t)lv_event_get_user_data(e);
	int pos = encoded & 0xff;
	int target = pos + ((encoded & 0x100) ? -1 : 1);
	if (pos < 0 || pos >= HUB_ROWS || target < 0 || target >= HUB_ROWS) {
		return;
	}
	int swap = hub_order[pos];
	hub_order[pos] = hub_order[target];
	hub_order[target] = swap;
	hub_layout_save();
	radio_arrange_rebuild();
	hub_layout_apply(wifi_is_connected());
}

static void radio_arrange_toggle_cb(lv_event_t *e) {
	int id = (int)(intptr_t)lv_event_get_user_data(e);
	if (id < 0 || id >= HUB_ROWS) {
		return;
	}
	lv_obj_t *toggle = lv_event_get_target(e);
	hub_hidden[id] = !lv_obj_has_state(toggle, LV_STATE_CHECKED);
	hub_layout_save();
	hub_layout_apply(wifi_is_connected());
}

// ---------------------------------------------------------------------------
// The region page
//
// The shared list, Worldwide first and the chosen one ticked -- the same rows
// the podcast and the YouTube Music settings pages draw (region.h), so one
// choice in one place serves every service. Nothing is fetched to build it,
// which is what a page that used to wait for a sixty-country slice of the
// directory gives back: it opens instantly, with no network at all.
//
// Choosing one is what keeps every browse list in that country (see radio.h);
// it does not touch the stations the user starred or played, which are theirs
// wherever they were found.
// ---------------------------------------------------------------------------

static void region_rows_build(void);
static void region_rows_build_async(void *user) {
	(void)user;
	region_rows_build();
}

static void region_rows_load(void) {
	region_count = region_list(region_rows, REGION_LIST_MAX);
}

static int region_index_of(const char *country) {
	region_rows_load();
	for (int i = 0; i < region_count; i++) {
		if (strcasecmp(region_rows[i].country, country ? country : "") == 0) {
			return i;
		}
	}
	return 0;
}

// The row on the settings page: the entry's own name, which is "Worldwide"
// for the empty code this directory's "any country" always was.
static void radio_region_label(const char *country, char *out, size_t size) {
	region_entry_label(&region_rows[region_index_of(country)], out, size);
}

static void region_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= region_count) {
		return;
	}
	radio_set_region(region_rows[index].country);
	radio_region_refresh();
	// The rows are rebuilt a pass later: the row that was tapped is one of them,
	// and deleting it from inside its own click is how a page goes down with the
	// finger still on it.
	gui_post(region_rows_build_async, NULL);
}

static void region_rows_build(void) {
	if (!region_container) {
		return;
	}
	int keep_scroll = (int)lv_obj_get_scroll_y(region_container);
	lv_obj_clean(region_container);

	region_rows_load();
	int active = region_index_of(radio_region());

	for (int i = 0; i < region_count; i++) {
		char label[96];
		region_entry_label(&region_rows[i], label, sizeof(label));
		lv_obj_t *value = NULL;
		lv_obj_t *row = settingsrow_add(region_container, label, &value, region_pick_cb, (void *)(intptr_t)i);
		if (i == active) {
			settingsrow_add_icon(row, &icon_check, THEME_SEMANTIC_GREEN);
		}
	}
	lv_obj_update_layout(region_container);
	lv_obj_scroll_to_y(region_container, keep_scroll, LV_ANIM_OFF);
}

static void region_loaded_cb(lv_event_t *e) {
	(void)e;
	// The list is the firmware's own, so there is nothing to wait for and
	// nothing to ask the directory for: every visit is the same rows.
	region_rows_build();
}

static void open_region_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}
	switch_screen(region_screen);
}

static void build_region_page(gui_config_t *cfg) {
	region_screen = lv_obj_create(NULL);
	region_container = settingsrow_page(region_screen, cfg, "radio_region");
	lv_obj_add_event_cb(region_screen, region_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(region_screen);
}

// ---------------------------------------------------------------------------
// The reorder page
//
// The hub's five... six rows, in the order they are shown, each with the pair of
// arrows that moves it and the switch that keeps it. It used to sit inline in
// Radio settings; it is a page of its own now, because it is a different job
// from the three settings above it and it is taller than the rest of them put
// together.
// ---------------------------------------------------------------------------
static void radio_order_loaded_cb(lv_event_t *e) {
	(void)e;
	hub_layout_load();
	radio_arrange_rebuild();
}

static void open_order_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}
	switch_screen(radio_order_screen);
}

static void build_radio_order_page(gui_config_t *cfg) {
	radio_order_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(radio_order_screen, cfg, "radio_reorder");

	radio_order_rows = lv_obj_create(container);
	lv_obj_set_width(radio_order_rows, lv_pct(100));
	lv_obj_set_height(radio_order_rows, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(radio_order_rows, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(radio_order_rows, 0, 0);
	lv_obj_set_style_pad_all(radio_order_rows, 0, 0);
	lv_obj_set_style_pad_gap(radio_order_rows, bp_pick(4, 8), 0);
	lv_obj_set_scrollable(radio_order_rows, false);
	lv_obj_set_flex_flow(radio_order_rows, LV_FLEX_FLOW_COLUMN);

	radio_arrange_rebuild();
	lv_obj_add_event_cb(radio_order_screen, radio_order_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(radio_order_screen);
	player_sheet_attach_drag(container, true);
	player_sheet_attach_drag(radio_order_screen, true);
}

static void radio_settings_loaded_cb(lv_event_t *e) {
	(void)e;
	radio_sleep_refresh();
	radio_region_refresh();
	radio_quality_refresh();
}

static void build_radio_settings_page(gui_config_t *cfg) {
	build_region_page(cfg);
	build_radio_order_page(cfg);

	radio_settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(radio_settings_screen, cfg, "radio_settings");

	// Its own, as the podcasts have theirs: falling asleep to a station is not
	// falling asleep to an album. Off in Plugins, no page draws a timer.
	if (addons_enabled("sleep_timer")) {
		settingsrow_toggle_duration(container, "sleep_timer", radio_sleep_toggle_cb, radio_sleep_wheel_cb,
								&radio_sleep_row);
	}

	// Where in the world the lists are: the country list's own picker, rather
	// than a row of pills for two or three of them.
	settingsrow_add(container, "radio_region", &radio_region_value, open_region_cb, NULL);

	// How good a stream has to be before it is worth listing. 320 and up is the
	// default (see radio.c); 128 and up is the middle ground for a country
	// where the high bar leaves too little, and "Any bitrate" for one where
	// nothing that high exists at all. The row wraps, so the third pill takes a
	// second line on the V1's 240 px instead of squeezing the other two.
	lv_obj_t *quality_pills = NULL;
	settingsrow_pills(container, "radio_quality", &quality_pills);
	radio_quality_pills[0] = settingsrow_pill(quality_pills, "radio_quality_any", 0, radio_quality_pick_cb);
	radio_quality_pills[1] = settingsrow_pill(quality_pills, "radio_quality_128", 128, radio_quality_pick_cb);
	radio_quality_pills[2] = settingsrow_pill(quality_pills, "radio_quality_320", 320, radio_quality_pick_cb);

	settingsrow_add(container, "radio_reorder", NULL, open_order_cb, NULL);

	hub_layout_load();
	radio_sleep_refresh();
	radio_region_refresh();
	radio_quality_refresh();
	lv_obj_add_event_cb(radio_settings_screen, radio_settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(radio_region_refresh);
	theme_register_refresh(radio_quality_refresh);
	theme_register_refresh(radio_sleep_refresh);
	switcher_attach_back_gesture(radio_settings_screen);
	player_sheet_attach_drag(container, true);
	player_sheet_attach_drag(radio_settings_screen, true);
}

// ---------------------------------------------------------------------------
// the search page
//
// A field, the keyboard, and the last few searches. The hits are stations, and
// there is already a page that draws a list of stations: pressing the magnifier
// asks the directory and hands the answer to it.
//
// The history is Tidal's and the podcast section's, in the radio's own part of
// the configuration file: eight terms, newest first, a row to run one again and
// an x to forget it. The keyboard and the history take turns in the same space
// under the field, because on the V1's 320 px they cannot both be there.
// ---------------------------------------------------------------------------

#define SEARCH_HISTORY_MAX 8
#define SEARCH_HISTORY_LEN 96

static char radio_history[SEARCH_HISTORY_MAX][SEARCH_HISTORY_LEN];
static int radio_history_count;
static bool radio_history_loaded;

static void search_history_rebuild(void);
static void search_keyboard_show(bool visible);
static void search_accept_cb(lv_event_t *e);

// Rebuilding deletes the row whose tap asked for it, so the work is queued.
static void search_history_rebuild_async(void *user) {
	(void)user;
	search_history_rebuild();
}

static void radio_history_load(void) {
	if (radio_history_loaded) {
		return;
	}
	radio_history_loaded = true;
	radio_history_count = 0;

	int stored = (int)config_get_int("radio", "search_history_count", 0);
	if (stored < 0) {
		stored = 0;
	}
	if (stored > SEARCH_HISTORY_MAX) {
		stored = SEARCH_HISTORY_MAX;
	}
	for (int i = 0; i < stored; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		const char *term = config_get("radio", key, "");
		if (term && term[0]) {
			snprintf(radio_history[radio_history_count], SEARCH_HISTORY_LEN, "%s", term);
			radio_history_count++;
		}
	}
}

static void radio_history_save(void) {
	config_set_int("radio", "search_history_count", radio_history_count);
	for (int i = 0; i < SEARCH_HISTORY_MAX; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		config_set("radio", key, i < radio_history_count ? radio_history[i] : "");
	}
	config_save();
}

// Puts the term on top, once: searching for the same thing twice does not fill
// the history with it.
static void radio_history_remember(const char *term) {
	radio_history_load();
	while (term && *term == ' ') {
		term++;
	}
	if (!term || !*term) {
		return;
	}

	char kept[SEARCH_HISTORY_MAX][SEARCH_HISTORY_LEN];
	int kept_count = 0;
	for (int i = 0; i < radio_history_count && kept_count < SEARCH_HISTORY_MAX - 1; i++) {
		if (strcmp(radio_history[i], term) == 0) {
			continue;
		}
		snprintf(kept[kept_count], SEARCH_HISTORY_LEN, "%s", radio_history[i]);
		kept_count++;
	}
	snprintf(radio_history[0], SEARCH_HISTORY_LEN, "%s", term);
	for (int i = 0; i < kept_count; i++) {
		snprintf(radio_history[i + 1], SEARCH_HISTORY_LEN, "%s", kept[i]);
	}
	radio_history_count = kept_count + 1;
	radio_history_save();
}

static void search_keyboard_show(bool visible) {
	if (!search_keyboard) {
		return;
	}
	keyboard_set_visible(search_keyboard, visible);
	if (visible) {
		if (search_history_list) {
			lv_obj_set_hidden(search_history_list, true);
		}
		if (search_history_empty) {
			lv_obj_set_hidden(search_history_empty, true);
		}
	} else {
		search_history_rebuild();
	}
}

static void history_forget_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= radio_history_count) {
		return;
	}
	for (int i = index; i + 1 < radio_history_count; i++) {
		snprintf(radio_history[i], SEARCH_HISTORY_LEN, "%s", radio_history[i + 1]);
	}
	radio_history_count--;
	radio_history_save();
	gui_post(search_history_rebuild_async, NULL);
}

static void history_pick_cb(lv_event_t *e);

// Draws the last few searches, in place of the keyboard while nothing is being
// typed.
static void search_history_rebuild(void) {
	if (!search_history_list) {
		return;
	}
	radio_history_load();
	lv_obj_clean(search_history_list);

	bool any = radio_history_count > 0;
	lv_obj_set_hidden(search_history_list, !any);
	if (search_history_empty) {
		lv_obj_set_hidden(search_history_empty, any);
	}
	if (!any) {
		return;
	}

	for (int i = 0; i < radio_history_count; i++) {
		lv_obj_t *row = lv_btn_create(search_history_list);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, bp_pick(40, 68));
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, bp_pick(10, 12), 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, bp_pick(8, 20), 0);
		lv_obj_set_style_pad_column(row, bp_pick(4, 10), 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(row, history_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, radio_history[i]);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24, 0);
		lv_obj_set_flex_grow(label, 1);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_height(label, lv_font_get_line_height(&font_ui_18));

		lv_obj_t *x = lv_btn_create(row);
		lv_obj_set_size(x, bp_pick(30, 44), bp_pick(30, 44));
		lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(x, 0, 0);
		lv_obj_set_style_shadow_width(x, 0, 0);
		lv_obj_set_style_pad_all(x, 0, 0);
		lv_obj_add_event_cb(x, history_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *glyph = lv_image_create(x);
		lv_image_set_src(glyph, &icon_close);
		lv_obj_add_style(glyph, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(glyph, LV_OPA_60, 0);
		lv_obj_center(glyph);
	}
	lv_obj_update_layout(search_history_list);
}

static void history_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= radio_history_count) {
		return;
	}
	lv_textarea_set_text(search_field, radio_history[index]);
	search_accept_cb(e);
}

static void search_field_clicked_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	search_keyboard_show(true);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
}

static void run_search(void) {
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	radio_history_remember(text);
	// The results get the whole page: a keyboard left up over a list nobody
	// asked to see with it would be in the way of every row.
	search_keyboard_show(false);

	list_mode = LIST_STATIONS;
	term_count = 0;
	station_count = 0;
	page_is_search = true;
	page_is_recommended = false;
	page_offset = 0;
	page_more = false;
	page_append = false;
	page_loading = true;
	snprintf(page_value, sizeof(page_value), "%s", text);

	lv_label_set_text(list_title, text);
	settingsrow_title_realign(list_title);
	sort_button_update();
	list_rebuild();
	set_message("loading");

	last_job_serial = radio_job_serial();
	radio_request_search(page_value, 0);

	switch_screen(radiolist_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	run_search();
}

static void search_loaded_cb(lv_event_t *e) {
	(void)e;
	radio_history_load();
	if (radio_history_count > 0) {
		// Something to pick up from last time: the history rather than the
		// keyboard, and a tap on the field brings the keys back.
		search_keyboard_show(false);
		lv_obj_remove_state(search_field, LV_STATE_FOCUSED);
	} else {
		// Nothing searched yet: the keyboard, as the page always opened.
		keyboard_reset(search_keyboard);
		search_keyboard_show(true);
		lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	}
}

static void build_search_page(gui_config_t *cfg) {
	lv_obj_add_style(radiosearch_screen, &theme_style_screen, 0);
	settingsrow_title(radiosearch_screen, cfg, "radio_search_radio");

	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(radiosearch_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("radio_station_name"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_clicked_cb, LV_EVENT_CLICKED, NULL);

	// The history, in the space under the field. The keyboard takes the same
	// space when one is being typed; on the V1 there is room for one of them.
	int history_top = top + 70;
	search_history_list = lv_obj_create(radiosearch_screen);
	lv_obj_set_size(search_history_list, cfg->screen_width - 2 * cfg->padding,
					cfg->screen_height - history_top - cfg->padding);
	lv_obj_set_style_bg_opa(search_history_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_history_list, 0, 0);
	lv_obj_set_style_pad_all(search_history_list, 0, 0);
	lv_obj_set_style_pad_gap(search_history_list, bp_pick(4, 8), 0);
	lv_obj_set_scroll_dir(search_history_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(search_history_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(search_history_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(search_history_list, LV_ALIGN_TOP_LEFT, cfg->padding, history_top);

	search_history_empty = lv_label_create(radiosearch_screen);
	lv_label_set_text(search_history_empty, tr("radio_search_history_empty"));
	lv_label_set_long_mode(search_history_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(search_history_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(search_history_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(search_history_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(search_history_empty, &font_ui_18, 0);
	lv_obj_align(search_history_empty, LV_ALIGN_TOP_MID, 0, history_top + 60);
	lv_obj_set_hidden(search_history_empty, true);

	// Same regular/compact keyboard height as the other search pages, so it is
	// the same keyboard in the same place rather than a lookalike.
	search_keyboard = keyboard_create(radiosearch_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316,
									  search_field, &icon_search, NULL,
									  search_accept_cb, NULL);
	keyboard_set_visible(search_keyboard, false);

	lv_obj_add_event_cb(radiosearch_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(radiosearch_screen);
}

static void build_radio_settings_page(gui_config_t *cfg);

lv_obj_t *radiopage_settings_screen(void) { return radio_settings_screen; }

void radiopage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	radio_init();

	hub_container = settingsrow_page(radiopage_screen, cfg, "radio");
	settingsrow_title_corner_slots(settingsrow_page_title(radiopage_screen), cfg, 2);

	// Before the header buttons below: the gear's event is bound with the
	// settings screen as its user data, and LVGL copies that pointer at
	// registration time. Built after the button, the gear handed it NULL and
	// pressed nothing -- which is exactly what it did.
	build_radio_settings_page(cfg);

	// The drag towards the player. The screen already has it (gui.c attaches it
	// along with the other pages'), but the container sits on top and would take
	// the gesture, so as on the Qobuz pages it is attached to both.
	player_sheet_attach_drag(hub_container, true);

	// Two, and two only, and in the Music page's order: search, and the
	// settings for this page. Corner buttons are placed from the right edge, so
	// slot 0 (the rightmost) is the settings gear and search is beside it. The
	// heart is gone from the header: the starred stations are the FAVOURITES
	// row below, under Recently played, and they can be moved or switched off
	// with the rest.
	hub_settings_btn = corner_button(cfg, 0, &icon_music_settings);
	lv_obj_add_event_cb(hub_settings_btn, radio_settings_cb, LV_EVENT_CLICKED, NULL);

	hub_search_btn = corner_button(cfg, 1, &icon_search);
	lv_obj_add_event_cb(hub_search_btn, search_cb, LV_EVENT_CLICKED, NULL);

	hub_layout_load();
	hub_rows[HUB_RECENT] = settingsrow_add(hub_container, HUB_LABELS[HUB_RECENT], NULL, recent_cb, NULL);
	hub_rows[HUB_LANGUAGE] = settingsrow_add(hub_container, HUB_LABELS[HUB_LANGUAGE], NULL, language_cb, NULL);
	hub_rows[HUB_COUNTRY] = settingsrow_add(hub_container, HUB_LABELS[HUB_COUNTRY], NULL, country_cb, NULL);
	hub_rows[HUB_GENRE] = settingsrow_add(hub_container, HUB_LABELS[HUB_GENRE], NULL, genre_cb, NULL);
	hub_rows[HUB_CUSTOM] = settingsrow_add(hub_container, HUB_LABELS[HUB_CUSTOM], NULL, custom_cb, NULL);
	hub_rows[HUB_FAVOURITES] =
		settingsrow_add(hub_container, HUB_LABELS[HUB_FAVOURITES], NULL, favourites_cb, NULL);
	hub_rows[HUB_RECOMMENDED] =
		settingsrow_add(hub_container, HUB_LABELS[HUB_RECOMMENDED], NULL, recommended_cb, NULL);
	// No mark leads the Favourites row: the other rows in this list are words
	// alone, and a red heart beside one of them made it read as a different
	// kind of entry.

	// Flylo's curated chill-hop picks -- a short, hand-picked list that lives
	// on the page rather than in a file or a database. Same row shape as the
	// browse entries above, so the hub reads as one list; and the same
	// arrangement as them, so it can be moved or switched off like any other.
	hub_rows[HUB_FLYLO] = settingsrow_add(hub_container, HUB_LABELS[HUB_FLYLO], NULL, flylo_cb, NULL);

	// The same notice as the Wi-Fi transfer page: a centred message, and below
	// it the "Wi-Fi settings" row that leads to where it can be fixed.
	hub_wifi_warning = lv_label_create(hub_container);
	lv_label_set_text(hub_wifi_warning, tr("enable_wi_fi_first"));
	lv_label_set_long_mode(hub_wifi_warning, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(hub_wifi_warning, lv_pct(100));
	lv_obj_add_style(hub_wifi_warning, &theme_style_text, 0);
	lv_obj_set_style_text_font(hub_wifi_warning, &font_ui_22, 0);
	lv_obj_set_style_text_align(hub_wifi_warning, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(hub_wifi_warning, 4, 0);
	lv_obj_set_style_margin_top(hub_wifi_warning, 14, 0);
	hide(hub_wifi_warning);

	hub_wifi_row = settingsrow_add(hub_container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(hub_wifi_row, 16, 0);
	hide(hub_wifi_row);

	build_list_page(cfg);
	build_search_page(cfg);

	lv_obj_add_event_cb(radiopage_screen, hub_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// The Wi-Fi can come and go while the page is open; the rows follow it.
	hub_timer = lv_timer_create(hub_poll_cb, 1000, NULL);
	(void)hub_timer;
}
