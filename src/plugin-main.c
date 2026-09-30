/*
OBS Soft Zoom
Copyright (C) 2026 Ben Allfree

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <plugin-support.h>
#include "soft-zoom-filter-internal.h"
#include "soft-zoom-settings.h"
#include "zoom-outline.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-soft-zoom", "en-US")

extern struct obs_source_info soft_zoom_filter;

static obs_hotkey_id g_master_toggle_hotkey = OBS_INVALID_HOTKEY_ID;

static const char *const k_toggle_hotkey_name = "obs_soft_zoom.toggle";

static void global_toggle_hotkey(void *unused, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	soft_zoom_filter_master_toggle();
}

static void load_toggle_hotkey_bindings(void)
{
	if (g_master_toggle_hotkey == OBS_INVALID_HOTKEY_ID)
		return;

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	const char *info = config_get_string(config, "Hotkeys", k_toggle_hotkey_name);
	if (!info || !*info)
		return;

	obs_data_t *data = obs_data_create_from_json(info);
	if (!data)
		return;

	obs_data_array_t *bindings = obs_data_get_array(data, "bindings");
	if (bindings)
		obs_hotkey_load(g_master_toggle_hotkey, bindings);

	obs_data_release(data);
}

static void ensure_toggle_hotkey_registered(void)
{
	if (g_master_toggle_hotkey != OBS_INVALID_HOTKEY_ID)
		return;

	g_master_toggle_hotkey = obs_hotkey_register_frontend(k_toggle_hotkey_name, obs_module_text("SoftZoom.Toggle"),
							      global_toggle_hotkey, NULL);
}

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		ensure_toggle_hotkey_registered();
		load_toggle_hotkey_bindings();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	soft_zoom_settings_load();
	obs_register_source(&soft_zoom_filter);
	ensure_toggle_hotkey_registered();
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	soft_zoom_settings_save();
	zoom_outline_shutdown();
	obs_log(LOG_INFO, "unloaded");
}
