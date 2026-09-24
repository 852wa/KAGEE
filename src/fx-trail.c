/* Kagee Motion Trail filter: echo / lighten / additive feedback trails with drift & zoom,
 * plus an optional frame-rate limiter (stutter look). The accumulation advances once per
 * video tick, so extra renders (projectors, multiview) don't speed it up. */
#include "common.h"

struct trail {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texrender_t *input;
	gs_texrender_t *acc[2];
	int cur;
	bool has_prev;
	uint32_t cx, cy;

	float decay;
	int mode;
	struct vec4 tint;
	float drift_x, drift_y, zoom;
	float fps_limit;

	float hold_timer;
	bool need_step;
};

static const char *trail_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Trail.Name");
}

static void trail_update(void *data, obs_data_t *s)
{
	struct trail *f = data;
	f->decay = (float)obs_data_get_double(s, "decay") / 100.0f;
	f->mode = (int)obs_data_get_int(s, "mode");
	vec4_from_rgba(&f->tint, (uint32_t)obs_data_get_int(s, "tint"));
	f->drift_x = (float)obs_data_get_double(s, "drift_x") / 1000.0f;
	f->drift_y = (float)obs_data_get_double(s, "drift_y") / 1000.0f;
	f->zoom = 1.0f + (float)obs_data_get_double(s, "zoom") / 1000.0f;
	f->fps_limit = (float)obs_data_get_double(s, "fps_limit");
}

static void trail_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "decay", 85.0);
	obs_data_set_default_int(s, "mode", 3);
	obs_data_set_default_int(s, "tint", 0xFFFFFFFF);
	obs_data_set_default_double(s, "drift_x", 0.0);
	obs_data_set_default_double(s, "drift_y", 0.0);
	obs_data_set_default_double(s, "zoom", 0.0);
	obs_data_set_default_double(s, "fps_limit", 0.0);
}

static obs_properties_t *trail_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_list(props, "mode", T_("Trail.Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Trail.Ghost"), 3);
	obs_property_list_add_int(p, T_("Trail.Echo"), 0);
	obs_property_list_add_int(p, T_("Trail.Lighten"), 1);
	obs_property_list_add_int(p, T_("Trail.Additive"), 2);
	obs_properties_add_float_slider(props, "decay", T_("Trail.Decay"), 0.0, 99.0, 0.5);
	obs_properties_add_color(props, "tint", T_("Trail.Tint"));
	obs_properties_add_float_slider(props, "drift_x", T_("Trail.DriftX"), -20.0, 20.0, 0.1);
	obs_properties_add_float_slider(props, "drift_y", T_("Trail.DriftY"), -20.0, 20.0, 0.1);
	obs_properties_add_float_slider(props, "zoom", T_("Trail.Zoom"), -30.0, 30.0, 0.1);
	obs_properties_add_float_slider(props, "fps_limit", T_("Trail.FpsLimit"), 0.0, 60.0, 0.5);
	return props;
}

static void *trail_create(obs_data_t *settings, obs_source_t *source)
{
	struct trail *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/trail.effect");
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->acc[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->acc[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	trail_update(f, settings);
	f->need_step = true;
	return f;
}

static void trail_destroy(void *data)
{
	struct trail *f = data;
	obs_enter_graphics();
	gs_texrender_destroy(f->input);
	gs_texrender_destroy(f->acc[0]);
	gs_texrender_destroy(f->acc[1]);
	obs_leave_graphics();
	bfree(f);
}

static void trail_tick(void *data, float seconds)
{
	struct trail *f = data;
	if (f->fps_limit > 0.0f) {
		f->hold_timer += seconds;
		if (f->hold_timer >= 1.0f / f->fps_limit) {
			f->hold_timer = fmodf(f->hold_timer, 1.0f / f->fps_limit);
			f->need_step = true;
		}
	} else {
		f->need_step = true;
	}
}

static void trail_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct trail *f = data;

	if (f->need_step || !f->has_prev) {
		uint32_t cx, cy;
		if (!f->effect || !kg_filter_capture(f->context, f->input, &cx, &cy)) {
			obs_source_skip_video_filter(f->context);
			return;
		}
		if (cx != f->cx || cy != f->cy) {
			f->cx = cx;
			f->cy = cy;
			f->has_prev = false;
		}

		gs_texture_t *cur = gs_texrender_get_texture(f->input);
		gs_texture_t *prev = f->has_prev ? gs_texrender_get_texture(f->acc[f->cur]) : cur;
		gs_effect_t *e = f->effect;
		struct vec2 drift;
		vec2_set(&drift, f->drift_x, f->drift_y);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "prev"), prev);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "decay"), f->has_prev ? f->decay : 0.0f);
		gs_effect_set_int(gs_effect_get_param_by_name(e, "mode"), f->mode);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "tint"), &f->tint);
		gs_effect_set_vec2(gs_effect_get_param_by_name(e, "drift"), &drift);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zoom"), f->zoom);

		int next = 1 - f->cur;
		if (kg_pass(f->acc[next], e, "Trail", cur, cx, cy)) {
			f->cur = next;
			f->has_prev = true;
		}
		f->need_step = false;
	}

	gs_texture_t *out = f->has_prev ? gs_texrender_get_texture(f->acc[f->cur]) : NULL;
	if (!out) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	kg_draw_premultiplied(out, f->cx, f->cy);
}

struct obs_source_info kagee_trail_filter = {
	.id = "kagee_trail_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = trail_name,
	.create = trail_create,
	.destroy = trail_destroy,
	.update = trail_update,
	.get_defaults = trail_defaults,
	.get_properties = trail_props,
	.video_tick = trail_tick,
	.video_render = trail_render,
};
