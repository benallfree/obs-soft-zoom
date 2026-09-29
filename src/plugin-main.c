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

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-soft-zoom", "en-US")

extern struct obs_source_info soft_zoom_filter;

bool obs_module_load(void)
{
	obs_register_source(&soft_zoom_filter);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "unloaded");
}
