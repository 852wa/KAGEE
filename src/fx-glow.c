/* Kagee Glow & Flare filter: multi-radius bloom plus anamorphic streak / star flares */
#include "common.h"

enum flare_mode { FLARE_NONE = 0, FLARE_STREAK, FLARE_CROSS, FLARE_STAR };

struct glow {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texrender_t *input, *bright;
	struct kg_blur blur_a, blur_b, streak_a, streak_b;

	float threshold, knee, saturation, intensity, radius;
	struct vec4 tint, streak_tint;
	int flare;
	float streak_len, streak_int, streak_angle;
};

static const char *glow_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Glow.Name");
}

static void glow_update(void *data, obs_data_t *s)
{
	struct glow *f = data;
	f->threshold = (float)obs_data_get_double(s, "threshold") / 100.0f;
	f->knee = (float)obs_data_get_double(s, "knee") / 100.0f * 0.5f + 0.001f;
	f->saturation = (float)obs_data_get_double(s, "saturation") / 100.0f;
	f->intensity = (float)obs_data_get_double(s, "intensity") / 100.0f;
	f->radius = (float)obs_data_get_double(s, "radius");
	vec4_from_rgba(&f->tint, (uint32_t)obs_data_get_int(s, "tint"));
	f->flare = (int)obs_data_get_int(s, "flare");
	f->streak_len = (float)obs_data_get_double(s, "streak_len");
	f->streak_int = (float)obs_data_get_double(s, "streak_int") / 100.0f;
	f->streak_angle = (float)obs_data_get_double(s, "streak_angle");
	vec4_from_rgba(&f->streak_tint, (uint32_t)obs_data_get_int(s, "streak_tint"));
}

static void glow_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "threshold", 70.0);
	obs_data_set_default_double(s, "knee", 30.0);
	obs_data_set_default_double(s, "saturation", 100.0);
	obs_data_set_default_double(s, "intensity", 80.0);
	obs_data_set_default_double(s, "radius", 40.0);
	obs_data_set_default_int(s, "tint", 0xFFFFFFFF);
	obs_data_set_default_int(s, "flare", FLARE_NONE);
	obs_data_set_default_double(s, "streak_len", 300.0);
	obs_data_set_default_double(s, "streak_int", 60.0);
	obs_data_set_default_double(s, "streak_angle", 0.0);
	obs_data_set_default_int(s, "streak_tint", 0xFFFFC080); /* ABGR: light blue */
}

static bool flare_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	bool on = obs_data_get_int(s, "flare") != FLARE_NONE;
	obs_property_set_visible(obs_properties_get(props, "streak_len"), on);
	obs_property_set_visible(obs_properties_get(props, "streak_int"), on);
	obs_property_set_visible(obs_properties_get(props, "streak_angle"), on);
	obs_property_set_visible(obs_properties_get(props, "streak_tint"), on);
	return true;
}

static obs_properties_t *glow_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "threshold", T_("Glow.Threshold"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "knee", T_("Glow.Knee"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "intensity", T_("Glow.Intensity"), 0.0, 500.0, 1.0);
	obs_properties_add_float_slider(props, "radius", T_("Glow.Radius"), 1.0, 300.0, 1.0);
	obs_properties_add_float_slider(props, "saturation", T_("Glow.Saturation"), 0.0, 200.0, 1.0);
	obs_properties_add_color(props, "tint", T_("Glow.Tint"));

	obs_property_t *p =
		obs_properties_add_list(props, "flare", T_("Glow.Flare"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Off"), FLARE_NONE);
	obs_property_list_add_int(p, T_("Glow.Streak"), FLARE_STREAK);
	obs_property_list_add_int(p, T_("Glow.Cross"), FLARE_CROSS);
	obs_property_list_add_int(p, T_("Glow.Star"), FLARE_STAR);
	obs_property_set_modified_callback(p, flare_modified);
	obs_properties_add_float_slider(props, "streak_len", T_("Glow.StreakLength"), 10.0, 1500.0, 1.0);
	obs_properties_add_float_slider(props, "streak_int", T_("Glow.StreakIntensity"), 0.0, 500.0, 1.0);
	obs_properties_add_float_slider(props, "streak_angle", T_("Glow.StreakAngle"), -90.0, 90.0, 0.5);
	obs_properties_add_color(props, "streak_tint", T_("Glow.StreakTint"));
	return props;
}

static void *glow_create(obs_data_t *settings, obs_source_t *source)
{
	struct glow *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/glow.effect");
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bright = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	kg_blur_init(&f->blur_a);
	kg_blur_init(&f->blur_b);
	kg_blur_init(&f->streak_a);
	kg_blur_init(&f->streak_b);
	obs_leave_graphics();
	glow_update(f, settings);
	return f;
}

static void glow_destroy(void *data)
{
	struct glow *f = data;
	obs_enter_graphics();
	gs_texrender_destroy(f->input);
	gs_texrender_destroy(f->bright);
	kg_blur_free(&f->blur_a);
	kg_blur_free(&f->blur_b);
	kg_blur_free(&f->streak_a);
	kg_blur_free(&f->streak_b);
	obs_leave_graphics();
	bfree(f);
}

static void glow_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct glow *f = data;
	uint32_t cx, cy;
	if (!f->effect || !kg_filter_capture(f->context, f->input, &cx, &cy)) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	gs_texture_t *base = gs_texrender_get_texture(f->input);
	gs_effect_t *e = f->effect;

	uint32_t hw = cx / 2 ? cx / 2 : 1, hh = cy / 2 ? cy / 2 : 1;
	gs_effect_set_float(gs_effect_get_param_by_name(e, "threshold"), f->threshold);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "knee"), f->knee);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "saturation"), f->saturation);
	gs_texture_t *bright = kg_pass(f->bright, e, "Bright", base, hw, hh);
	if (!bright) {
		kg_draw_premultiplied(base, cx, cy);
		return;
	}

	gs_texture_t *ba = kg_blur_apply(&f->blur_a, bright, hw, hh, f->radius * 0.35f, NULL);
	gs_texture_t *bb = kg_blur_apply(&f->blur_b, bright, hw, hh, f->radius * 1.2f, NULL);

	gs_texture_t *sa = NULL, *sb = NULL;
	int streaks = 0;
	if (f->flare != FLARE_NONE) {
		float ang = KG_DEG2RAD(f->streak_angle);
		struct vec2 d1, d2;
		if (f->flare == FLARE_STREAK) {
			vec2_set(&d1, cosf(ang), sinf(ang));
			sa = kg_blur_apply(&f->streak_a, bright, hw, hh, f->streak_len * 0.5f, &d1);
			streaks = 1;
		} else {
			float base_ang = f->flare == FLARE_CROSS ? ang + KG_PI * 0.25f : ang;
			vec2_set(&d1, cosf(base_ang), sinf(base_ang));
			vec2_set(&d2, -sinf(base_ang), cosf(base_ang));
			sa = kg_blur_apply(&f->streak_a, bright, hw, hh, f->streak_len * 0.5f, &d1);
			sb = kg_blur_apply(&f->streak_b, bright, hw, hh, f->streak_len * 0.5f, &d2);
			streaks = 2;
		}
	}

	gs_effect_set_texture(gs_effect_get_param_by_name(e, "bloom_a"), ba);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "bloom_b"), bb);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "streak_a"), sa ? sa : ba);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "streak_b"), sb ? sb : ba);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "bloom_int"), f->intensity);
	/* a long 1D blur spreads a highlight's energy over ~length px: scale the gain with length
	 * so streaks stay visible (the composite's screen blend keeps the cores from clipping hard) */
	float streak_gain = sqrtf(kg_clampf(f->streak_len / 40.0f, 1.0f, 40.0f));
	gs_effect_set_float(gs_effect_get_param_by_name(e, "streak_int"), f->streak_int * streak_gain);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "tint"), &f->tint);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "streak_tint"), &f->streak_tint);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "streaks"), streaks);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), base);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Composite"))
		gs_draw_sprite(base, 0, cx, cy);
	gs_blend_state_pop();
}

struct obs_source_info kagee_glow_filter = {
	.id = "kagee_glow_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = glow_name,
	.create = glow_create,
	.destroy = glow_destroy,
	.update = glow_update,
	.get_defaults = glow_defaults,
	.get_properties = glow_props,
	.video_render = glow_render,
};
