#pragma once

#include <obs-module.h>
#include <stdbool.h>

#define ANCHOR_CENTER 0
#define ANCHOR_UPPER_LEFT 1
#define ANCHOR_UPPER_RIGHT 2
#define ANCHOR_LOWER_LEFT 3
#define ANCHOR_LOWER_RIGHT 4

struct soft_zoom_settings {
	int zoom_factor;
	int ease_ms;
	int outline_thickness;
	int dim_opacity;
	int anchor_mode;
	bool follow_mouse;
};

struct soft_zoom_filter;

void soft_zoom_settings_load(void);
void soft_zoom_settings_save(void);
const struct soft_zoom_settings *soft_zoom_settings_get(void);
void soft_zoom_settings_set_from_obs_data(obs_data_t *settings);
void soft_zoom_settings_fill_obs_data(obs_data_t *settings);
bool soft_zoom_settings_commit_obs_data(obs_data_t *settings);
void soft_zoom_settings_set_defaults(obs_data_t *settings);
void soft_zoom_settings_reset_to_factory(void);

void soft_zoom_settings_register(struct soft_zoom_filter *f);
void soft_zoom_settings_unregister(struct soft_zoom_filter *f);
void soft_zoom_settings_apply_to_filter(struct soft_zoom_filter *f);
void soft_zoom_settings_apply_all(void);
void soft_zoom_settings_for_each(void (*fn)(struct soft_zoom_filter *f, void *param), void *param);
