#include "soft-zoom-filter-internal.h"
#include "soft-zoom-settings.h"

#include <stdio.h>
#include <string.h>

#include <util/darray.h>
#include <util/platform.h>

static struct soft_zoom_settings g_settings = {
	.zoom_factor = 2,
	.ease_ms = 250,
	.outline_thickness = 3,
	.dim_opacity = 0,
	.anchor_mode = ANCHOR_CENTER,
	.follow_mouse = true,
};

static DARRAY(struct soft_zoom_filter *) g_instances;

static int snap_zoom(int z)
{
	if (z <= 3)
		return 2;
	if (z <= 5)
		return 4;
	return 8;
}

static void settings_path(char *path, size_t size)
{
	char *base = obs_module_config_path(NULL);
	if (!base)
		return;
	snprintf(path, size, "%s/soft-zoom.json", base);
	bfree(base);
}

void soft_zoom_settings_load(void)
{
	char path[512];
	settings_path(path, sizeof(path));
	if (!path[0])
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "soft_zoom");
	if (!data)
		return;

	soft_zoom_settings_set_from_obs_data(data);
	obs_data_release(data);
}

void soft_zoom_settings_save(void)
{
	char path[512];
	settings_path(path, sizeof(path));
	if (!path[0])
		return;

	obs_data_t *data = obs_data_create();
	soft_zoom_settings_fill_obs_data(data);
	os_mkdirs(obs_module_config_path(NULL));
	obs_data_save_json(data, path);
	obs_data_release(data);
}

const struct soft_zoom_settings *soft_zoom_settings_get(void)
{
	return &g_settings;
}

void soft_zoom_settings_set_from_obs_data(obs_data_t *settings)
{
	if (!settings)
		return;

	if (obs_data_has_user_value(settings, "zoom"))
		g_settings.zoom_factor = snap_zoom((int)obs_data_get_int(settings, "zoom"));
	if (obs_data_has_user_value(settings, "ease_ms"))
		g_settings.ease_ms = (int)obs_data_get_int(settings, "ease_ms");
	if (obs_data_has_user_value(settings, "outline"))
		g_settings.outline_thickness = (int)obs_data_get_int(settings, "outline");
	if (obs_data_has_user_value(settings, "dim"))
		g_settings.dim_opacity = (int)obs_data_get_int(settings, "dim");
	if (obs_data_has_user_value(settings, "anchor"))
		g_settings.anchor_mode = (int)obs_data_get_int(settings, "anchor");
	if (obs_data_has_user_value(settings, "follow_mouse")) {
		obs_data_item_t *item = obs_data_item_byname(settings, "follow_mouse");
		if (item && obs_data_item_gettype(item) == OBS_DATA_NUMBER)
			g_settings.follow_mouse = obs_data_get_int(settings, "follow_mouse") != 0;
		else
			g_settings.follow_mouse = obs_data_get_bool(settings, "follow_mouse");
		obs_data_item_release(&item);
	}
}

void soft_zoom_settings_fill_obs_data(obs_data_t *settings)
{
	if (!settings)
		return;

	obs_data_set_int(settings, "zoom", g_settings.zoom_factor);
	obs_data_set_int(settings, "ease_ms", g_settings.ease_ms);
	obs_data_set_int(settings, "outline", g_settings.outline_thickness);
	obs_data_set_int(settings, "dim", g_settings.dim_opacity);
	obs_data_set_int(settings, "anchor", g_settings.anchor_mode);
	obs_data_set_bool(settings, "follow_mouse", g_settings.follow_mouse);
}

void soft_zoom_settings_set_defaults(obs_data_t *settings)
{
	soft_zoom_settings_fill_obs_data(settings);
}

bool soft_zoom_settings_commit_obs_data(obs_data_t *settings)
{
	if (!settings)
		return false;

	struct soft_zoom_settings prev = g_settings;
	soft_zoom_settings_set_from_obs_data(settings);
	if (memcmp(&prev, &g_settings, sizeof(prev)) == 0)
		return false;

	soft_zoom_settings_fill_obs_data(settings);
	soft_zoom_settings_save();
	soft_zoom_settings_apply_all();
	return true;
}

void soft_zoom_settings_apply_to_filter(struct soft_zoom_filter *f)
{
	if (!f)
		return;

	f->zoom_factor = g_settings.zoom_factor;
	f->ease_ms = g_settings.ease_ms;
	f->outline_thickness = g_settings.outline_thickness;
	f->dim_opacity = g_settings.dim_opacity;
	f->anchor_mode = g_settings.anchor_mode;
	f->follow_mouse = g_settings.follow_mouse;
}

void soft_zoom_settings_register(struct soft_zoom_filter *f)
{
	if (!f)
		return;
	for (size_t i = 0; i < g_instances.num; i++) {
		if (g_instances.array[i] == f)
			return;
	}
	da_push_back(g_instances, &f);
}

void soft_zoom_settings_unregister(struct soft_zoom_filter *f)
{
	if (!f)
		return;
	for (size_t i = 0; i < g_instances.num; i++) {
		if (g_instances.array[i] != f)
			continue;
		da_erase(g_instances, i);
		break;
	}
}

void soft_zoom_settings_apply_all(void)
{
	for (size_t i = 0; i < g_instances.num; i++) {
		soft_zoom_settings_apply_to_filter(g_instances.array[i]);
		soft_zoom_filter_on_global_settings_changed(g_instances.array[i]);
	}
}

void soft_zoom_settings_for_each(void (*fn)(struct soft_zoom_filter *f, void *param), void *param)
{
	if (!fn)
		return;
	for (size_t i = 0; i < g_instances.num; i++)
		fn(g_instances.array[i], param);
}
