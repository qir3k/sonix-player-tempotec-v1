#include "menuorder.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

typedef struct {
	const char *slug;
	const char *label;
} menu_item_t;

static const menu_item_t HOME_ITEMS[] = {
	{"music", "music"},
	{"streaming", "streaming"},
	{"wireless", "wireless"},
	{"audiobooks", "audiobooks"},
	{"more", "more"},
	{"settings", "settings"},
};

static const menu_item_t MORE_ITEMS[] = {
	{"dac", "dac"},
#if GB_CORE
	{"gearboy", "gearboy"},
#endif
	{"books", "books"},
	{"file_explorer", "file_explorer"},
	{"flappy_bird", "flappy_bird"},
	{"calculator", "calculator"},
	{"notes", "notes"},
};

static const menu_item_t STREAMING_ITEMS[] = {
	{"radio", "radio"},
	{"podcasts", "podcasts"},
	{"tidal", "tidal"},
	{"qobuz", "qobuz"},
	{"subsonic", "subsonic"},
};

static const menu_item_t *items_for(menuorder_section_t section, int *count) {
	if (section == MENUORDER_MORE) {
		*count = (int)(sizeof(MORE_ITEMS) / sizeof(MORE_ITEMS[0]));
		return MORE_ITEMS;
	}
	if (section == MENUORDER_STREAMING) {
		*count = (int)(sizeof(STREAMING_ITEMS) / sizeof(STREAMING_ITEMS[0]));
		return STREAMING_ITEMS;
	}
	*count = (int)(sizeof(HOME_ITEMS) / sizeof(HOME_ITEMS[0]));
	return HOME_ITEMS;
}

int menuorder_count(menuorder_section_t section) {
	int count = 0;
	items_for(section, &count);
	return count;
}

const char *menuorder_slug(menuorder_section_t section, int id) {
	int count = 0;
	const menu_item_t *items = items_for(section, &count);
	return id >= 0 && id < count ? items[id].slug : "";
}

const char *menuorder_label(menuorder_section_t section, int id) {
	int count = 0;
	const menu_item_t *items = items_for(section, &count);
	return id >= 0 && id < count ? tr(items[id].label) : "";
}

static int find_slug(const menu_item_t *items, int count, const char *slug, size_t length) {
	for (int i = 0; i < count; i++) {
		if (strlen(items[i].slug) == length && strncmp(items[i].slug, slug, length) == 0) {
			return i;
		}
	}
	return -1;
}

int menuorder_load(menuorder_section_t section, int *order, int max, bool *visible) {
	int count = 0;
	const menu_item_t *items = items_for(section, &count);
	if (!order || !visible || max < count) {
		return 0;
	}
	bool seen[16] = {false};
	for (int i = 0; i < count; i++) {
		visible[i] = true;
	}
	const char *key = section == MENUORDER_MORE ? "more_menu_order"
													: section == MENUORDER_STREAMING ? "streaming_menu_order" : "main_menu_order";
	char stored[512];
	snprintf(stored, sizeof(stored), "%s", config_get("ui", key, ""));
	int out = 0;
	const char *p = stored;
	while (*p && out < count) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		if (!*p) {
			break;
		}
		bool enabled = *p != '!';
		if (!enabled) {
			p++;
		}
		const char *end = p;
		while (*end && *end != ',' && *end != ' ') {
			end++;
		}
		int id = find_slug(items, count, p, (size_t)(end - p));
		if (id >= 0 && !seen[id]) {
			seen[id] = true;
			visible[id] = enabled;
			order[out++] = id;
		}
		p = end;
	}
	for (int id = 0; id < count; id++) {
		if (!seen[id]) {
			order[out++] = id;
		}
	}
	bool any = false;
	for (int i = 0; i < out; i++) {
		any = any || visible[order[i]];
	}
	if (!any && out > 0) {
		visible[order[0]] = true;
	}
	return out;
}

void menuorder_save(menuorder_section_t section, const int *order, int count, const bool *visible) {
	int item_count = 0;
	const menu_item_t *items = items_for(section, &item_count);
	bool seen[16] = {false};
	bool enabled[16] = {false};
	int clean[16];
	int clean_count = 0;
	for (int id = 0; id < item_count; id++) {
		enabled[id] = visible[id];
	}
	for (int i = 0; order && i < count && clean_count < item_count; i++) {
		int id = order[i];
		if (id >= 0 && id < item_count && !seen[id]) {
			seen[id] = true;
			clean[clean_count++] = id;
		}
	}
	for (int id = 0; id < item_count; id++) {
		if (!seen[id]) {
			clean[clean_count++] = id;
		}
	}
	bool any = false;
	for (int i = 0; i < clean_count; i++) {
		any = any || enabled[clean[i]];
	}
	if (!any && clean_count > 0) {
		enabled[clean[0]] = true;
	}
	char stored[512];
	size_t used = 0;
	stored[0] = '\0';
	for (int i = 0; i < clean_count; i++) {
		int id = clean[i];
		int n = snprintf(stored + used, sizeof(stored) - used, "%s%s%s", i ? "," : "",
						 enabled[id] ? "" : "!", items[id].slug);
		if (n < 0 || (size_t)n >= sizeof(stored) - used) {
			break;
		}
		used += (size_t)n;
	}
	const char *key = section == MENUORDER_MORE ? "more_menu_order"
											: section == MENUORDER_STREAMING ? "streaming_menu_order" : "main_menu_order";
	config_set("ui", key, stored);
	config_save();
}
