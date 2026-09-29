#include <obs-module.h>
#include <util/platform.h>
#include "soft-zoom-filter-internal.h"
#include "soft-zoom-settings.h"
#include "zoom-outline.h"

static zoom_outline_owner_id overlay_owner_id(const struct soft_zoom_filter *f)
{
	return (zoom_outline_owner_id)(uintptr_t)f;
}

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

static void compute_crop(struct soft_zoom_filter *f, float zoom)
{
	if (zoom <= 1.001f) {
		f->mul_val.x = 1.f;
		f->mul_val.y = 1.f;
		f->add_val.x = 0.f;
		f->add_val.y = 0.f;
		return;
	}

	const float crop_w = 1.f / zoom;
	const float crop_h = 1.f / zoom;

	float left;
	float top;
	if (f->center_fallback) {
		left = 0.5f - crop_w * 0.5f;
		top = 0.5f - crop_h * 0.5f;
	} else {
		const float px = f->anchor_x;
		const float py = f->anchor_y;

		switch (f->anchor_mode) {
		case ANCHOR_UPPER_LEFT:
			left = px;
			top = py;
			break;
		case ANCHOR_UPPER_RIGHT:
			left = px - crop_w;
			top = py;
			break;
		case ANCHOR_LOWER_LEFT:
			left = px;
			top = py - crop_h;
			break;
		case ANCHOR_LOWER_RIGHT:
			left = px - crop_w;
			top = py - crop_h;
			break;
		case ANCHOR_CENTER:
		default:
			left = px - crop_w * 0.5f;
			top = py - crop_h * 0.5f;
			break;
		}
	}

	if (left < 0.f)
		left = 0.f;
	if (top < 0.f)
		top = 0.f;
	if (left + crop_w > 1.f)
		left = 1.f - crop_w;
	if (top + crop_h > 1.f)
		top = 1.f - crop_h;

	f->mul_val.x = crop_w;
	f->mul_val.y = crop_h;
	f->add_val.x = left;
	f->add_val.y = top;
}

void soft_zoom_filter_sync_overlay(struct soft_zoom_filter *f, obs_source_t *parent)
{
	if (parent && !obs_source_showing(parent)) {
		if (f->overlay_shown) {
			zoom_outline_hide_for(overlay_owner_id(f));
			f->overlay_shown = false;
		}
		return;
	}

	if (!f->zoom_active || f->zoom_current <= 1.001f || f->animating) {
		if (f->overlay_shown) {
			zoom_outline_hide_for(overlay_owner_id(f));
			f->overlay_shown = false;
		}
		return;
	}

	if (f->outline_thickness <= 0 && f->dim_opacity <= 0) {
		if (f->overlay_shown) {
			zoom_outline_hide_for(overlay_owner_id(f));
			f->overlay_shown = false;
		}
		return;
	}

	uint32_t display_id = capture_display_id(parent);
	zoom_display_frame display = {0};
	if (!zoom_display_frame_for_id(display_id, &display))
		return;

	const float crop_w = f->mul_val.x;
	const float crop_h = f->mul_val.y;
	const float left = f->add_val.x;
	const float top = f->add_val.y;

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

	const zoom_outline_owner_id owner = overlay_owner_id(f);
	if (f->overlay_shown)
		zoom_outline_update_for(owner, &params);
	else {
		zoom_outline_show_for(owner, &params);
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
	zoom_outline_hide_for(overlay_owner_id(f));
	f->overlay_shown = false;
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
	zoom_outline_destroy_for(overlay_owner_id(f));

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

	compute_crop(f, f->zoom_current);

	if (parent) {
		if (f->zoom_active && !f->animating)
			soft_zoom_filter_sync_overlay(f, parent);
		else if (!f->zoom_active && !f->animating && f->overlay_shown) {
			zoom_outline_hide_for(overlay_owner_id(f));
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
