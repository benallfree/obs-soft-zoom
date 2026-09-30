#pragma once

#include <obs-module.h>
#include <graphics/vec2.h>
#include <stdbool.h>

struct soft_zoom_filter {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_mul;
	gs_eparam_t *param_add;
	gs_eparam_t *param_multiplier;

	int zoom_factor;
	int ease_ms;
	int outline_thickness;
	int dim_opacity;
	int anchor_mode;
	bool follow_mouse;

	bool zoom_active;
	bool animating;
	float zoom_current;
	float anim_from;
	float anim_to;
	float anim_elapsed;

	float anchor_x;
	float anchor_y;
	bool center_fallback;

	struct vec2 mul_val;
	struct vec2 add_val;

	struct vec2 overlay_mul;
	struct vec2 overlay_add;

	bool overlay_shown;
};

void soft_zoom_filter_sync_overlay(struct soft_zoom_filter *f, obs_source_t *parent);
void soft_zoom_filter_on_global_settings_changed(struct soft_zoom_filter *f);
void soft_zoom_filter_set_active(struct soft_zoom_filter *f, bool active);
void soft_zoom_filter_master_toggle(void);
