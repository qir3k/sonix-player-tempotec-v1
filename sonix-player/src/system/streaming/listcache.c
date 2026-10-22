#include "listcache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// The cache itself: one file per list, in <card>/.local/listcache.
//
// The header is the whole of the format: four bytes of magic, the version, the
// size of one record and how many follow, then the records. Both the version and
// the record size are checked on the way back in, so a build that changes either
// struct (or the alignment of one) reads the old file as a miss instead of
// filling the page with nonsense.
//
// Written to a temporary name and renamed, like every other file this player
// keeps on the card: a half-written list would otherwise be picked up on the next
// visit and rejected by the header, which is survivable, but the rename costs
// nothing and leaves the previous list in place.

#define LISTCACHE_DIR ".local/listcache"
#define LISTCACHE_MAGIC "SXLC"
#define LISTCACHE_VERSION 1

// A plausible clock. The device has no battery-backed time: before NTP answers,
// time() is 1970 or 1970 plus uptime, and an age worked out against that is
// meaningless. Until the clock is real, a file is treated as fresh -- it was
// written by this or an earlier boot, and asking again would cost exactly the
// request the cache exists to avoid.
#define CLOCK_PLAUSIBLE 1600000000L

typedef struct {
	char magic[4];
	int version;
	int item_size;
	int count;
	long written_at;
} header_t;

static char cache_dir[512];

void listcache_set_root(const char *sd_root) {
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, LISTCACHE_DIR);
}

bool listcache_ready(void) { return cache_dir[0] != '\0'; }

// The key is a name the caller chooses; anything that is not safe in a file name
// becomes an underscore. Keys are built by the pages out of ids and a prefix, so
// the result is readable on a computer and stable across builds.
static void safe_name(const char *key, char *out, size_t size) {
	size_t used = 0;
	for (const char *p = key; p && *p && used + 1 < size; p++) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
				  c == '_';
		out[used++] = ok ? c : '_';
	}
	out[used] = '\0';
	if (used == 0) {
		snprintf(out, size, "list");
	}
}

static bool path_for(const char *key, char *out, size_t size) {
	if (!cache_dir[0] || !key || !key[0]) {
		out[0] = '\0';
		return false;
	}
	char name[72];
	safe_name(key, name, sizeof(name));
	snprintf(out, size, "%.400s/%.64s.bin", cache_dir, name);
	return true;
}

static bool ensure_dir(void) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", cache_dir);
	if (!dir[0]) {
		return false;
	}
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(dir, 0777);
		*p = '/';
	}
	return mkdir(dir, 0777) == 0 || errno == EEXIST;
}

bool listcache_save(const char *key, const void *items, size_t item_size, int count) {
	if (!items || item_size == 0 || count <= 0) {
		return false;
	}
	char path[600];
	if (!path_for(key, path, sizeof(path)) || !ensure_dir()) {
		return false;
	}

	header_t header;
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, LISTCACHE_MAGIC, 4);
	header.version = LISTCACHE_VERSION;
	header.item_size = (int)item_size;
	header.count = count;
	header.written_at = (long)time(NULL);

	char temp[640];
	snprintf(temp, sizeof(temp), "%.600s.parte", path);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
			  fwrite(items, item_size, (size_t)count, f) == (size_t)count;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok || rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

int listcache_load(const char *key, void *items, size_t item_size, int max) {
	if (!items || item_size == 0 || max <= 0) {
		return -1;
	}
	char path[600];
	if (!path_for(key, path, sizeof(path))) {
		return -1;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}

	header_t header;
	bool ok = fread(&header, sizeof(header), 1, f) == 1 && memcmp(header.magic, LISTCACHE_MAGIC, 4) == 0 &&
			  header.version == LISTCACHE_VERSION && header.item_size == (int)item_size && header.count > 0 &&
			  header.count <= max;

	int count = -1;
	if (ok) {
		count = (int)fread(items, item_size, (size_t)header.count, f);
		if (count <= 0) {
			count = -1;
		}
	}
	fclose(f);
	return count;
}

int listcache_age(const char *key) {
	char path[600];
	if (!path_for(key, path, sizeof(path))) {
		return -1;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return -1;
	}
	long now = (long)time(NULL);
	if (now < CLOCK_PLAUSIBLE || (long)st.st_mtime < CLOCK_PLAUSIBLE) {
		return 0; // the clock cannot say: treat it as just written
	}
	long age = now - (long)st.st_mtime;
	if (age < 0) {
		return 0;
	}
	return (int)age;
}

void listcache_drop(const char *key) {
	char path[600];
	if (path_for(key, path, sizeof(path))) {
		remove(path);
	}
}

void listcache_tidal_playlist_key(const char *playlist_id, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if (!playlist_id || !*playlist_id) {
		return;
	}
	// The UUID is 36 characters of hex and dashes; the prefix keeps it from
	// colliding with the other Tidal lists.
	snprintf(out, size, "tidal_list_%.40s", playlist_id);
}
