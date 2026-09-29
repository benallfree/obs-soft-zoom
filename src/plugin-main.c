/*
OBS Soft Zoom
Copyright (C) 2026 Ben Allfree

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>
#include "soft-zoom-filter-internal.h"
#include "soft-zoom-settings.h"
#include "zoom-outline.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-soft-zoom", "en-US")

extern struct obs_source_info soft_zoom_filter;

static obs_hotkey_id g_master_toggle_hotkey = OBS_INVALID_HOTKEY_ID;

static void global_toggle_hotkey(void *unused, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	soft_zoom_filter_master_toggle();
}

bool obs_module_load(void)
{
	soft_zoom_settings_load();
	obs_register_source(&soft_zoom_filter);
	g_master_toggle_hotkey =
		obs_hotkey_register_frontend("obs_soft_zoom.toggle", obs_module_text("SoftZoom.Toggle"),
					     global_toggle_hotkey, NULL);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (g_master_toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_master_toggle_hotkey);
		g_master_toggle_hotkey = OBS_INVALID_HOTKEY_ID;
	}
	soft_zoom_settings_save();
	zoom_outline_shutdown();
	obs_log(LOG_INFO, "unloaded");
}
