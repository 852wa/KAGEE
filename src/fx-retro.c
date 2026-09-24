/* Kagee Retro filter: VHS / CRT / 8mm film / video game / LED panel / halftone */
#include "common.h"

enum retro_mode { R_VHS = 0, R_CRT, R_FILM, R_GAME, R_LED, R_HALFTONE };

struct retro {
	obs_source_t *context;
	gs_effect_t *effect;
	int mode, palette;
	float mix, noise, scanline, jitter, curvature, fade, pixel_size, levels, dither;
	float time, frame_timer, frame_seed;
};

static const char *retro_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Retro.Name");
}

static void retro_update(void *data, obs_data_t *s)
{
	struct retro *f = data;
	f->mode = (int)obs_data_get_int(s, "mode");
	f->palette = (int)obs_data_get_int(s, "palette");
	f->mix = (float)obs_data_get_double(s, "mix") / 100.0f;
	f->noise = (float)obs_data_get_double(s, "noise") / 100.0f;
	f->scanline = (float)obs_data_get_double(s, "scanline") / 100.0f;
	f->jitter = (float)obs_data_get_double(s, "jitter") / 100.0f;
	f->curvature = (float)obs_data_get_double(s, "curvature") / 100.0f;
	f->fade = (float)obs_data_get_double(s, "fade") / 100.0f;
	f->pixel_size = (float)obs_data_get_double(s, "pixel_size");
	f->levels = (float)obs_data_get_int(s, "levels");
	f->dither = (float)obs_data_get_double(s, "dither") / 100.0f;
}

static void retro_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "mode", R_VHS);
	obs_data_set_default_int(s, "palette", 0);
	obs_data_set_default_double(s, "mix", 100.0);
	obs_data_set_default_double(s, "noise", 35.0);
	obs_data_set_default_double(s, "scanline", 40.0);
	obs_data_set_default_double(s, "jitter", 40.0);
	obs_data_set_default_double(s, "curvature", 50.0);
	obs_data_set_default_double(s, "fade", 30.0);
	obs_data_set_default_double(s, "pixel_size", 6.0);
	obs_data_set_default_int(s, "levels", 6);
	obs_data_set_default_double(s, "dither", 70.0);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	int m = (int)obs_data_get_int(s, "mode");
	bool analog = m == R_VHS || m == R_CRT || m == R_FILM;
	obs_property_set_visible(obs_properties_get(props, "noise"), analog);
	obs_property_set_visible(obs_properties_get(props, "scanline"), analog || m == R_LED);
	obs_property_set_visible(obs_properties_get(props, "jitter"), m == R_VHS || m == R_FILM);
	obs_property_set_visible(obs_properties_get(props, "curvature"), m == R_CRT);
	obs_property_set_visible(obs_properties_get(props, "fade"), m == R_VHS || m == R_FILM || m == R_HALFTONE);
	obs_property_set_visible(obs_properties_get(props, "pixel_size"), m == R_GAME || m == R_LED || m == R_HALFTONE);
	obs_property_set_visible(obs_properties_get(props, "levels"), m == R_GAME || m == R_LED);
	obs_property_set_visible(obs_properties_get(props, "palette"), m == R_GAME);
	obs_property_set_visible(obs_properties_get(props, "dither"), m == R_GAME);
	return true;
}

static obs_properties_t *retro_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "mode", T_("Retro.Mode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "VHS", R_VHS);
	obs_property_list_add_int(p, T_("Retro.CRT"), R_CRT);
	obs_property_list_add_int(p, T_("Retro.Film"), R_FILM);
	obs_property_list_add_int(p, T_("Retro.Game"), R_GAME);
	obs_property_list_add_int(p, T_("Retro.LED"), R_LED);
	obs_property_list_add_int(p, T_("Retro.Halftone"), R_HALFTONE);
	obs_property_set_modified_callback(p, mode_modified);

	obs_properties_add_float_slider(props, "mix", T_("Mix"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "noise", T_("Retro.Noise"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "scanline", T_("Retro.Scanline"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "jitter", T_("Retro.Jitter"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "curvature", T_("Retro.Curvature"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "fade", T_("Retro.Fade"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "pixel_size", T_("Retro.PixelSize"), 1.0, 64.0, 0.5);
	obs_properties_add_int_slider(props, "levels", T_("Retro.Levels"), 2, 32, 1);
	p = obs_properties_add_list(props, "palette", T_("Retro.Palette"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Retro.PaletteRGB"), 0);
	obs_property_list_add_int(p, T_("Retro.PaletteGB"), 1);
	obs_property_list_add_int(p, T_("Retro.Palette1bit"), 2);
	obs_property_list_add_int(p, T_("Retro.Palette4c"), 3);
	obs_properties_add_float_slider(props, "dither", T_("Retro.Dither"), 0.0, 100.0, 1.0);
	return props;
}

static void *retro_create(obs_data_t *settings, obs_source_t *source)
{
	struct retro *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/retro.effect");
	obs_leave_graphics();
	if (!f->effect) {
		bfree(f);
		return NULL;
	}
	retro_update(f, settings);
	return f;
}

static void retro_destroy(void *data)
{
	bfree(data);
}

static void retro_tick(void *data, float seconds)
{
	struct retro *f = data;
	f->time = fmodf(f->time + seconds, 3600.0f);
	/* film artefacts change at ~18 fps like a real projector */
	f->frame_timer += seconds;
	if (f->frame_timer >= 1.0f / 18.0f) {
		f->frame_timer = 0.0f;
		f->frame_seed = fmodf(f->frame_seed + 7.31f, 911.0f);
	}
}

static void retro_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct retro *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t cx = obs_source_get_base_width(target), cy = obs_source_get_base_height(target);
	if (!cx || !cy || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	gs_effect_t *e = f->effect;
	struct vec2 size;
	vec2_set(&size, (float)cx, (float)cy);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "mode"), f->mode);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "palette"), f->palette);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "size"), &size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "time"), f->time);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "frame_seed"), f->frame_seed);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "mix_amount"), f->mix);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "noise_amt"), f->noise);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "scanline"), f->scanline);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "jitter"), f->jitter);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "curvature"), f->curvature);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "fade"), f->fade);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "pixel_size"), f->pixel_size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "levels"), f->levels);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "dither"), f->dither);

	obs_source_process_filter_end(f->context, e, 0, 0);
}

struct obs_source_info kagee_retro_filter = {
	.id = "kagee_retro_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = retro_name,
	.create = retro_create,
	.destroy = retro_destroy,
	.update = retro_update,
	.get_defaults = retro_defaults,
	.get_properties = retro_props,
	.video_tick = retro_tick,
	.video_render = retro_render,
};
