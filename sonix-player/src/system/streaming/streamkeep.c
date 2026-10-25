#include "streamkeep.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"

#define KEEP_MUSIC_DIR "Music"
#define KEEP_PART_SUFFIX ".part"
#define KEEP_CHUNK 32768

// How long "Keep on card" waits for a download that is still in flight. A
// hi-res DASH track can take minutes on a slow link, and the wait sleeps, so it
// costs the player nothing but the one slot "one keep at a time" already
// reserves. Long enough for the biggest track on the slowest link anybody would
// listen through, short enough to end a keep that will never finish.
#define KEEP_WAIT_MAX_MS (30 * 60 * 1000)
#define KEEP_WAIT_STEP_MS 200

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;

bool streamkeep_is_stream_track(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	return tidalcache_owns(path) || qobuzcache_owns(path);
}

bool streamkeep_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = running;
	pthread_mutex_unlock(&lock);
	return busy;
}

// ---------------------------------------------------------------------------
// the tags the caches keep beside a track
// ---------------------------------------------------------------------------

// The same key=value file tidalcache.c and qobuzcache.c write next to the
// download. A FLAC stitched back together from DASH segments carries no tags of
// its own, so this is the only place the title and artist exist.
static bool sidecar_tag(const char *path, const char *key, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	char tags[800];
	snprintf(tags, sizeof(tags), "%.760s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}

	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// the destination on the card
// ---------------------------------------------------------------------------

// Every folder on the way to the path, made. The card may never have held a
// Tidal album, so <sd>/Music/Tidal/<artist>/<album> is four levels of nothing.
// The path itself is the file and stops one level short.
static bool make_dirs(const char *path) {
	char work[768];
	snprintf(work, sizeof(work), "%s", path);

	char *leaf = strrchr(work, '/');
	if (!leaf || leaf == work) {
		return false; // nowhere to put a folder, and a bare name has no folder
	}
	*leaf = '\0';

	for (char *p = work + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(work, 0755) != 0 && errno != EEXIST) {
			return false;
		}
		*p = '/';
	}
	return mkdir(work, 0755) == 0 || errno == EEXIST;
}

static void stem_of(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	size_t n = dot && (!slash || dot > slash) ? (size_t)(dot - path) : strlen(path);
	if (n >= size) {
		n = size - 1;
	}
	memcpy(out, path, n);
	out[n] = '\0';
}

// <card>/Music/Tidal/<artist>/<album>/<nn> <title>.<ext>
static bool destination_path(const char *path, const char *service, char *out, size_t size) {
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		out[0] = '\0';
		return false;
	}

	// Whatever the sidecars say, falling back to the numbers the cache names
	// its files by -- the copy keeps working after a card whose sidecars were
	// lost, and the title is better than "471624591" even so.
	char title[256] = "";
	char artist[256] = "";
	char album[256] = "";
	char number[16] = "";
	sidecar_tag(path, "title", title, sizeof(title));
	sidecar_tag(path, "artist", artist, sizeof(artist));
	sidecar_tag(path, "album", album, sizeof(album));
	sidecar_tag(path, "track", number, sizeof(number));
	if (!title[0]) {
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		snprintf(title, sizeof(title), "%s", base);
	}
	// Plain English, like podcastdl.c's fallbacks: these are folder names on
	// the card, not sentences on the screen.
	if (!artist[0]) {
		snprintf(artist, sizeof(artist), "%s", "Unknown artist");
	}
	if (!album[0]) {
		snprintf(album, sizeof(album), "%s", "Unknown album");
	}

	char name_artist[160], name_album[160], name_title[160];
	podcastdl_safe_name(artist, "Unknown", name_artist, sizeof(name_artist));
	podcastdl_safe_name(album, "Unknown", name_album, sizeof(name_album));
	podcastdl_safe_name(title, "Track", name_title, sizeof(name_title));

	char ext[16] = "";
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	if (dot && (!slash || dot > slash) && strlen(dot) < sizeof(ext)) {
		snprintf(ext, sizeof(ext), "%s", dot);
	}

	int track = atoi(number);
	char leaf[512];
	if (track > 0) {
		snprintf(leaf, sizeof(leaf), "%02d %s%s", track, name_title, ext);
	} else {
		snprintf(leaf, sizeof(leaf), "%s%s", name_title, ext);
	}

	snprintf(out, size, "%s/%s/%s/%s/%s/%s", sd, KEEP_MUSIC_DIR, service, name_artist, name_album, leaf);
	return true;
}

// ---------------------------------------------------------------------------
// the copy
// ---------------------------------------------------------------------------

static bool copy_bytes(const char *from, const char *to) {
	struct stat st;
	if (stat(from, &st) != 0 || st.st_size <= 0) {
		return false;
	}

	FILE *in = fopen(from, "rb");
	if (!in) {
		return false;
	}

	// Aside and renamed, so a half-written file -- the card pulled, the power
	// gone -- is never mistaken for a finished track by a later scan.
	char part[900];
	snprintf(part, sizeof(part), "%.860s%s", to, KEEP_PART_SUFFIX);
	FILE *out = fopen(part, "wb");
	if (!out) {
		fclose(in);
		return false;
	}

	bool ok = true;
	char *buf = malloc(KEEP_CHUNK);
	if (!buf) {
		ok = false;
	} else {
		size_t n;
		while (ok && (n = fread(buf, 1, KEEP_CHUNK, in)) > 0) {
			ok = fwrite(buf, 1, n, out) == n;
		}
		ok = ok && !ferror(in) && !ferror(out);
		free(buf);
	}
	if (fclose(out) != 0 || !ok) {
		ok = false;
	}
	fclose(in);
	if (!ok || rename(part, to) != 0) {
		remove(part);
		return false;
	}
	return true;
}

static void notify_async(void *user) {
	gui_notify_popup((intptr_t)user == 1 ? "stream_keep_done" : "stream_keep_failed");
}

static void notify(int done) { gui_post(notify_async, (void *)(intptr_t)done); }

// Waits for a download that is still growing to finish. A track being listened
// to is downloaded past its prebuffer in the background, so the marker the cache
// keeps beside it exists exactly when somebody is most likely to press "Keep on
// card" -- and refusing at once, which is what this used to do, meant the button
// never worked for a track that was playing. It waits here instead, on a sleep,
// and the copy starts when the file is whole.
//
// A download that ended badly leaves its marker but stops growing; that is a
// failure to report, not one to wait thirty minutes for. The small grace after
// the growth stops is for the success path, where download_end() clears the
// marker in the same breath as it stops the growth.
static bool wait_for_download(const char *path, const char *marker) {
	int waited = 0;
	while (waited < KEEP_WAIT_MAX_MS) {
		struct stat st;
		if (stat(marker, &st) != 0) {
			return true; // finished, or never was in flight
		}
		if (!growfile_is_growing(path)) {
			for (int i = 0; i < 10; i++) {
				usleep(100 * 1000);
				if (stat(marker, &st) != 0) {
					return true;
				}
			}
			return false; // the download gave up and left its marker
		}
		usleep(KEEP_WAIT_STEP_MS * 1000);
		waited += KEEP_WAIT_STEP_MS;
	}
	return false;
}

static void *keep_worker(void *arg) {
	char path[768];
	char service[16];
	snprintf(path, sizeof(path), "%s", (const char *)arg);
	snprintf(service, sizeof(service), "%s", tidalcache_owns(path) ? "Tidal" : "Qobuz");
	free(arg);
	thread_be_low_priority("stream keep"); // the card is also playing from it

	// The download is still growing: a copy now would be a truncated track the
	// library would then index as a real one. Wait for it, rather than refusing
	// a track that is merely still arriving (see wait_for_download).
	char marker[800];
	snprintf(marker, sizeof(marker), "%.760s.incompleto", path);
	bool ok = wait_for_download(path, marker);

	if (ok) {
		char dest[768];
		ok = destination_path(path, service, dest, sizeof(dest)) && make_dirs(dest);
		if (ok) {
			ok = copy_bytes(path, dest);
			if (ok) {
				// The tags and the sleeve, after the track itself: a reader
				// that finds the file must find its metadata beside it. The
				// two caches name them differently and both are copied as
				// they were written: the tags as "<file>.tags" (the whole
				// name, extension included) and the cover as "<stem>.jpg".
				char src_stem[768], dest_stem[768];
				char from[820], to[820];
				stem_of(path, src_stem, sizeof(src_stem));
				stem_of(dest, dest_stem, sizeof(dest_stem));
				snprintf(from, sizeof(from), "%s.tags", path);
				snprintf(to, sizeof(to), "%s.tags", dest);
				copy_bytes(from, to);
				snprintf(from, sizeof(from), "%s.jpg", src_stem);
				snprintf(to, sizeof(to), "%s.jpg", dest_stem);
				copy_bytes(from, to);
				fprintf(stderr, "streamkeep: %s is now %s\n", path, dest);
			}
		}
	}
	sync();

	pthread_mutex_lock(&lock);
	running = false;
	pthread_mutex_unlock(&lock);

	notify(ok ? 1 : 0);
	return NULL;
}

bool streamkeep_keep(const char *path) {
	if (!streamkeep_is_stream_track(path)) {
		return false;
	}
	// No card, one that is read-only, or one the computer is using: none of
	// them will take a track, and finding that out after the copy starts is
	// worse than saying so now. The caller says why -- it knows whether the
	// card is missing or merely unusable, and saying it here as well would put
	// two toasts on the screen for one tap.
	if (!storage_sd_root() || !storage_sd_writable() || !storage_card_attached() || usb_storage_active()) {
		return false;
	}

	pthread_mutex_lock(&lock);
	if (running) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	running = true;
	pthread_mutex_unlock(&lock);

	char *arg = malloc(strlen(path) + 1);
	if (!arg) {
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	strcpy(arg, path);

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	bool started = pthread_create(&thread, &attr, keep_worker, arg) == 0;
	pthread_attr_destroy(&attr);
	if (!started) {
		free(arg);
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	return true;
}
