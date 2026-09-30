#include "soft-zoom-spotlight.h"

#include <plugin-support.h>
#include <obs-frontend-api.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static obs_source_t *g_hooked_scene;

static int g_selected_count;
static struct vec2 g_last_selected_size;
static obs_source_t *g_last_selected_source;

static int g_visible_capture_count;
static struct vec2 g_only_visible_capture_size;
static obs_source_t *g_only_visible_capture_source;

static bool g_cache_valid;

static int g_log_state;
static obs_source_t *g_log_source;
static struct vec2 g_log_size;
static int g_log_selected_count;
static int g_log_visible_captures;

static obs_source_t *g_log_filter_parent;
static struct vec2 g_log_filter_parent_size;

#define LOG_STATE_NONE 0
#define LOG_STATE_FILTER_PARENT 4
#define LOG_STATE_REFERENCE 1
#define LOG_STATE_ZERO 2
#define LOG_STATE_FALLBACK 3

#define SPOTLIGHT_MAX_LOG_SELECTED 12

struct spotlight_selected_log {
	obs_source_t *source;
	struct vec2 raw_box;
	struct vec2 canvas_size;
};

static bool is_display_capture(obs_source_t *source)
{
	if (!source)
		return false;

	const char *id = obs_source_get_id(source);
	return id && (!strcmp(id, "screen_capture") || !strcmp(id, "display_capture") || !strcmp(id, "monitor_capture"));
}

static bool is_capture_sceneitem(obs_sceneitem_t *item)
{
	return is_display_capture(obs_sceneitem_get_source(item));
}

static struct vec2 item_box_canvas_size(obs_sceneitem_t *item, const struct vec2 *canvas_scale)
{
	struct vec2 box;
	obs_sceneitem_get_box_scale(item, &box);
	box.x *= canvas_scale->x;
	box.y *= canvas_scale->y;
	return box;
}

struct spotlight_enum_pass {
	struct vec2 canvas_scale;

	int selected_count;
	struct vec2 last_selected_size;
	obs_source_t *last_selected_source;

	int selected_log_count;
	struct spotlight_selected_log selected_log[SPOTLIGHT_MAX_LOG_SELECTED];

	int visible_capture_count;
	struct vec2 only_visible_capture_size;
	obs_source_t *only_visible_capture_source;
};

static void note_selected(struct spotlight_enum_pass *pass, obs_sceneitem_t *item, const struct vec2 *raw_box,
			  const struct vec2 *canvas_size)
{
	pass->selected_count++;
	pass->last_selected_size = *canvas_size;
	pass->last_selected_source = obs_sceneitem_get_source(item);

	if (pass->selected_log_count < SPOTLIGHT_MAX_LOG_SELECTED) {
		struct spotlight_selected_log *row = &pass->selected_log[pass->selected_log_count++];
		row->source = pass->last_selected_source;
		row->raw_box = *raw_box;
		row->canvas_size = *canvas_size;
	}
}

struct find_source_pass {
	obs_source_t *target;
	struct vec2 canvas_scale;
	struct vec2 best_size;
	bool found;
};

static bool enum_find_source_box(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);

	struct find_source_pass *pass = param;

	if (obs_sceneitem_is_group(item)) {
		const struct vec2 saved_scale = pass->canvas_scale;

		struct vec2 gbox;
		obs_sceneitem_get_box_scale(item, &gbox);
		gbox.x *= pass->canvas_scale.x;
		gbox.y *= pass->canvas_scale.y;

		obs_source_t *group_source = obs_sceneitem_get_source(item);
		const uint32_t iw = group_source ? obs_source_get_width(group_source) : 0;
		const uint32_t ih = group_source ? obs_source_get_height(group_source) : 0;
		if (iw && ih) {
			pass->canvas_scale.x *= gbox.x / (float)iw;
			pass->canvas_scale.y *= gbox.y / (float)ih;
		}

		obs_sceneitem_group_enum_items(item, enum_find_source_box, pass);
		pass->canvas_scale = saved_scale;
		return true;
	}

	if (obs_sceneitem_get_source(item) == pass->target && obs_sceneitem_visible(item)) {
		pass->best_size = item_box_canvas_size(item, &pass->canvas_scale);
		pass->found = true;
	}

	return true;
}

static bool canvas_box_for_source(obs_scene_t *scene, obs_source_t *source, struct vec2 *out)
{
	if (!scene || !source || !out)
		return false;

	struct find_source_pass pass = {0};
	pass.target = source;
	pass.canvas_scale.x = 1.f;
	pass.canvas_scale.y = 1.f;

	obs_scene_enum_items(scene, enum_find_source_box, &pass);
	if (!pass.found || pass.best_size.x <= 0.f || pass.best_size.y <= 0.f)
		return false;

	*out = pass.best_size;
	return true;
}

static bool enum_spotlight_layers(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);

	struct spotlight_enum_pass *pass = param;

	if (obs_sceneitem_is_group(item)) {
		const struct vec2 saved_scale = pass->canvas_scale;

		if (obs_sceneitem_selected(item)) {
			struct vec2 raw;
			obs_sceneitem_get_box_scale(item, &raw);
			const struct vec2 size = item_box_canvas_size(item, &pass->canvas_scale);
			note_selected(pass, item, &raw, &size);
		}

		struct vec2 gbox;
		obs_sceneitem_get_box_scale(item, &gbox);
		gbox.x *= pass->canvas_scale.x;
		gbox.y *= pass->canvas_scale.y;

		obs_source_t *group_source = obs_sceneitem_get_source(item);
		const uint32_t iw = group_source ? obs_source_get_width(group_source) : 0;
		const uint32_t ih = group_source ? obs_source_get_height(group_source) : 0;
		if (iw && ih) {
			pass->canvas_scale.x *= gbox.x / (float)iw;
			pass->canvas_scale.y *= gbox.y / (float)ih;
		}

		obs_sceneitem_group_enum_items(item, enum_spotlight_layers, pass);
		pass->canvas_scale = saved_scale;
		return true;
	}

	struct vec2 raw;
	obs_sceneitem_get_box_scale(item, &raw);
	const struct vec2 size = item_box_canvas_size(item, &pass->canvas_scale);

	if (is_capture_sceneitem(item) && obs_sceneitem_visible(item)) {
		pass->visible_capture_count++;
		pass->only_visible_capture_size = size;
		pass->only_visible_capture_source = obs_sceneitem_get_source(item);
	}

	if (obs_sceneitem_selected(item))
		note_selected(pass, item, &raw, &size);

	return true;
}

static void log_spotlight_selection_detail(const struct spotlight_enum_pass *pass)
{
	if (pass->selected_log_count == 0)
		return;

	for (int i = 0; i < pass->selected_log_count; i++) {
		const struct spotlight_selected_log *row = &pass->selected_log[i];
		const char *name = row->source ? obs_source_get_name(row->source) : "(null)";
		obs_log(LOG_INFO,
			"spotlight selected[%d] %s item_box_scale=%.1fx%.1f canvas_size=%.0fx%.0f (topmost wins)", i, name,
			row->raw_box.x, row->raw_box.y, row->canvas_size.x, row->canvas_size.y);
	}
}

static void log_spotlight_state(const struct spotlight_enum_pass *pass)
{
	struct obs_video_info ovi;
	const bool have_canvas = obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height;

	int new_state = LOG_STATE_FALLBACK;
	obs_source_t *src = NULL;
	struct vec2 size = {0};

	if (pass->selected_count > 0) {
		size = pass->last_selected_size;
		src = pass->last_selected_source;
		new_state = (size.x > 0.f && size.y > 0.f) ? LOG_STATE_REFERENCE : LOG_STATE_ZERO;
	} else if (pass->visible_capture_count == 1) {
		size = pass->only_visible_capture_size;
		src = pass->only_visible_capture_source;
		new_state = (size.x > 0.f && size.y > 0.f) ? LOG_STATE_REFERENCE : LOG_STATE_ZERO;
	}

	if (new_state == g_log_state && src == g_log_source && g_log_size.x == size.x && g_log_size.y == size.y &&
	    g_log_selected_count == pass->selected_count && g_log_visible_captures == pass->visible_capture_count)
		return;

	g_log_state = new_state;
	g_log_source = src;
	g_log_size = size;
	g_log_selected_count = pass->selected_count;
	g_log_visible_captures = pass->visible_capture_count;

	const char *name = src ? obs_source_get_name(src) : "(none)";

	log_spotlight_selection_detail(pass);

	if (new_state == LOG_STATE_FALLBACK) {
		obs_log(LOG_INFO,
			"spotlight fallback full display (selected=%d visible_captures=%d) — crop stays 1/zoom on both axes",
			pass->selected_count, pass->visible_capture_count);
		if (pass->visible_capture_count == 1 && pass->only_visible_capture_source) {
			const char *cap = obs_source_get_name(pass->only_visible_capture_source);
			obs_log(LOG_INFO, "spotlight sole visible capture %s size %.0fx%.0f (not used: nothing selected)",
				cap, pass->only_visible_capture_size.x, pass->only_visible_capture_size.y);
		}
		return;
	}

	if (new_state == LOG_STATE_ZERO) {
		obs_log(LOG_INFO, "spotlight zero box for %s (selected=%d) — crop stays 1/zoom on both axes", name,
			pass->selected_count);
		return;
	}

	const char *via = pass->selected_count > 0 ? "selection" : "sole visible capture";
	if (have_canvas) {
		const float frac_w = size.x / (float)ovi.base_width;
		const float frac_h = size.y / (float)ovi.base_height;
		obs_log(LOG_INFO, "spotlight reference %s via %s box %.0fx%.0f canvas px (frac %.3fx%.3f)", name, via,
			size.x, size.y, frac_w, frac_h);
	} else {
		obs_log(LOG_INFO, "spotlight reference %s via %s box %.0fx%.0f canvas px", name, via, size.x, size.y);
	}
}

static void refresh_cache_from_scene(obs_scene_t *scene)
{
	struct spotlight_enum_pass pass = {0};
	pass.canvas_scale.x = 1.f;
	pass.canvas_scale.y = 1.f;

	obs_scene_enum_items(scene, enum_spotlight_layers, &pass);

	pthread_mutex_lock(&g_cache_mutex);
	g_selected_count = pass.selected_count;
	g_last_selected_size = pass.last_selected_size;
	g_last_selected_source = pass.last_selected_source;
	g_visible_capture_count = pass.visible_capture_count;
	g_only_visible_capture_size = pass.only_visible_capture_size;
	g_only_visible_capture_source = pass.only_visible_capture_source;
	g_cache_valid = true;
	pthread_mutex_unlock(&g_cache_mutex);

	log_spotlight_state(&pass);
}

void soft_zoom_spotlight_refresh_cache(void)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene)
		refresh_cache_from_scene(scene);

	obs_source_release(scene_source);
}

void soft_zoom_spotlight_tick(void)
{
	soft_zoom_spotlight_refresh_cache();
}

static void scene_changed(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(cd);
	soft_zoom_spotlight_refresh_cache();
}

static void disconnect_scene_signals(obs_source_t *scene_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(scene_source);
	signal_handler_disconnect(sh, "item_add", scene_changed, NULL);
	signal_handler_disconnect(sh, "item_remove", scene_changed, NULL);
	signal_handler_disconnect(sh, "item_select", scene_changed, NULL);
	signal_handler_disconnect(sh, "item_deselect", scene_changed, NULL);
	signal_handler_disconnect(sh, "item_transform", scene_changed, NULL);
	signal_handler_disconnect(sh, "item_visible", scene_changed, NULL);
	signal_handler_disconnect(sh, "reorder", scene_changed, NULL);
	signal_handler_disconnect(sh, "refresh", scene_changed, NULL);
}

static void connect_scene_signals(obs_source_t *scene_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(scene_source);
	signal_handler_connect(sh, "item_add", scene_changed, NULL);
	signal_handler_connect(sh, "item_remove", scene_changed, NULL);
	signal_handler_connect(sh, "item_select", scene_changed, NULL);
	signal_handler_connect(sh, "item_deselect", scene_changed, NULL);
	signal_handler_connect(sh, "item_transform", scene_changed, NULL);
	signal_handler_connect(sh, "item_visible", scene_changed, NULL);
	signal_handler_connect(sh, "reorder", scene_changed, NULL);
	signal_handler_connect(sh, "refresh", scene_changed, NULL);
}

void soft_zoom_spotlight_rehook_scene(void)
{
	if (g_hooked_scene) {
		disconnect_scene_signals(g_hooked_scene);
		obs_source_release(g_hooked_scene);
		g_hooked_scene = NULL;
	}

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;

	connect_scene_signals(scene_source);
	g_hooked_scene = scene_source;
	g_log_state = LOG_STATE_NONE;
	obs_log(LOG_INFO, "spotlight scene hook %s", obs_source_get_name(scene_source));
	soft_zoom_spotlight_refresh_cache();
}

void soft_zoom_spotlight_init(void)
{
	soft_zoom_spotlight_rehook_scene();
}

void soft_zoom_spotlight_shutdown(void)
{
	if (g_hooked_scene) {
		disconnect_scene_signals(g_hooked_scene);
		obs_source_release(g_hooked_scene);
		g_hooked_scene = NULL;
	}

	pthread_mutex_lock(&g_cache_mutex);
	g_cache_valid = false;
	g_selected_count = 0;
	g_visible_capture_count = 0;
	pthread_mutex_unlock(&g_cache_mutex);

	g_log_state = LOG_STATE_NONE;
	g_log_source = NULL;
	g_log_size.x = 0.f;
	g_log_size.y = 0.f;
	g_log_filter_parent = NULL;
	g_log_filter_parent_size.x = 0.f;
	g_log_filter_parent_size.y = 0.f;
}

static void log_filter_parent_size(obs_source_t *filter_parent, const struct vec2 *size)
{
	if (filter_parent == g_log_filter_parent && g_log_filter_parent_size.x == size->x &&
	    g_log_filter_parent_size.y == size->y && g_log_state == LOG_STATE_FILTER_PARENT)
		return;

	g_log_filter_parent = filter_parent;
	g_log_filter_parent_size = *size;
	g_log_state = LOG_STATE_FILTER_PARENT;

	const char *name = obs_source_get_name(filter_parent);
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		const float frac_w = size->x / (float)ovi.base_width;
		const float frac_h = size->y / (float)ovi.base_height;
		obs_log(LOG_INFO, "spotlight reference %s via filter_parent box %.0fx%.0f canvas px (frac %.3fx%.3f)", name,
			size->x, size->y, frac_w, frac_h);
	} else {
		obs_log(LOG_INFO, "spotlight reference %s via filter_parent box %.0fx%.0f canvas px", name, size->x,
			size->y);
	}
}

bool soft_zoom_spotlight_layer_size(obs_source_t *filter_parent, float *layer_w, float *layer_h)
{
	if (!layer_w || !layer_h)
		return false;

	pthread_mutex_lock(&g_cache_mutex);

	if (!g_cache_valid) {
		pthread_mutex_unlock(&g_cache_mutex);
		soft_zoom_spotlight_refresh_cache();
		pthread_mutex_lock(&g_cache_mutex);
		if (!g_cache_valid) {
			pthread_mutex_unlock(&g_cache_mutex);
			return false;
		}
	}

	struct vec2 size = {0};
	bool ok = false;

	if (g_selected_count > 0) {
		size = g_last_selected_size;
		ok = true;
	} else if (g_visible_capture_count == 1) {
		size = g_only_visible_capture_size;
		ok = true;
	}

	pthread_mutex_unlock(&g_cache_mutex);

	if (ok && size.x > 0.f && size.y > 0.f) {
		*layer_w = size.x;
		*layer_h = size.y;
		return true;
	}

	if (!filter_parent)
		return false;

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return false;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	struct vec2 parent_size = {0};
	const bool have_box = scene && canvas_box_for_source(scene, filter_parent, &parent_size);
	obs_source_release(scene_source);

	if (!have_box)
		return false;

	log_filter_parent_size(filter_parent, &parent_size);
	*layer_w = parent_size.x;
	*layer_h = parent_size.y;
	return true;
}

bool soft_zoom_spotlight_overlay_owner(obs_source_t **owner)
{
	if (!owner)
		return false;

	*owner = NULL;

	pthread_mutex_lock(&g_cache_mutex);

	if (!g_cache_valid) {
		pthread_mutex_unlock(&g_cache_mutex);
		soft_zoom_spotlight_refresh_cache();
		pthread_mutex_lock(&g_cache_mutex);
	}

	if (g_selected_count > 0)
		*owner = g_last_selected_source;
	else if (g_visible_capture_count == 1)
		*owner = g_only_visible_capture_source;

	pthread_mutex_unlock(&g_cache_mutex);

	return *owner != NULL;
}
