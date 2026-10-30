#include "earfunpage.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/earfun.h"
#include "src/system/core/lang.h"

lv_obj_t *earfunpage_screen;

#define POLL_MS 700

static lv_obj_t *left_value;
static lv_obj_t *right_value;
static lv_obj_t *case_value;
static lv_obj_t *version_value;
static lv_obj_t *connection_value;

static lv_obj_t *noise_pills;
static lv_obj_t *noise_pill[3]; // the three modes, in EARFUN_NOISE_* order - 1
static lv_obj_t *anc_pills;
static lv_obj_t *anc_pill[3]; // strong, balanced, AI
static lv_obj_t *transparency_pills;
static lv_obj_t *transparency_pill[2]; // default, natural
static lv_obj_t *game_switch;
static lv_obj_t *in_ear_switch;

static lv_timer_t *poll_timer;
static uint32_t drawn_serial = 0xFFFFFFFFu;
static bool drawn_connected;

// ---------------------------------------------------------------------------
// The controls
// ---------------------------------------------------------------------------

static void noise_cb(lv_event_t *e) {
	earfun_set_noise((earfun_noise_t)(intptr_t)lv_event_get_user_data(e));
}

static void anc_cb(lv_event_t *e) {
	earfun_set_anc_level((int)(intptr_t)lv_event_get_user_data(e));
}

static void transparency_cb(lv_event_t *e) {
	earfun_set_transparency_mode((int)(intptr_t)lv_event_get_user_data(e));
}

static void game_cb(lv_event_t *e) {
	earfun_set_game_mode(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void in_ear_cb(lv_event_t *e) {
	earfun_set_in_ear(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void set_level(lv_obj_t *label, int percent) {
	if (!label) {
		return;
	}
	if (percent < 0) {
		lv_label_set_text(label, "--");
		return;
	}
	lv_label_set_text_fmt(label, "%d%%", percent);
}

static void redraw(const earfun_state_t *state) {
	if (connection_value) {
		lv_obj_set_hidden(connection_value, state->connected);
	}
	set_level(left_value, state->left);
	set_level(right_value, state->right);
	set_level(case_value, state->case_level);

	// The mode the earbuds report, not the one that was asked for: a pill that
	// lit up on the tap and stayed lit after the earbuds refused would be a
	// switch that lies about the room.
	for (int i = 0; i < 3; i++) {
		if (noise_pill[i]) {
			settingsrow_pill_active(noise_pill[i], (int)state->noise == i + 1);
			lv_obj_set_clickable(noise_pill[i], state->connected);
		}
	}
	for (int i = 0; i < 3; i++) {
		if (anc_pill[i]) {
			settingsrow_pill_active(anc_pill[i], state->anc_level == i);
			lv_obj_set_clickable(anc_pill[i], state->connected && state->noise == EARFUN_NOISE_CANCELLATION);
		}
	}
	for (int i = 0; i < 2; i++) {
		if (transparency_pill[i]) {
			settingsrow_pill_active(transparency_pill[i], state->transparency_mode == i);
			lv_obj_set_clickable(transparency_pill[i],
							 state->connected && state->noise == EARFUN_NOISE_TRANSPARENCY);
		}
	}
	if (game_switch) {
		if (state->game_mode) {
			lv_obj_add_state(game_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(game_switch, LV_STATE_CHECKED);
		}
		// Nothing to switch until the earbuds have said what it is now: the
		// switch would otherwise start in an arbitrary position.
		bool available = state->connected && state->game_mode_known;
		lv_obj_set_clickable(game_switch, available);
		lv_obj_set_style_opa(game_switch, available ? LV_OPA_COVER : LV_OPA_50, 0);
	}
	if (in_ear_switch) {
		if (state->in_ear) {
			lv_obj_add_state(in_ear_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(in_ear_switch, LV_STATE_CHECKED);
		}
		bool available = state->connected && state->in_ear_known;
		lv_obj_set_clickable(in_ear_switch, available);
		lv_obj_set_style_opa(in_ear_switch, available ? LV_OPA_COVER : LV_OPA_50, 0);
	}

	// The strength and variant only mean anything for their own mode, so their
	// cards step back while another mode is on rather than offering a setting
	// that would do nothing.
	bool cancelling = state->noise == EARFUN_NOISE_CANCELLATION;
	bool transparent = state->noise == EARFUN_NOISE_TRANSPARENCY;
	if (anc_pills) {
		lv_obj_set_style_opa(anc_pills, state->connected && cancelling ? LV_OPA_COVER : LV_OPA_50, 0);
	}
	if (transparency_pills) {
		lv_obj_set_style_opa(transparency_pills, state->connected && transparent ? LV_OPA_COVER : LV_OPA_50, 0);
	}

	if (version_value) {
		lv_label_set_text(version_value, state->version[0] ? state->version : "--");
	}
}

static void refresh(bool force) {
	earfun_state_t state;
	bool connected = earfun_get(&state);
	if (!force && state.serial == drawn_serial && connected == drawn_connected) {
		return;
	}
	drawn_serial = state.serial;
	drawn_connected = connected;
	// A page whose session has gone keeps the last reading for one redraw --
	// the numbers of earbuds that have just left are worse than a blank.
	if (!connected) {
		memset(&state, 0, sizeof(state));
		state.left = -1;
		state.right = -1;
		state.case_level = -1;
		state.anc_level = -1;
		state.transparency_mode = -1;
	}
	redraw(&state);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh(false);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh(true);
	lv_timer_resume(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
}

// ---------------------------------------------------------------------------
// Building it
// ---------------------------------------------------------------------------

void earfunpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(earfunpage_screen, cfg, "earfun");

	// --- what is left ---
	settingsrow_heading(container, "earfun_battery");
	lv_obj_t *connection_row = settingsrow_add(container, "connecting", &connection_value, NULL, NULL);
	lv_label_set_text(connection_value, tr("connecting"));
	lv_obj_set_hidden(connection_row, true);
	lv_obj_t *left_row = settingsrow_add(container, "earfun_left", &left_value, NULL, NULL);
	settingsrow_add_icon(left_row, &icon_headphones, THEME_SEMANTIC_CYAN);
	lv_obj_t *right_row = settingsrow_add(container, "earfun_right", &right_value, NULL, NULL);
	settingsrow_add_icon(right_row, &icon_headphones, THEME_SEMANTIC_CYAN);
	lv_obj_t *case_row = settingsrow_add(container, "earfun_case", &case_value, NULL, NULL);
	settingsrow_add_icon(case_row, &icon_battery, THEME_SEMANTIC_GREEN);
	set_level(left_value, -1);
	set_level(right_value, -1);
	set_level(case_value, -1);

	// --- noise control ---
	settingsrow_heading(container, "earfun_sound");
	settingsrow_pills(container, "earfun_noise", &noise_pills);
	noise_pill[0] = settingsrow_pill(noise_pills, "earfun_noise_off", EARFUN_NOISE_OFF, noise_cb);
	noise_pill[1] = settingsrow_pill(noise_pills, "earfun_noise_anc", EARFUN_NOISE_CANCELLATION, noise_cb);
	noise_pill[2] =
		settingsrow_pill(noise_pills, "earfun_noise_transparency", EARFUN_NOISE_TRANSPARENCY, noise_cb);

	// The strength of the cancellation and the kind of transparency are the
	// earbuds' own list settings: three and two answers, in the order the
	// earbuds' application offers them.
	settingsrow_pills(container, "earfun_anc_level", &anc_pills);
	anc_pill[0] = settingsrow_pill(anc_pills, "earfun_anc_strong", 0, anc_cb);
	anc_pill[1] = settingsrow_pill(anc_pills, "earfun_anc_balanced", 1, anc_cb);
	anc_pill[2] = settingsrow_pill(anc_pills, "earfun_anc_ai", 2, anc_cb);

	settingsrow_pills(container, "earfun_transparency", &transparency_pills);
	transparency_pill[0] =
		settingsrow_pill(transparency_pills, "earfun_transparency_default", 0, transparency_cb);
	transparency_pill[1] =
		settingsrow_pill(transparency_pills, "earfun_transparency_natural", 1, transparency_cb);

	// --- the two switches ---
	settingsrow_toggle(container, "earfun_game_mode", &game_switch, game_cb);
	settingsrow_toggle(container, "earfun_in_ear", &in_ear_switch, in_ear_cb);

	// --- what they are ---
	lv_obj_t *version_row = settingsrow_add(container, "earfun_firmware", &version_value, NULL, NULL);
	settingsrow_add_icon(version_row, &icon_headphones, THEME_SEMANTIC_MUTED);
	lv_label_set_text(version_value, "--");

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);
	lv_obj_add_event_cb(earfunpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(earfunpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(earfunpage_screen);
}
