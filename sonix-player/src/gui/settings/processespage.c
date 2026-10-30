#include "processespage.h"

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/system.h"
#include "src/system/streaming/ytmusic.h"

lv_obj_t *processespage_screen;

// The page container (a scrolling column) and the parts rebuilt on every open:
// the RAM card at the top and the process list below it.
static lv_obj_t *page_container;
static lv_obj_t *ram_label;
static lv_obj_t *proc_list; // emptied and refilled on every SCREEN_LOADED

// A process, reduced to what is worth showing.
typedef struct {
	long rss_kb;
	char name[24];
} proc_row_t;

#define PROC_MAX 200	 // how many are collected
#define PROC_SHOWN 60	 // how many are shown (the largest)

// One line of /proc/meminfo, in kB. -1 when absent.
static long meminfo_kb(const char *key) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}
	char line[160];
	size_t klen = strlen(key);
	long val = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
			val = atol(line + klen + 1);
			break;
		}
	}
	fclose(f);
	return val;
}

// The process name (comm), newline stripped. False if the PID is already gone.
static bool read_comm(const char *pid, char *out, size_t size) {
	char path[300];
	snprintf(path, sizeof(path), "/proc/%s/comm", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	if (!fgets(out, (int)size, f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return out[0] != '\0';
}

// Resident memory in kB, from /proc/PID/statm (second field, in pages). -1 if
// the PID is already gone.
static long read_rss_kb(const char *pid, long page_kb) {
	char path[300];
	snprintf(path, sizeof(path), "/proc/%s/statm", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long total_pages = 0, rss_pages = 0;
	int got = fscanf(f, "%ld %ld", &total_pages, &rss_pages);
	fclose(f);
	if (got < 2) {
		return -1;
	}
	return rss_pages * page_kb;
}

static int rss_desc(const void *a, const void *b) {
	long ra = ((const proc_row_t *)a)->rss_kb;
	long rb = ((const proc_row_t *)b)->rss_kb;
	return ra < rb ? 1 : ra > rb ? -1 : 0;
}

// "12,3 MB" above a megabyte, "820 kB" below: this device's RAM is measured in
// megabytes, but a small process must not read as "0 MB".
static void format_kb(long kb, char *out, size_t size) {
	if (kb < 0) {
		snprintf(out, size, "-");
	} else if (kb >= 1024) {
		snprintf(out, size, "%ld,%ld MB", kb / 1024, (kb % 1024) * 10 / 1024);
	} else {
		snprintf(out, size, "%ld kB", kb);
	}
}

// A list row: name on the left, memory on the right, like the other settings
// rows but with no chevron and no touch handling.
static void add_proc_row(const char *name, long rss_kb) {
	lv_obj_t *card = lv_obj_create(proc_list);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_pick(bp_tile_radius(), 12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, bp_pick(8, 18), 0);
	lv_obj_set_style_pad_ver(card, bp_pick(6, 10), 0);
	lv_obj_set_scrollable(card, false);

	lv_obj_t *name_lbl = lv_label_create(card);
	lv_label_set_text(name_lbl, name);
	lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_lbl, lv_pct(62));
	lv_obj_set_style_text_font(name_lbl, &font_ui_22, 0);
	lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

	char mem[24];
	format_kb(rss_kb, mem, sizeof(mem));
	lv_obj_t *mem_lbl = lv_label_create(card);
	lv_label_set_text(mem_lbl, mem);
	lv_obj_add_style(mem_lbl, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mem_lbl, &font_ui_20, 0);
	lv_obj_align(mem_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
}

// Re-reads everything from /proc and redraws the RAM card and the list.
static void rebuild(void) {
	// --- RAM ---
	long total = meminfo_kb("MemTotal");
	long avail = meminfo_kb("MemAvailable");
	long freek = meminfo_kb("MemFree");
	if (avail < 0) {
		// Older kernels may not have MemAvailable (the 4.4 on this device does,
		// but not every build): fall back to free+buffers+cached.
		long buffers = meminfo_kb("Buffers");
		long cached = meminfo_kb("Cached");
		avail = (freek < 0 ? 0 : freek) + (buffers < 0 ? 0 : buffers) + (cached < 0 ? 0 : cached);
	}
	if (ram_label) {
		if (total > 0) {
			long used = total - avail;
			if (used < 0) {
				used = 0;
			}
			int pct = (int)(used * 100 / total);
			char used_s[24], total_s[24], avail_s[24];
			format_kb(used, used_s, sizeof(used_s));
			format_kb(total, total_s, sizeof(total_s));
			format_kb(avail, avail_s, sizeof(avail_s));
			// The player's own heap, on a third line. /proc says how much of the
			// process is anonymous but not what it is; this is the part of that
			// the interface and everything else asked malloc for, and watching
			// it while walking the pages is what tells a growth from a level.
			size_t heap_used = 0;
			heap_usage(&heap_used, NULL);
			char heap_s[24];
			format_kb((long)(heap_used / 1024), heap_s, sizeof(heap_s));
			lv_label_set_text_fmt(ram_label, "%s: %s / %s (%d%%)\n%s: %s\n%s: %s", tr("processes_ram_used"), used_s, total_s,
								  pct, tr("processes_ram_free"), avail_s, tr("processes_player_heap"), heap_s);
		} else {
			lv_label_set_text(ram_label, "/proc/meminfo?");
		}
	}

	// --- processes ---
	if (!proc_list) {
		return;
	}
	lv_obj_clean(proc_list); // drop the rows from the previous pass

	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	if (page_kb < 1) {
		page_kb = 4;
	}

	static proc_row_t rows[PROC_MAX];
	int n = 0;
	DIR *d = opendir("/proc");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL && n < PROC_MAX) {
			// Only all-digit directory names: those are the PIDs.
			const char *p = e->d_name;
			bool numeric = p[0] != '\0';
			for (const char *c = p; *c; c++) {
				if (*c < '0' || *c > '9') {
					numeric = false;
					break;
				}
			}
			if (!numeric) {
				continue;
			}
			char name[24];
			if (!read_comm(p, name, sizeof(name))) {
				continue;
			}
			long rss = read_rss_kb(p, page_kb);
			rows[n].rss_kb = rss;
			snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
			n++;
		}
		closedir(d);
	}

	qsort(rows, (size_t)n, sizeof(rows[0]), rss_desc);

	int shown = n < PROC_SHOWN ? n : PROC_SHOWN;
	for (int i = 0; i < shown; i++) {
		add_proc_row(rows[i].name, rows[i].rss_kb);
	}
}

// ---------------------------------------------------------------------------
// Optimize
//
// The answer to "it has been on for hours and it has gone slow": give the heap
// and the disk cache back, and sweep the junk the streaming services leave
// behind. Nothing here changes a setting or touches the user's own music -- it
// is exactly the housekeeping a restart would do, minus the restart.
//
// Four things, in the order they cost the least:
//
//   * the kernel's page cache, dropped with drop_caches. Those are clean copies
//     of files already on flash; what comes back is free memory in large
//     pieces, which is what a driver allocating its buffers wants after hours
//     of reading scattered files (audio.c does the same before opening one);
//   * the heap itself, trimmed back to the kernel. The allocator holds on to
//     freed chunks by design, so the figure on the card does not fall by
//     itself even when nothing is using it;
//   * the playback caches of the streaming services: files whose download never
//     finished (a .part left by a Wi-Fi hiccup) are dead weight, and YouTube
//     Music's cache is scratch by definition -- a song kept on the card on
//     purpose is in Music, not here;
//   * the thumbnail cache, which is rebuilt from the artwork on demand and is
//     the biggest thing in .local.
// ---------------------------------------------------------------------------

typedef struct {
	unsigned long long bytes;
	int files;
} sweep_t;

// Removes the regular files of `dir` whose name ends with `suffix` (NULL: every
// one). One level, no recursion: every junk directory here is flat, and a walk
// would be a way to delete something nobody meant.
static void sweep_dir(const char *dir, const char *suffix, sweep_t *out) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (entry->d_name[0] == '.' && entry->d_name[1] == '\0') {
			continue;
		}
		if (suffix) {
			size_t name_len = strlen(entry->d_name), suffix_len = strlen(suffix);
			if (name_len <= suffix_len || strcmp(entry->d_name + name_len - suffix_len, suffix) != 0) {
				continue;
			}
		}
		char path[600];
		if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		if (unlink(path) == 0) {
			out->files++;
			if (st.st_size > 0) {
				out->bytes += (unsigned long long)st.st_size;
			}
		}
	}
	closedir(d);
}

static long mem_available_kb(void) {
	return meminfo_kb("MemAvailable");
}

// The page's own label under the button: what the last run did.
static lv_obj_t *optimize_result;

// What Optimize is allowed to touch. Four switches rather than one button that
// does everything, because the four are not equally dangerous: freeing memory
// costs nothing, clearing the artwork caches costs a few seconds of redrawing
// them, and clearing the streaming caches throws away audio that is still
// being listened to. The choice is kept in the config, so a card with room to
// spare does not lose its cached lists every time somebody presses the button.
static bool optimize_memory(void) { return config_get_bool("optimize", "memory", true); }
static bool optimize_artwork(void) { return config_get_bool("optimize", "artwork", true); }
static bool optimize_parts(void) { return config_get_bool("optimize", "parts", true); }
static bool optimize_caches(void) { return config_get_bool("optimize", "caches", false); }

// One switch, written to the config as it is flipped. A callback each rather
// than one taking the key as user data: settingsrow_toggle() owns the handler
// it attaches, and a second one on the same switch would save twice.
static void optimize_saved(lv_event_t *e, const char *key) {
	config_set_bool("optimize", key, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	config_save();
}

static void optimize_memory_cb(lv_event_t *e) { optimize_saved(e, "memory"); }
static void optimize_artwork_cb(lv_event_t *e) { optimize_saved(e, "artwork"); }
static void optimize_parts_cb(lv_event_t *e) { optimize_saved(e, "parts"); }
static void optimize_caches_cb(lv_event_t *e) { optimize_saved(e, "caches"); }

// A switch on the page, already in the state the config remembers.
static void optimize_toggle(lv_obj_t *container, const char *label, lv_event_cb_t cb, bool on) {
	lv_obj_t *sw = NULL;
	settingsrow_toggle(container, label, &sw, cb);
	if (sw && on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	}
}

// Every playback cache the streaming services keep on the card. Their names
// are not the same shape -- Tidal and YouTube Music hold their sleeves beside
// the audio, Qobuz and the radio keep theirs in a folder of their own -- which
// is why sweeping them is a list rather than one naming rule.
static const char *const PLAYBACK_CACHES[] = {
	".local/tidal-cache", ".local/qobuz-cache", ".local/podcast-cache", ".local/ytmusic-cache", ".local/dlna",
};

// The cover caches, and the folders whose whole content is a sleeve cache.
static const char *const ART_CACHES[] = {
	".local/radio-art", ".local/qobuz-art", ".local/podcast-art",
};

// A cache is not always a folder. The thumbnail cache is one SQLite file of
// several megabytes, and the sweep used to call opendir on it, fail, and leave
// the biggest thing in .local exactly where it was -- which is why the button
// reported a couple of dozen kilobytes freed. A path that names a file is
// unlinked; one that names a folder is swept.
static void sweep_path(const char *path, const char *suffix, sweep_t *out) {
	struct stat st;
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		// A single file: it is the whole cache, so it goes whole. Its owner
		// builds it again on the next look at a cover.
		if (!suffix && unlink(path) == 0) {
			out->files++;
			if (st.st_size > 0) {
				out->bytes += (unsigned long long)st.st_size;
			}
		}
		return;
	}
	sweep_dir(path, suffix, out);
}

static void sweep_under_root(const char *relative, const char *suffix, sweep_t *out) {
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		return;
	}
	char path[600];
	if (snprintf(path, sizeof(path), "%s/%s", root, relative) < (int)sizeof(path)) {
		sweep_path(path, suffix, out);
	}
}

static void optimize_clicked_cb(lv_event_t *e) {
	(void)e;
	long before = mem_available_kb();

	sweep_t junk = {0, 0};
	for (size_t i = 0; i < sizeof(PLAYBACK_CACHES) / sizeof(PLAYBACK_CACHES[0]); i++) {
		if (optimize_parts()) {
			// Half-written downloads, one folder each -- never a finished file.
			sweep_under_root(PLAYBACK_CACHES[i], ".part", &junk);
		}
		if (optimize_caches()) {
			// The rest of the scratch: cached lists, and audio that has already
			// been listened to. A song kept on the card on purpose is in Music,
			// not here, so nothing of the user's own is behind this switch.
			sweep_under_root(PLAYBACK_CACHES[i], NULL, &junk);
		}
	}
	if (optimize_artwork()) {
		// The thumbnail cache is rebuilt from the artwork on demand and is the
		// biggest thing in .local: a file of its own on the card, a folder in
		// the RAM-backed /tmp that a player with no card uses.
		sweep_under_root(".local/thumbnails", NULL, &junk);
		sweep_path("/tmp/.local/thumbnails", NULL, &junk);
		for (size_t i = 0; i < sizeof(ART_CACHES) / sizeof(ART_CACHES[0]); i++) {
			sweep_under_root(ART_CACHES[i], NULL, &junk);
		}
		for (size_t i = 0; i < sizeof(PLAYBACK_CACHES) / sizeof(PLAYBACK_CACHES[0]); i++) {
			sweep_under_root(PLAYBACK_CACHES[i], ".jpg", &junk);
		}
	}
	if (optimize_caches()) {
		sweep_under_root(".local/listcache", NULL, &junk);
		// YouTube Music keeps its own bookkeeping beside the audio; clearing it
		// by name is what the service's own "leave" does.
		ytmusic_cache_clear();
	}

	long gained = 0;
	if (optimize_memory()) {
		// Page cache first, then the heap: the trim walks it, and the walk is
		// cheaper once the kernel has let go of what it was caching.
#ifndef HOST_BUILD
		FILE *drop = fopen("/proc/sys/vm/drop_caches", "w");
		if (drop) {
			fputs("1\n", drop);
			fclose(drop);
		}
#endif
		malloc_trim(0);
		long after = mem_available_kb();
		gained = (before > 0 && after > before) ? after - before : 0;
	}
	fprintf(stderr, "processes: optimize freed %ld kB of RAM, removed %d file(s), %llu kB of junk\n", gained, junk.files,
			junk.bytes / 1024);

	rebuild();
	if (optimize_result) {
		char freed[24], disk[24];
		format_kb(gained, freed, sizeof(freed));
		format_kb((long)(junk.bytes / 1024), disk, sizeof(disk));
		// Both figures, because the RAM one is almost always small -- the heap
		// was never holding much -- and the card one is what the button really
		// buys back on a player that has been streaming for hours.
		lv_label_set_text_fmt(optimize_result, tr("optimize_done"), junk.files, freed, disk);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

// Refresh on demand: the page already redraws on every open, but watching a
// process grow should not require leaving and re-entering for each new figure.
static void refresh_clicked_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void processespage_init(gui_config_t *cfg) {
	page_container = settingsrow_page(processespage_screen, cfg, "processes");

	// The top-right icon, like the other page corner buttons.
	settingsrow_title_corner_slots(settingsrow_page_title(processespage_screen), cfg, 1);
	{
		lv_obj_t *button = lv_btn_create(processespage_screen);
		settingsrow_place_corner_button(button, cfg, 0);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_add_event_cb(button, refresh_clicked_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, &icon_refresh);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		settingsrow_scale_corner_icon(icon, cfg);
		lv_obj_center(icon);
	}

	// The RAM card: two lines, used/total and free.
	lv_obj_t *ram_card = lv_obj_create(page_container);
	lv_obj_set_width(ram_card, lv_pct(100));
	lv_obj_set_height(ram_card, LV_SIZE_CONTENT);
	lv_obj_add_style(ram_card, &theme_style_card, 0);
	lv_obj_set_style_radius(ram_card, bp_pick(bp_tile_radius(), 12), 0);
	lv_obj_set_style_border_width(ram_card, 0, 0);
	lv_obj_set_style_shadow_width(ram_card, 0, 0);
	lv_obj_set_style_pad_all(ram_card, bp_pick(8, 18), 0);
	lv_obj_set_scrollable(ram_card, false);

	ram_label = lv_label_create(ram_card);
	lv_obj_set_width(ram_label, lv_pct(100));
	lv_obj_set_style_text_font(ram_label, &font_ui_22, 0);
	lv_label_set_text(ram_label, "");

	// Optimize, right under what it improves: the figures are the reason
	// anybody is on this page, and the button is what changes them.
	lv_obj_t *optimize = settingsrow_add(page_container, "optimize", NULL, optimize_clicked_cb, NULL);
	settingsrow_add_caption(optimize, "optimize_note");

	// What the button is allowed to touch, under it: the four parts are not
	// equally cheap to lose, so each one is the user's to keep or give up.
	lv_obj_t *what = lv_label_create(page_container);
	lv_label_set_text(what, tr("optimize_what"));
	lv_obj_add_style(what, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(what, &font_ui_18, 0);
	lv_obj_set_style_pad_top(what, 6, 0);
	optimize_toggle(page_container, "optimize_memory", optimize_memory_cb, optimize_memory());
	optimize_toggle(page_container, "optimize_artwork", optimize_artwork_cb, optimize_artwork());
	optimize_toggle(page_container, "optimize_parts", optimize_parts_cb, optimize_parts());
	optimize_toggle(page_container, "optimize_caches", optimize_caches_cb, optimize_caches());

	optimize_result = lv_label_create(page_container);
	lv_obj_set_width(optimize_result, lv_pct(100));
	lv_obj_add_style(optimize_result, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(optimize_result, &font_ui_18, 0);
	lv_label_set_text(optimize_result, "");

	// The list heading, then the list itself: a column refilled on every open.
	lv_obj_t *list_title = lv_label_create(page_container);
	lv_label_set_text(list_title, tr("processes_running_processes"));
	lv_obj_add_style(list_title, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_title, &font_ui_18, 0);
	lv_obj_set_style_pad_top(list_title, 6, 0);

	proc_list = lv_obj_create(page_container);
	lv_obj_set_width(proc_list, lv_pct(100));
	lv_obj_set_height(proc_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(proc_list, 0, 0);
	lv_obj_set_style_border_width(proc_list, 0, 0);
	lv_obj_set_style_pad_all(proc_list, 0, 0);
	lv_obj_set_style_pad_row(proc_list, 8, 0);
	lv_obj_set_flex_flow(proc_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(proc_list, false);

	rebuild();
	lv_obj_add_event_cb(processespage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
