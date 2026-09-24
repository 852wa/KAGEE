/* Kagee Lens filter: distortion, chromatic aberration, sharpen, grain, vignette, letterbox */
#include "common.h"

struct lens {
	obs_source_t *context;
	gs_effect_t *effect;
	float distortion, ca, sharpen, grain, grain_size;
	float vig_amount, vig_radius, vig_soft, vig_round;
	struct vec4 vig_color;
	float lb_ratio;
	struct vec4 lb_color;
	float seed;
};

static const char *lens_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Lens.Name");
}

static void color_param(struct vec4 *out, uint32_t rgba)
{
	vec4_from_rgba(out, rgba);
}

static void lens_update(void *data, obs_data_t *s)
{
	struct lens *f = data;
	f->distortion = (float)obs_data_get_double(s, "distortion") / 100.0f * 0.6f;
	f->ca = (float)obs_data_get_double(s, "ca") / 100.0f;
	f->sharpen = (float)obs_data_get_double(s, "sharpen") / 100.0f * 1.5f;
	f->grain = (float)obs_data_get_double(s, "grain") / 100.0f;
	f->grain_size = (float)obs_data_get_double(s, "grain_size");
	f->vig_amount = (float)obs_data_get_double(s, "vig_amount") / 100.0f;
	f->vig_radius = (float)obs_data_get_double(s, "vig_radius") / 100.0f;
	f->vig_soft = (float)obs_data_get_double(s, "vig_soft") / 100.0f;
	f->vig_round = (float)obs_data_get_double(s, "vig_round") / 100.0f;
	color_param(&f->vig_color, (uint32_t)obs_data_get_int(s, "vig_color"));
	f->lb_ratio = (float)obs_data_get_double(s, "lb_ratio");
	color_param(&f->lb_color, (uint32_t)obs_data_get_int(s, "lb_color"));
}

static void lens_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "distortion", 0.0);
	obs_data_set_default_double(s, "ca", 15.0);
	obs_data_set_default_double(s, "sharpen", 0.0);
	obs_data_set_default_double(s, "grain", 8.0);
	obs_data_set_default_double(s, "grain_size", 1.5);
	obs_data_set_default_double(s, "vig_amount", 45.0);
	obs_data_set_default_double(s, "vig_radius", 55.0);
	obs_data_set_default_double(s, "vig_soft", 80.0);
	obs_data_set_default_double(s, "vig_round", 50.0);
	obs_data_set_default_int(s, "vig_color", 0xFF000000);
	obs_data_set_default_double(s, "lb_ratio", 0.0);
	obs_data_set_default_int(s, "lb_color", 0xFF000000);
}

static obs_properties_t *lens_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "distortion", T_("Lens.Distortion"), -100.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "ca", T_("Lens.CA"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "sharpen", T_("Lens.Sharpen"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "grain", T_("Lens.Grain"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "grain_size", T_("Lens.GrainSize"), 1.0, 8.0, 0.1);

	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "vig_amount", T_("Lens.VigAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "vig_radius", T_("Lens.VigRadius"), 0.0, 150.0, 1.0);
	obs_properties_add_float_slider(g, "vig_soft", T_("Lens.VigSoft"), 0.0, 150.0, 1.0);
	obs_properties_add_float_slider(g, "vig_round", T_("Lens.VigRound"), 0.0, 100.0, 1.0);
	obs_properties_add_color_alpha(g, "vig_color", T_("Lens.VigColor"));
	obs_properties_add_group(props, "grp_vig", T_("Lens.Vignette"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(g, "lb_ratio", T_("Lens.Letterbox"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(p, T_("Off"), 0.0);
	obs_property_list_add_float(p, "2.39:1 (Scope)", 2.39);
	obs_property_list_add_float(p, "2:1", 2.0);
	obs_property_list_add_float(p, "1.85:1 (Vista)", 1.85);
	obs_property_list_add_float(p, "16:9", 16.0 / 9.0);
	obs_property_list_add_float(p, "4:3", 4.0 / 3.0);
	obs_property_list_add_float(p, "1:1", 1.0);
	obs_property_list_add_float(p, "9:16", 9.0 / 16.0);
	obs_properties_add_color_alpha(g, "lb_color", T_("Lens.LetterboxColor"));
	obs_properties_add_group(props, "grp_lb", T_("Lens.LetterboxGroup"), OBS_GROUP_NORMAL, g);
	return props;
}

static void *lens_create(obs_data_t *settings, obs_source_t *source)
{
	struct lens *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/lens.effect");
	obs_leave_graphics();
	if (!f->effect) {
		bfree(f);
		return NULL;
	}
	lens_update(f, settings);
	return f;
}

static void lens_destroy(void *data)
{
	bfree(data);
}

static void lens_tick(void *data, float seconds)
{
	struct lens *f = data;
	UNUSED_PARAMETER(seconds);
	f->seed = fmodf(f->seed + 1.618f, 97.0f);
}

static void lens_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct lens *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t cx = obs_source_get_base_width(target), cy = obs_source_get_base_height(target);
	if (!cx || !cy || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	float aspect = (float)cx / (float)cy;
	float rmax2 = 0.25f * aspect * aspect + 0.25f;
	float zoom = f->distortion > 0.0f ? 1.0f / (1.0f + f->distortion * rmax2) : 1.0f;
	struct vec2 texel;
	vec2_set(&texel, 1.0f / (float)cx, 1.0f / (float)cy);

	gs_effect_t *e = f->effect;
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "texel"), &texel);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "aspect"), aspect);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "distortion"), f->distortion);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "dist_zoom"), zoom);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "ca_amount"), f->ca);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "sharpen"), f->sharpen);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "grain"), f->grain);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "grain_size"), f->grain_size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "seed"), f->seed);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "vig_amount"), f->vig_amount);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "vig_radius"), f->vig_radius);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "vig_soft"), f->vig_soft);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "vig_round"), f->vig_round);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "vig_color"), &f->vig_color);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "lb_ratio"), f->lb_ratio);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "lb_color"), &f->lb_color);

	obs_source_process_filter_end(f->context, e, 0, 0);
}

struct obs_source_info kagee_lens_filter = {
	.id = "kagee_lens_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = lens_name,
	.create = lens_create,
	.destroy = lens_destroy,
	.update = lens_update,
	.get_defaults = lens_defaults,
	.get_properties = lens_props,
	.video_tick = lens_tick,
	.video_render = lens_render,
};
