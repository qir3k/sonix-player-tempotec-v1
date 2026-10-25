#ifndef MENUORDER_H
#define MENUORDER_H

#include <stdbool.h>

typedef enum {
	MENUORDER_HOME,
	MENUORDER_MORE,
	MENUORDER_STREAMING,
} menuorder_section_t;

enum {
	MENU_HOME_MUSIC,
	MENU_HOME_STREAMING,
	MENU_HOME_WIRELESS,
	MENU_HOME_AUDIOBOOKS,
	MENU_HOME_MORE,
	MENU_HOME_SETTINGS,
};

enum {
	MENU_STREAM_RADIO,
	MENU_STREAM_PODCASTS,
	MENU_STREAM_TIDAL,
	MENU_STREAM_QOBUZ,
	MENU_STREAM_SUBSONIC,
};

enum {
	MENU_MORE_DAC,
	MENU_MORE_GEARBOY,
	MENU_MORE_BOOKS,
	MENU_MORE_FILE_EXPLORER,
	MENU_MORE_FLAPPY_BIRD,
	MENU_MORE_CALCULATOR,
	MENU_MORE_NOTES,
};

int menuorder_count(menuorder_section_t section);
const char *menuorder_slug(menuorder_section_t section, int id);
const char *menuorder_label(menuorder_section_t section, int id);
int menuorder_load(menuorder_section_t section, int *order, int max, bool *visible);
void menuorder_save(menuorder_section_t section, const int *order, int count, const bool *visible);

#endif
