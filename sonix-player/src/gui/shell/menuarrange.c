#include "menuarrange.h"

#include <stdint.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/menuorder.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/system/core/lang.h"

lv_obj_t *menuarrange_screen;
extern void morepage_refresh_layout(void) __attribute__((weak));

static lv_obj_t *rows;
static lv_obj_t *home_tab;
static lv_obj_t *more_tab;
static lv_obj_t *streaming_tab;
static menuorder_section_t shown_section;

static void paint_tabs(void) {
	if (!home_tab || !more_tab || !streaming_tab) {
		return;
	}
	lv_obj_t *home_label = lv_obj_get_child(home_tab, 0);
	lv_obj_t *more_label = lv_obj_get_child(more_tab, 0);
	lv_obj_t *streaming_label = lv_obj_get_child(streaming_tab, 0);
	bool home = shown_section == MENUORDER_HOME;
	bool more = shown_section == MENUORDER_MORE;
	lv_obj_set_style_bg_color(home_tab, home ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_bg_color(more_tab, more ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_bg_color(streaming_tab, shown_section == MENUORDER_STREAMING ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(home_label, home ? theme_on_accent() : theme()->text_primary, 0);
	lv_obj_set_style_text_color(more_label, more ? theme_on_accent() : theme()->text_primary, 0);
	lv_obj_set_style_text_color(streaming_label, shown_section == MENUORDER_STREAMING ? theme_on_accent() : theme()->text_primary, 0);
}

static void repaint(void);

static void save_and_refresh(int *order, int count, bool *visible) {
	menuorder_save(shown_section, order, count, visible);
	main_menu_refresh_layout();
	if (morepage_refresh_layout) {
		morepage_refresh_layout();
	}
	streaming_refresh_layout();
	repaint();
}

static void move_row_cb(lv_event_t *e) {
	int encoded = (int)(intptr_t)lv_event_get_user_data(e);
	int position = encoded & 0xff;
	int direction = (encoded & 0x100) ? -1 : 1;
	int count = menuorder_count(shown_section);
	int order[16];
	bool visible[16];
	if (menuorder_load(shown_section, order, 16, visible) != count) {
		return;
	}
	int target = position + direction;
	if (target < 0 || target >= count) {
		return;
	}
	int swap = order[position];
	order[position] = order[target];
	order[target] = swap;
	save_and_refresh(order, count, visible);
}

static void visibility_cb(lv_event_t *e) {
	int id = (int)(intptr_t)lv_event_get_user_data(e);
	int count = menuorder_count(shown_section);
	int order[16];
	bool visible[16];
	if (menuorder_load(shown_section, order, 16, visible) != count) {
		return;
	}
	visible[id] = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	save_and_refresh(order, count, visible);
}

static lv_obj_t *move_button(lv_obj_t *row, int position, int direction) {
	lv_obj_t *button = lv_btn_create(row);
	lv_obj_set_size(button, bp_pick(28, 40), bp_pick(30, 40));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, move_row_cb, LV_EVENT_CLICKED,
					 (void *)(intptr_t)((direction < 0 ? 0x100 : 0) | position));
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_chevron_up);
	lv_image_set_rotation(icon, direction > 0 ? 1800 : 0);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

static void repaint(void) {
	if (!rows) {
		return;
	}
	paint_tabs();
	lv_obj_clean(rows);
	int count = menuorder_count(shown_section);
	int order[16];
	bool visible[16];
	if (menuorder_load(shown_section, order, 16, visible) != count) {
		return;
	}
	int row_height = bp_pick(42, 60);
	for (int i = 0; i < count; i++) {
		int id = order[i];
		lv_obj_t *row = lv_obj_create(rows);
		lv_obj_set_size(row, lv_pct(100), row_height);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_style_radius(row, bp_pick(bp_tile_radius(), 8), 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, bp_pick(6, 12), 0);
		lv_obj_set_style_pad_gap(row, bp_pick(1, 5), 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *name = lv_label_create(row);
		const char *label = menuorder_label(shown_section, id);
		if (shown_section == MENUORDER_HOME && id == MENU_HOME_MUSIC && main_menu_browse()) {
			label = tr("music_browse");
		} else if (shown_section == MENUORDER_HOME && id == MENU_HOME_AUDIOBOOKS && main_menu_books()) {
			label = tr("books");
		} else if (shown_section == MENUORDER_MORE && id == MENU_MORE_BOOKS && main_menu_books()) {
			label = tr("audiobooks");
		}
		lv_label_set_text(name, label);
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(name, 1);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22, 0);

		move_button(row, i, -1);
		move_button(row, i, 1);
		lv_obj_t *toggle = lv_switch_create(row);
		lv_obj_set_size(toggle, bp_pick(34, 48), bp_pick(20, 28));
		if (visible[id]) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		}
		lv_obj_add_event_cb(toggle, visibility_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
	}
	lv_obj_update_layout(rows);
}

static void tab_cb(lv_event_t *e) {
	shown_section = (menuorder_section_t)(intptr_t)lv_event_get_user_data(e);
	repaint();
}

static lv_obj_t *make_tab(lv_obj_t *parent, const char *label, menuorder_section_t section) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_flex_grow(button, 1);
	lv_obj_set_height(button, bp_pick(34, 48));
	lv_obj_set_style_radius(button, bp_pick(8, 10), 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)section);
	lv_obj_t *text = lv_label_create(button);
	lv_label_set_text(text, tr(label));
	lv_obj_set_style_text_font(text, bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_20, 0);
	lv_obj_center(text);
	return button;
}

void menuarrange_init(gui_config_t *cfg) {
	(void)cfg;
	menuarrange_screen = lv_obj_create(NULL);
	lv_obj_add_style(menuarrange_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(menuarrange_screen, cfg, "menu_reorder");
	player_sheet_attach_drag(container, true);
	settingsrow_page_title(menuarrange_screen);
	lv_obj_t *tabs = lv_obj_create(container);
	lv_obj_set_size(tabs, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tabs, 0, 0);
	lv_obj_set_style_pad_all(tabs, 0, 0);
	lv_obj_set_style_pad_gap(tabs, bp_pick(4, 8), 0);
	lv_obj_set_scrollable(tabs, false);
	lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
	home_tab = make_tab(tabs, "menu_section_home", MENUORDER_HOME);
	more_tab = make_tab(tabs, "more", MENUORDER_MORE);
	streaming_tab = make_tab(tabs, "streaming", MENUORDER_STREAMING);
	rows = lv_obj_create(container);
	lv_obj_set_width(rows, lv_pct(100));
	lv_obj_set_flex_grow(rows, 1);
	lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(rows, 0, 0);
	lv_obj_set_style_pad_all(rows, 0, 0);
	lv_obj_set_style_pad_gap(rows, bp_pick(4, 8), 0);
	lv_obj_set_scroll_dir(rows, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
	shown_section = MENUORDER_HOME;
	repaint();
	theme_register_refresh(paint_tabs);
}
