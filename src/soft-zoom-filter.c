#include <obs-module.h>
#include <util/platform.h>
#include <math.h>
#include <plugin-support.h>
#include "soft-zoom-filter-internal.h"
#include "soft-zoom-settings.h"
#include "soft-zoom-spotlight.h"
#include "zoom-outline.h"

static bool soft_zoom_settings_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	soft_zoom_settings_commit_obs_data(settings);
	return false;
}

static void bind_settings_modified(obs_property_t *property)
{
	if (property)
		obs_property_set_modified_callback(property, soft_zoom_settings_modified);
}

static bool reset_globals_clicked(obs_properties_t *props, obs_property_t *property, void *unused)
{
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(unused);

	soft_zoom_settings_reset_to_factory();

	obs_data_t *settings = obs_data_create();
	soft_zoom_settings_fill_obs_data(settings);
	obs_properties_apply_settings(props, settings);
	obs_data_release(settings);
	return true;
}

static const char *soft_zoom_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SoftZoom");
}

static bool is_display_capture(obs_source_t *parent)
{
	if (!parent)
		return false;

	const char *id = obs_source_get_id(parent);
	return id && (!strcmp(id, "screen_capture") || !strcmp(id, "display_capture") || !strcmp(id, "monitor_capture"));
}

static uint32_t capture_display_id(obs_source_t *parent)
{
	obs_data_t *settings = obs_source_get_settings(parent);
	if (!settings)
		return 0;

	const int type = (int)obs_data_get_int(settings, "type");
	const char *uuid = obs_data_get_string(settings, "display_uuid");
	uint32_t display_id = 0;

	if (type == 0 && uuid && *uuid)
		display_id = zoom_display_id_from_uuid(uuid);

	obs_data_release(settings);
	return display_id;
}

static float smoothstep(float t)
{
	if (t <= 0.f)
		return 0.f;
	if (t >= 1.f)
		return 1.f;
	return t * t * (3.f - 2.f * t);
}

static struct {
	obs_source_t *parent;
	float uv_crop;
	float uv_left;
	float uv_top;
	float overlay_w;
	float overlay_h;
	float overlay_left;
	float overlay_top;
	struct vec2 src_mul;
	struct vec2 src_add;
} g_last_crop_log;

static void log_compute_crop_if_changed(obs_source_t *parent, float zoom, const struct vec2 *src_mul,
					const struct vec2 *src_add, float uv_crop, float uv_left, float uv_top,
					float overlay_w, float overlay_h, float overlay_left, float overlay_top)
{
	if (g_last_crop_log.parent == parent && g_last_crop_log.uv_crop == uv_crop && g_last_crop_log.uv_left == uv_left &&
	    g_last_crop_log.uv_top == uv_top && g_last_crop_log.overlay_w == overlay_w &&
	    g_last_crop_log.overlay_h == overlay_h && g_last_crop_log.overlay_left == overlay_left &&
	    g_last_crop_log.overlay_top == overlay_top && g_last_crop_log.src_mul.x == src_mul->x &&
	    g_last_crop_log.src_mul.y == src_mul->y && g_last_crop_log.src_add.x == src_add->x &&
	    g_last_crop_log.src_add.y == src_add->y)
		return;

	g_last_crop_log.parent = parent;
	g_last_crop_log.uv_crop = uv_crop;
	g_last_crop_log.uv_left = uv_left;
	g_last_crop_log.uv_top = uv_top;
	g_last_crop_log.overlay_w = overlay_w;
	g_last_crop_log.overlay_h = overlay_h;
	g_last_crop_log.overlay_left = overlay_left;
	g_last_crop_log.overlay_top = overlay_top;
	g_last_crop_log.src_mul = *src_mul;
	g_last_crop_log.src_add = *src_add;

	const char *parent_name = parent ? obs_source_get_name(parent) : "(no parent)";
	obs_log(LOG_INFO,
		"soft zoom crop parent=%s zoom=%.2f uv=%.4f at=%.4f,%.4f overlay=%.4fx%.4f at=%.4f,%.4f src_mul=%.4fx%.4f add=%.4f,%.4f",
		parent_name, zoom, uv_crop, uv_left, uv_top, overlay_w, overlay_h, overlay_left, overlay_top, src_mul->x,
		src_mul->y, src_add->x, src_add->y);
}

static bool soft_zoom_input_crop_map(obs_source_t *filter, struct vec2 *src_mul, struct vec2 *src_add)
{
	src_mul->x = 1.f;
	src_mul->y = 1.f;
	src_add->x = 0.f;
	src_add->y = 0.f;

	if (!filter)
		return false;

	obs_source_t *target = obs_filter_get_target(filter);
	if (!target)
		return false;

	const char *id = obs_source_get_id(target);
	if (!id || strcmp(id, "crop_filter"))
		return false;

	obs_source_t *base = obs_filter_get_target(target);
	if (!base)
		return false;

	const uint32_t width = obs_source_get_base_width(base);
	const uint32_t height = obs_source_get_base_height(base);
	if (!width || !height)
		return false;

	obs_data_t *settings = obs_source_get_settings(target);
	if (!settings)
		return false;

	const bool absolute = !obs_data_get_bool(settings, "relative");
	const int left = (int)obs_data_get_int(settings, "left");
	const int top = (int)obs_data_get_int(settings, "top");
	const int right = (int)obs_data_get_int(settings, "right");
	const int bottom = (int)obs_data_get_int(settings, "bottom");
	const int abs_cx = (int)obs_data_get_int(settings, "cx");
	const int abs_cy = (int)obs_data_get_int(settings, "cy");
	obs_data_release(settings);

	int crop_w;
	int crop_h;

	if (absolute) {
		crop_w = abs_cx;
		crop_h = abs_cy;
	} else {
		crop_w = (int)width - left - right;
		crop_h = (int)height - top - bottom;
	}

	if (crop_w < 1)
		crop_w = 1;
	if (crop_h < 1)
		crop_h = 1;

	src_mul->x = (float)crop_w / (float)width;
	src_mul->y = (float)crop_h / (float)height;
	src_add->x = (float)left / (float)width;
	src_add->y = (float)top / (float)height;
	return true;
}

static float clamp01(float v)
{
	if (v < 0.f)
		return 0.f;
	if (v > 1.f)
		return 1.f;
	return v;
}

static void display_norm_to_texture(float dx, float dy, const struct vec2 *src_mul, const struct vec2 *src_add,
				    float *tx, float *ty)
{
	if (src_mul->x > 0.0001f)
		*tx = (dx - src_add->x) / src_mul->x;
	else
		*tx = dx;

	if (src_mul->y > 0.0001f)
		*ty = (dy - src_add->y) / src_mul->y;
	else
		*ty = dy;

	*tx = clamp01(*tx);
	*ty = clamp01(*ty);
}

static void texture_rect_to_display(float uv_left, float uv_top, float uv_w, float uv_h, const struct vec2 *src_mul,
				    const struct vec2 *src_add, float *disp_left, float *disp_top, float *disp_w,
				    float *disp_h)
{
	*disp_left = uv_left * src_mul->x + src_add->x;
	*disp_top = uv_top * src_mul->y + src_add->y;
	*disp_w = uv_w * src_mul->x;
	*disp_h = uv_h * src_mul->y;
}

static void clamp_crop_rect(float crop_w, float crop_h, float *left, float *top)
{
	if (*left < 0.f)
		*left = 0.f;
	if (*top < 0.f)
		*top = 0.f;
	if (*left + crop_w > 1.f)
		*left = 1.f - crop_w;
	if (*top + crop_h > 1.f)
		*top = 1.f - crop_h;
}

static void anchor_crop_origin(float crop_w, float crop_h, bool center_fallback, float anchor_x, float anchor_y,
			       int anchor_mode, float *left, float *top)
{
	if (center_fallback) {
		*left = 0.5f - crop_w * 0.5f;
		*top = 0.5f - crop_h * 0.5f;
		return;
	}

	switch (anchor_mode) {
	case ANCHOR_UPPER_LEFT:
		*left = anchor_x;
		*top = anchor_y;
		break;
	case ANCHOR_UPPER_RIGHT:
		*left = anchor_x - crop_w;
		*top = anchor_y;
		break;
	case ANCHOR_LOWER_LEFT:
		*left = anchor_x;
		*top = anchor_y - crop_h;
		break;
	case ANCHOR_LOWER_RIGHT:
		*left = anchor_x - crop_w;
		*top = anchor_y - crop_h;
		break;
	case ANCHOR_CENTER:
	default:
		*left = anchor_x - crop_w * 0.5f;
		*top = anchor_y - crop_h * 0.5f;
		break;
	}
}

static void compute_crop(struct soft_zoom_filter *f, float zoom)
{
	if (zoom <= 1.001f) {
		f->mul_val.x = 1.f;
		f->mul_val.y = 1.f;
		f->add_val.x = 0.f;
		f->add_val.y = 0.f;
		f->overlay_mul.x = 1.f;
		f->overlay_mul.y = 1.f;
		f->overlay_add.x = 0.f;
		f->overlay_add.y = 0.f;
		return;
	}

	const float uv_crop = 1.f / zoom;

	struct vec2 src_mul;
	struct vec2 src_add;
	soft_zoom_input_crop_map(f->context, &src_mul, &src_add);

	float anchor_tx;
	float anchor_ty;
	if (f->center_fallback) {
		display_norm_to_texture(0.5f, 0.5f, &src_mul, &src_add, &anchor_tx, &anchor_ty);
	} else {
		display_norm_to_texture(f->anchor_x, f->anchor_y, &src_mul, &src_add, &anchor_tx, &anchor_ty);
	}

	float uv_left;
	float uv_top;
	anchor_crop_origin(uv_crop, uv_crop, false, anchor_tx, anchor_ty, f->anchor_mode, &uv_left, &uv_top);
	clamp_crop_rect(uv_crop, uv_crop, &uv_left, &uv_top);

	float overlay_left;
	float overlay_top;
	float overlay_w;
	float overlay_h;
	texture_rect_to_display(uv_left, uv_top, uv_crop, uv_crop, &src_mul, &src_add, &overlay_left, &overlay_top,
				&overlay_w, &overlay_h);

	f->overlay_mul.x = overlay_w;
	f->overlay_mul.y = overlay_h;
	f->overlay_add.x = overlay_left;
	f->overlay_add.y = overlay_top;

	f->mul_val.x = uv_crop;
	f->mul_val.y = uv_crop;
	f->add_val.x = uv_left;
	f->add_val.y = uv_top;

	log_compute_crop_if_changed(obs_filter_get_parent(f->context), zoom, &src_mul, &src_add, uv_crop, uv_left,
				    uv_top, overlay_w, overlay_h, overlay_left, overlay_top);
}

void soft_zoom_filter_sync_overlay(struct soft_zoom_filter *f, obs_source_t *parent)
{
	obs_source_t *overlay_owner = NULL;
	if (!soft_zoom_spotlight_overlay_owner(&overlay_owner) || parent != overlay_owner) {
		if (f->overlay_shown) {
			zoom_outline_hide();
			f->overlay_shown = false;
		}
		return;
	}

	if (parent && !obs_source_showing(parent)) {
		if (f->overlay_shown) {
			zoom_outline_hide();
			f->overlay_shown = false;
		}
		return;
	}

	if (!f->zoom_active || f->animating || f->zoom_current <= 1.001f) {
		if (f->overlay_shown) {
			zoom_outline_hide();
			f->overlay_shown = false;
		}
		return;
	}

	if (f->outline_thickness <= 0 && f->dim_opacity <= 0) {
		if (f->overlay_shown) {
			zoom_outline_hide();
			f->overlay_shown = false;
		}
		return;
	}

	uint32_t display_id = capture_display_id(parent);
	zoom_display_frame display = {0};
	if (!zoom_display_frame_for_id(display_id, &display))
		return;

	const float crop_w = f->overlay_mul.x;
	const float crop_h = f->overlay_mul.y;
	const float left = f->overlay_add.x;
	const float top = f->overlay_add.y;

	zoom_outline_params params = {0};
	params.display_x = display.x;
	params.display_y = display.y;
	params.display_w = display.w;
	params.display_h = display.h;
	params.region_x = left;
	params.region_y = top;
	params.region_w = crop_w;
	params.region_h = crop_h;
	params.outline_thickness = f->outline_thickness;
	params.dim_opacity = f->dim_opacity;

	static struct {
		float region_w;
		float region_h;
		uint32_t display_w;
		uint32_t display_h;
	} g_last_overlay_log;

	const bool overlay_changed = g_last_overlay_log.region_w != crop_w || g_last_overlay_log.region_h != crop_h ||
				     g_last_overlay_log.display_w != (uint32_t)display.w ||
				     g_last_overlay_log.display_h != (uint32_t)display.h;
	if (overlay_changed) {
		g_last_overlay_log.region_w = crop_w;
		g_last_overlay_log.region_h = crop_h;
		g_last_overlay_log.display_w = (uint32_t)display.w;
		g_last_overlay_log.display_h = (uint32_t)display.h;
		const char *parent_name = parent ? obs_source_get_name(parent) : "(no parent)";
		obs_log(LOG_INFO,
			"soft zoom overlay parent=%s display=%.0fx%.0f region_norm=%.4fx%.4f at=%.4f,%.4f physical_focus=%.0fx%.0f px",
			parent_name, display.w, display.h, crop_w, crop_h, left, top, crop_w * display.w,
			crop_h * display.h);
	}

	if (f->overlay_shown)
		zoom_outline_update(&params);
	else {
		zoom_outline_show(&params);
		f->overlay_shown = true;
	}
}

static void refresh_anchor_from_cursor(struct soft_zoom_filter *f, obs_source_t *parent)
{
	if (!is_display_capture(parent)) {
		f->center_fallback = true;
		return;
	}

	uint32_t display_id = capture_display_id(parent);
	bool on_display = false;
	float cx = 0.5f;
	float cy = 0.5f;

	zoom_cursor_normalized_on_display(display_id, &cx, &cy, &on_display);
	if (!on_display) {
		f->center_fallback = true;
		return;
	}

	f->center_fallback = false;
	f->anchor_x = cx;
	f->anchor_y = cy;
}

static void start_anim(struct soft_zoom_filter *f, float to)
{
	f->anim_from = f->zoom_current;
	f->anim_to = to;
	f->anim_elapsed = 0.f;
	f->animating = true;
}

void soft_zoom_filter_on_global_settings_changed(struct soft_zoom_filter *f)
{
	if (!f || !f->context || !f->zoom_active)
		return;

	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!f->animating && f->zoom_current > 1.001f) {
		const float target = (float)f->zoom_factor;
		if (fabsf(f->zoom_current - target) > 0.01f)
			start_anim(f, target);
	}

	compute_crop(f, f->zoom_current);

	if (parent && !f->animating)
		soft_zoom_filter_sync_overlay(f, parent);
}

void soft_zoom_filter_set_active(struct soft_zoom_filter *f, bool active)
{
	if (!f || !f->context)
		return;

	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!parent)
		return;

	if (active) {
		if (f->zoom_active)
			return;
		refresh_anchor_from_cursor(f, parent);
		f->zoom_active = true;
		start_anim(f, (float)f->zoom_factor);
		return;
	}

	if (!f->zoom_active)
		return;

	f->zoom_active = false;
	if (f->overlay_shown) {
		zoom_outline_hide();
		f->overlay_shown = false;
	}
	start_anim(f, 1.f);
}

static void note_any_active(struct soft_zoom_filter *f, void *param)
{
	if (f->zoom_active)
		*(bool *)param = true;
}

static void apply_master_target(struct soft_zoom_filter *f, void *param)
{
	soft_zoom_filter_set_active(f, *(bool *)param);
}

void soft_zoom_filter_master_toggle(void)
{
	bool any_active = false;
	soft_zoom_settings_for_each(note_any_active, &any_active);

	bool target_active = !any_active;
	soft_zoom_settings_for_each(apply_master_target, &target_active);
}

static void *soft_zoom_create(obs_data_t *settings, obs_source_t *context)
{
	UNUSED_PARAMETER(settings);

	struct soft_zoom_filter *f = bzalloc(sizeof(*f));
	char *effect_path = obs_module_file("soft_zoom.effect");

	f->context = context;
	f->zoom_current = 1.f;
	f->anchor_x = 0.5f;
	f->anchor_y = 0.5f;
	f->center_fallback = false;

	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);

	if (!f->effect) {
		bfree(f);
		return NULL;
	}

	f->param_mul = gs_effect_get_param_by_name(f->effect, "mul_val");
	f->param_add = gs_effect_get_param_by_name(f->effect, "add_val");
	f->param_multiplier = gs_effect_get_param_by_name(f->effect, "multiplier");

	soft_zoom_settings_register(f);
	soft_zoom_settings_apply_to_filter(f);
	return f;
}

static void soft_zoom_destroy(void *data)
{
	struct soft_zoom_filter *f = data;

	soft_zoom_settings_unregister(f);
	if (f->overlay_shown)
		zoom_outline_hide();

	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();

	bfree(f);
}

static void soft_zoom_update(void *data, obs_data_t *settings)
{
	struct soft_zoom_filter *f = data;
	UNUSED_PARAMETER(settings);

	soft_zoom_settings_apply_to_filter(f);
	soft_zoom_filter_on_global_settings_changed(f);
}

static void soft_zoom_defaults(obs_data_t *settings)
{
	soft_zoom_settings_set_defaults(settings);
}

static obs_properties_t *soft_zoom_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "soft_zoom_info", obs_module_text("SoftZoom.SettingsGlobal"), OBS_TEXT_INFO);

	obs_property_t *zoom =
		obs_properties_add_int_slider(props, "zoom", obs_module_text("SoftZoom.Zoom"), 2, 8, 2);
	obs_property_int_set_suffix(zoom, "x");
	bind_settings_modified(zoom);

	obs_property_t *ease = obs_properties_add_int_slider(props, "ease_ms", obs_module_text("SoftZoom.EaseMs"), 0, 2000,
							     10);
	bind_settings_modified(ease);

	obs_property_t *outline =
		obs_properties_add_int_slider(props, "outline", obs_module_text("SoftZoom.OutlineThickness"), 0, 50, 1);
	bind_settings_modified(outline);

	obs_property_t *dim =
		obs_properties_add_int_slider(props, "dim", obs_module_text("SoftZoom.DimOpacity"), 0, 100, 1);
	bind_settings_modified(dim);

	obs_property_t *anchor = obs_properties_add_list(props, "anchor", obs_module_text("SoftZoom.Anchor"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(anchor, obs_module_text("SoftZoom.Anchor.Center"), ANCHOR_CENTER);
	obs_property_list_add_int(anchor, obs_module_text("SoftZoom.Anchor.UpperLeft"), ANCHOR_UPPER_LEFT);
	obs_property_list_add_int(anchor, obs_module_text("SoftZoom.Anchor.UpperRight"), ANCHOR_UPPER_RIGHT);
	obs_property_list_add_int(anchor, obs_module_text("SoftZoom.Anchor.LowerLeft"), ANCHOR_LOWER_LEFT);
	obs_property_list_add_int(anchor, obs_module_text("SoftZoom.Anchor.LowerRight"), ANCHOR_LOWER_RIGHT);
	bind_settings_modified(anchor);

	obs_property_t *follow =
		obs_properties_add_bool(props, "follow_mouse", obs_module_text("SoftZoom.FollowMouse"));
	bind_settings_modified(follow);

	obs_properties_add_button2(props, "reset_globals", obs_module_text("SoftZoom.ResetGlobals"),
				   reset_globals_clicked, NULL);

	obs_data_t *display = obs_data_create();
	soft_zoom_settings_fill_obs_data(display);
	obs_properties_apply_settings(props, display);
	obs_data_release(display);

	return props;
}

static void soft_zoom_tick(void *data, float seconds)
{
	struct soft_zoom_filter *f = data;
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (f->animating) {
		const float duration = f->ease_ms <= 0 ? 0.f : (float)f->ease_ms / 1000.f;
		if (duration <= 0.f) {
			f->zoom_current = f->anim_to;
			f->animating = false;
		} else {
			f->anim_elapsed += seconds;
			float t = f->anim_elapsed / duration;
			if (t >= 1.f) {
				t = 1.f;
				f->animating = false;
			}
			const float s = smoothstep(t);
			f->zoom_current = f->anim_from + (f->anim_to - f->anim_from) * s;
		}

		if (!f->animating && f->zoom_current <= 1.001f) {
			f->zoom_current = 1.f;
			f->center_fallback = false;
		}
	}

	if (f->zoom_active && f->follow_mouse && parent && obs_source_showing(parent))
		refresh_anchor_from_cursor(f, parent);

	if (f->zoom_active && f->zoom_current > 1.001f)
		soft_zoom_spotlight_tick();

	compute_crop(f, f->zoom_current);

	if (parent) {
		if (f->zoom_active && f->zoom_current > 1.001f) {
			if (!f->animating)
				soft_zoom_filter_sync_overlay(f, parent);
			else if (f->overlay_shown) {
				zoom_outline_hide();
				f->overlay_shown = false;
			}
		} else if (f->overlay_shown) {
			zoom_outline_hide();
			f->overlay_shown = false;
		}
	}
}

static const char *get_tech_name_and_multiplier(enum gs_color_space current_space, enum gs_color_space source_space,
						float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		case GS_CS_709_EXTENDED:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_SCRGB:
			break;
		}
	}

	return tech_name;
}

static void soft_zoom_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct soft_zoom_filter *f = data;

	if (f->zoom_current <= 1.001f && !f->animating) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const uint32_t width = obs_source_get_base_width(target);
	const uint32_t height = obs_source_get_base_height(target);
	if (!width || !height) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space =
		obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(gs_get_color_space(), source_space, &multiplier);
	const enum gs_color_format format = gs_get_format_from_space(source_space);

	if (obs_source_process_filter_begin_with_color_space(f->context, format, source_space, OBS_NO_DIRECT_RENDERING)) {
		gs_effect_set_vec2(f->param_mul, &f->mul_val);
		gs_effect_set_vec2(f->param_add, &f->add_val);
		gs_effect_set_float(f->param_multiplier, multiplier);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_process_filter_tech_end(f->context, f->effect, width, height, technique);
		gs_blend_state_pop();
	}
}

static enum gs_color_space soft_zoom_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	struct soft_zoom_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return GS_CS_SRGB;

	const enum gs_color_space source_space =
		obs_source_get_color_space(target, OBS_COUNTOF(potential_spaces), potential_spaces);

	enum gs_color_space space = source_space;
	for (size_t i = 0; i < count; ++i) {
		space = preferred_spaces[i];
		if (space == source_space)
			break;
	}

	return space;
}

struct obs_source_info soft_zoom_filter = {
	.id = "soft_zoom_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = soft_zoom_name,
	.create = soft_zoom_create,
	.destroy = soft_zoom_destroy,
	.update = soft_zoom_update,
	.get_defaults = soft_zoom_defaults,
	.get_properties = soft_zoom_properties,
	.video_tick = soft_zoom_tick,
	.video_render = soft_zoom_render,
	.video_get_color_space = soft_zoom_get_color_space,
};
