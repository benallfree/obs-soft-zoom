#pragma once

#include <obs-module.h>
#include <stdbool.h>

void soft_zoom_spotlight_init(void);
void soft_zoom_spotlight_shutdown(void);
void soft_zoom_spotlight_rehook_scene(void);
void soft_zoom_spotlight_refresh_cache(void);
void soft_zoom_spotlight_tick(void);

bool soft_zoom_spotlight_layer_size(obs_source_t *filter_parent, float *layer_w, float *layer_h);

bool soft_zoom_spotlight_overlay_owner(obs_source_t **owner);
