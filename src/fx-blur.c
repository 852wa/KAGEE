/* Kagee Blur filter: gaussian / iris / tilt-shift / directional / zoom & spin */
#include "common.h"

enum blur_mode { B_GAUSSIAN = 0, B_IRIS, B_TILT, B_DIRECTIONAL, B_ZOOM };

struct blurfx {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texrender_t *input;
	struct kg_blur blur;

	int mode;
	float radius, cx, cy, focus, softness, angle, zoom, spin;
	bool invert;
};

static const char *blurfx_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Blur.Name");
}

static void blurfx_update(void *data, obs_data_t *s)
{
	struct blurfx *f = data;
	f->mode = (int)obs_data_get_int(s, "mode");
	f->radius = (float)obs_data_get_double(s, "radius");
	f->cx = (float)obs_data_get_double(s, "center_x") / 100.0f;
	f->cy = (float)obs_data_get_double(s, "center_y") / 100.0f;
	f->focus = (float)obs_data_get_double(s, "focus") / 100.0f;
	f->softness = (float)obs_data_get_double(s, "softness") / 100.0f;
	f->angle = (float)obs_data_get_double(s, "angle");
	f->zoom = (float)obs_data_get_double(s, "zoom") / 100.0f;
	f->spin = (float)obs_data_get_double(s, "spin");
	f->invert = obs_data_get_bool(s, "invert");
}

static void blurfx_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "mode", B_IRIS);
	obs_data_set_default_double(s, "radius", 16.0);
	obs_data_set_default_double(s, "center_x", 50.0);
	obs_data_set_default_double(s, "center_y", 50.0);
	obs_data_set_default_double(s, "focus", 25.0);
	obs_data_set_default_double(s, "softness", 30.0);
	obs_data_set_default_double(s, "angle", 0.0);
	obs_data_set_default_double(s, "zoom", 15.0);
	obs_data_set_default_double(s, "spin", 0.0);
}

static bool blur_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	int m = (int)obs_data_get_int(s, "mode");
	bool masked = m == B_IRIS || m == B_TILT;
	obs_property_set_visible(obs_properties_get(props, "radius"), m != B_ZOOM);
	obs_property_set_visible(obs_properties_get(props, "center_x"), masked || m == B_ZOOM);
	obs_property_set_visible(obs_properties_get(props, "center_y"), masked || m == B_ZOOM);
	obs_property_set_visible(obs_properties_get(props, "focus"), masked);
	obs_property_set_visible(obs_properties_get(props, "softness"), masked);
	obs_property_set_visible(obs_properties_get(props, "invert"), masked);
	obs_property_set_visible(obs_properties_get(props, "angle"), m == B_TILT || m == B_DIRECTIONAL);
	obs_property_set_visible(obs_properties_get(props, "zoom"), m == B_ZOOM);
	obs_property_set_visible(obs_properties_get(props, "spin"), m == B_ZOOM);
	return true;
}

static obs_properties_t *blurfx_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_list(props, "mode", T_("Blur.Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Blur.Gaussian"), B_GAUSSIAN);
	obs_property_list_add_int(p, T_("Blur.Iris"), B_IRIS);
	obs_property_list_add_int(p, T_("Blur.Tilt"), B_TILT);
	obs_property_list_add_int(p, T_("Blur.Directional"), B_DIRECTIONAL);
	obs_property_list_add_int(p, T_("Blur.Zoom"), B_ZOOM);
	obs_property_set_modified_callback(p, blur_mode_modified);

	obs_properties_add_float_slider(props, "radius", T_("Blur.Radius"), 0.0, 200.0, 0.5);
	obs_properties_add_float_slider(props, "center_x", T_("Blur.CenterX"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, "center_y", T_("Blur.CenterY"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, "focus", T_("Blur.Focus"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, "softness", T_("Blur.Softness"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, "angle", T_("Blur.Angle"), -180.0, 180.0, 0.5);
	obs_properties_add_float_slider(props, "zoom", T_("Blur.ZoomAmount"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, "spin", T_("Blur.Spin"), -90.0, 90.0, 0.5);
	obs_properties_add_bool(props, "invert", T_("Blur.Invert"));
	return props;
}

static void *blurfx_create(obs_data_t *settings, obs_source_t *source)
{
	struct blurfx *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/blurfx.effect");
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	kg_blur_init(&f->blur);
	obs_leave_graphics();
	blurfx_update(f, settings);
	return f;
}

static void blurfx_destroy(void *data)
{
	struct blurfx *f = data;
	obs_enter_graphics();
	gs_texrender_destroy(f->input);
	kg_blur_free(&f->blur);
	obs_leave_graphics();
	bfree(f);
}

static void set_mask_params(struct blurfx *f, int shape, uint32_t cx, uint32_t cy)
{
	gs_effect_t *e = f->effect;
	struct vec2 c;
	vec2_set(&c, f->cx, f->cy);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "shape"), shape);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "center"), &c);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "focus"), f->focus);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "softness"), f->softness);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "aspect"), (float)cx / (float)cy);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "angle"), KG_DEG2RAD(f->angle));
	gs_effect_set_float(gs_effect_get_param_by_name(e, "invert"), f->invert ? 1.0f : 0.0f);
}

static void draw_with(gs_effect_t *e, const char *tech, gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(tex, 0, cx, cy);
	gs_blend_state_pop();
}

static void blurfx_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct blurfx *f = data;
	uint32_t cx, cy;
	if (!f->effect || !kg_filter_capture(f->context, f->input, &cx, &cy)) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	gs_texture_t *base = gs_texrender_get_texture(f->input);

	switch (f->mode) {
	case B_ZOOM:
		set_mask_params(f, 0, cx, cy);
		gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "zoom"), f->zoom * 0.5f);
		gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "spin"), KG_DEG2RAD(f->spin));
		draw_with(f->effect, "Zoom", base, cx, cy);
		return;
	case B_DIRECTIONAL: {
		struct vec2 dir;
		vec2_set(&dir, cosf(KG_DEG2RAD(f->angle)), sinf(KG_DEG2RAD(f->angle)));
		gs_texture_t *b = kg_blur_apply(&f->blur, base, cx, cy, f->radius, &dir);
		kg_draw_premultiplied(b, cx, cy);
		return;
	}
	case B_GAUSSIAN: {
		gs_texture_t *b = kg_blur_apply(&f->blur, base, cx, cy, f->radius, NULL);
		kg_draw_premultiplied(b, cx, cy);
		return;
	}
	default: {
		gs_texture_t *b = kg_blur_apply(&f->blur, base, cx, cy, f->radius, NULL);
		set_mask_params(f, f->mode == B_IRIS ? 1 : 2, cx, cy);
		gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "blurred"), b);
		draw_with(f->effect, "Mask", base, cx, cy);
		return;
	}
	}
}

struct obs_source_info kagee_blur_filter = {
	.id = "kagee_blur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = blurfx_name,
	.create = blurfx_create,
	.destroy = blurfx_destroy,
	.update = blurfx_update,
	.get_defaults = blurfx_defaults,
	.get_properties = blurfx_props,
	.video_render = blurfx_render,
};
