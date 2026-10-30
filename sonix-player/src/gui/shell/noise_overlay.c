#include "noise_overlay.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/earfun.h"
#include "src/system/bluetooth/airpods.h"
#include "src/system/device/power.h"

#define NOISE_OVERLAY_HIDE_MS 1600
#define NOISE_OVERLAY_POLL_MS 250

static lv_obj_t *veil;
static lv_obj_t *overlay;
static lv_obj_t *overlay_icon;
static lv_obj_t *overlay_label;
static lv_timer_t *hide_timer;
static lv_timer_t *poll_timer;

static bool is_compact;
static noise_overlay_mode_t current_mode = NOISE_OVERLAY_OFF;

static earfun_noise_t last_ef_noise = EARFUN_NOISE_UNKNOWN;
static bool ef_was_connected;

static airpods_noise_t last_ap_noise = AIRPODS_NOISE_UNKNOWN;
static bool ap_was_connected;

static void apply_theme(void) {
	if (!overlay) {
		return;
	}

	lv_obj_set_style_bg_color(overlay, theme()->panel, 0);
	lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);

	lv_color_t icon_color = (current_mode == NOISE_OVERLAY_ANC) ? theme()->accent : theme()->text_primary;
	lv_obj_set_style_image_recolor(overlay_icon, icon_color, 0);
	lv_obj_set_style_image_recolor_opa(overlay_icon, LV_OPA_COVER, 0);

	lv_obj_set_style_text_color(overlay_label, theme()->text_primary, 0);
}

static void hide_cb(lv_timer_t *timer) {
	(void)timer;
	if (veil) {
		lv_obj_set_hidden(veil, true);
	}
	if (hide_timer) {
		lv_timer_pause(hide_timer);
	}
}

void noise_overlay_show(noise_overlay_mode_t mode) {
	if (!overlay || !veil) {
		return;
	}

	current_mode = mode;
	const lv_image_dsc_t *icon = &icon_headphones;
	const char *text = "Off";

	switch (mode) {
	case NOISE_OVERLAY_ANC:
		icon = &icon_airpods_anc;
		text = "ANC";
		break;
	case NOISE_OVERLAY_AMBIENT:
		icon = &icon_airpods_transparency;
		text = "Ambient";
		break;
	case NOISE_OVERLAY_OFF:
	default:
		icon = &icon_headphones;
		text = "Off";
		break;
	}

	lv_image_set_src(overlay_icon, icon);
	if (is_compact) {
		uint32_t base_w = icon->header.w ? icon->header.w : 32;
		lv_image_set_scale(overlay_icon, (uint32_t)(LV_SCALE_NONE * 18 / base_w));
	}
	lv_label_set_text(overlay_label, text);

	apply_theme();

	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);

	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	earfun_state_t ef;
	if (earfun_get(&ef)) {
		if (!ef_was_connected) {
			ef_was_connected = true;
			last_ef_noise = ef.noise;
		} else if (ef.noise != last_ef_noise && ef.noise != EARFUN_NOISE_UNKNOWN) {
			last_ef_noise = ef.noise;
			if (ef.noise == EARFUN_NOISE_CANCELLATION) {
				noise_overlay_show(NOISE_OVERLAY_ANC);
			} else if (ef.noise == EARFUN_NOISE_TRANSPARENCY) {
				noise_overlay_show(NOISE_OVERLAY_AMBIENT);
			} else if (ef.noise == EARFUN_NOISE_OFF) {
				noise_overlay_show(NOISE_OVERLAY_OFF);
			}
		}
	} else {
		ef_was_connected = false;
		last_ef_noise = EARFUN_NOISE_UNKNOWN;
	}

	airpods_state_t ap;
	if (airpods_get(&ap)) {
		if (!ap_was_connected) {
			ap_was_connected = true;
			last_ap_noise = ap.noise;
		} else if (ap.noise != last_ap_noise && ap.noise != AIRPODS_NOISE_UNKNOWN) {
			last_ap_noise = ap.noise;
			if (ap.noise == AIRPODS_NOISE_CANCELLATION) {
				noise_overlay_show(NOISE_OVERLAY_ANC);
			} else if (ap.noise == AIRPODS_NOISE_TRANSPARENCY) {
				noise_overlay_show(NOISE_OVERLAY_AMBIENT);
			} else if (ap.noise == AIRPODS_NOISE_OFF) {
				noise_overlay_show(NOISE_OVERLAY_OFF);
			}
		}
	} else {
		ap_was_connected = false;
		last_ap_noise = AIRPODS_NOISE_UNKNOWN;
	}
}

void noise_overlay_init(gui_config_t *cfg) {
	is_compact = cfg->screen_width < 320;

	const int overlay_w = is_compact ? 124 : 160;
	const int overlay_h = is_compact ? 32 : 40;
	const int top_y = is_compact ? (cfg->top_bar_height + 4) : (cfg->top_bar_height + 8);

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, false);
	lv_obj_set_hidden(veil, true);

	overlay = lv_obj_create(veil);
	lv_obj_set_size(overlay, overlay_w, overlay_h);
	lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, top_y);
	lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(overlay, 0, 0);
	lv_obj_set_style_shadow_width(overlay, 6, 0);
	lv_obj_set_style_shadow_opa(overlay, LV_OPA_30, 0);
	lv_obj_set_style_shadow_color(overlay, lv_color_black(), 0);
	lv_obj_set_style_pad_hor(overlay, is_compact ? 12 : 16, 0);
	lv_obj_set_style_pad_ver(overlay, 0, 0);
	lv_obj_set_style_pad_gap(overlay, 8, 0);
	lv_obj_set_scrollable(overlay, false);
	lv_obj_set_clickable(overlay, false);

	lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	overlay_icon = lv_image_create(overlay);
	lv_image_set_src(overlay_icon, &icon_headphones);
	if (is_compact) {
		lv_obj_set_size(overlay_icon, 18, 18);
		lv_image_set_inner_align(overlay_icon, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_scale(overlay_icon, (uint32_t)(LV_SCALE_NONE * 18 / 26));
	}

	overlay_label = lv_label_create(overlay);
	lv_label_set_text(overlay_label, "Off");
	lv_obj_set_style_text_font(overlay_label, is_compact ? &font_ui_14 : &font_ui_16, 0);
	lv_obj_set_style_text_align(overlay_label, LV_TEXT_ALIGN_CENTER, 0);

	hide_timer = lv_timer_create(hide_cb, NOISE_OVERLAY_HIDE_MS, NULL);
	lv_timer_pause(hide_timer);

	poll_timer = lv_timer_create(poll_cb, NOISE_OVERLAY_POLL_MS, NULL);
	power_pause_in_standby(poll_timer);

	apply_theme();
	theme_register_refresh(apply_theme);
}
