/* Kagee Glitch filter: RGB split / block displacement / line tear / turbulent displace,
 * with a constant base level plus hotkey- or BPM-triggered bursts. */
#include "common.h"

struct glitch {
	obs_source_t *context;
	gs_effect_t *effect;

	float base, burst_gain, burst_dur, rate;
	float rgb_split, block, block_size, tear, warp, warp_scale, warp_speed, color, scan;
	bool bpm_on;
	float bpm;
	int bpm_every;

	float time, seed_timer, seed, burst;
	long long last_beat;
	volatile long trigger;
	obs_hotkey_id hotkey;
};

static const char *glitch_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Glitch.Name");
}

static void glitch_update(void *data, obs_data_t *s)
{
	struct glitch *f = data;
	f->base = (float)obs_data_get_double(s, "base") / 100.0f;
	f->burst_gain = (float)obs_data_get_double(s, "burst_gain") / 100.0f;
	f->burst_dur = (float)obs_data_get_double(s, "burst_dur");
	f->rate = (float)obs_data_get_double(s, "rate");
	f->rgb_split = (float)obs_data_get_double(s, "rgb_split") / 100.0f;
	f->block = (float)obs_data_get_double(s, "block") / 100.0f;
	f->block_size = (float)obs_data_get_double(s, "block_size");
	f->tear = (float)obs_data_get_double(s, "tear") / 100.0f;
	f->warp = (float)obs_data_get_double(s, "warp") / 100.0f;
	f->warp_scale = (float)obs_data_get_double(s, "warp_scale");
	f->warp_speed = (float)obs_data_get_double(s, "warp_speed");
	f->color = (float)obs_data_get_double(s, "color") / 100.0f;
	f->scan = (float)obs_data_get_double(s, "scan") / 100.0f;
	f->bpm_on = obs_data_get_bool(s, "bpm_on");
	f->bpm = (float)obs_data_get_double(s, "bpm");
	f->bpm_every = (int)obs_data_get_int(s, "bpm_every");
}

static void glitch_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "base", 10.0);
	obs_data_set_default_double(s, "burst_gain", 100.0);
	obs_data_set_default_double(s, "burst_dur", 0.4);
	obs_data_set_default_double(s, "rate", 15.0);
	obs_data_set_default_double(s, "rgb_split", 50.0);
	obs_data_set_default_double(s, "block", 50.0);
	obs_data_set_default_double(s, "block_size", 24.0);
	obs_data_set_default_double(s, "tear", 40.0);
	obs_data_set_default_double(s, "warp", 0.0);
	obs_data_set_default_double(s, "warp_scale", 3.0);
	obs_data_set_default_double(s, "warp_speed", 0.3);
	obs_data_set_default_double(s, "color", 30.0);
	obs_data_set_default_double(s, "scan", 20.0);
	obs_data_set_default_bool(s, "bpm_on", false);
	obs_data_set_default_double(s, "bpm", 120.0);
	obs_data_set_default_int(s, "bpm_every", 4);
}

static bool btn_burst(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct glitch *f = data;
	os_atomic_set_long(&f->trigger, 1);
	return false;
}

static obs_properties_t *glitch_props(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "base", T_("Glitch.Base"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "rate", T_("Glitch.Rate"), 1.0, 60.0, 1.0);
	obs_properties_add_float_slider(props, "rgb_split", T_("Glitch.RGBSplit"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "block", T_("Glitch.Block"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "block_size", T_("Glitch.BlockSize"), 2.0, 200.0, 1.0);
	obs_properties_add_float_slider(props, "tear", T_("Glitch.Tear"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "color", T_("Glitch.Color"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "scan", T_("Glitch.Scan"), 0.0, 100.0, 1.0);

	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "warp", T_("Glitch.Warp"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "warp_scale", T_("Glitch.WarpScale"), 0.5, 20.0, 0.1);
	obs_properties_add_float_slider(g, "warp_speed", T_("Glitch.WarpSpeed"), 0.0, 5.0, 0.05);
	obs_properties_add_group(props, "grp_warp", T_("Glitch.WarpGroup"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_float_slider(g, "burst_gain", T_("Glitch.BurstGain"), 0.0, 300.0, 1.0);
	obs_properties_add_float_slider(g, "burst_dur", T_("Glitch.BurstDuration"), 0.05, 3.0, 0.05);
	obs_properties_add_button2(g, "btn_burst", T_("Glitch.BurstTest"), btn_burst, data);
	obs_properties_add_bool(g, "bpm_on", T_("Glitch.BpmOn"));
	obs_properties_add_float_slider(g, "bpm", T_("Beat.BPM"), 30.0, 300.0, 0.1);
	obs_properties_add_int_slider(g, "bpm_every", T_("Auto.EveryBeats"), 1, 32, 1);
	obs_properties_add_group(props, "grp_burst", T_("Glitch.BurstGroup"), OBS_GROUP_NORMAL, g);
	return props;
}

static void glitch_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct glitch *f = data;
	if (pressed)
		os_atomic_set_long(&f->trigger, 1);
}

static void *glitch_create(obs_data_t *settings, obs_source_t *source)
{
	struct glitch *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/glitch.effect");
	obs_leave_graphics();
	if (!f->effect) {
		bfree(f);
		return NULL;
	}
	glitch_update(f, settings);
	f->hotkey = obs_hotkey_register_source(source, "Kagee.GlitchBurst", T_("Hotkey.GlitchBurst"),
					       glitch_hotkey, f);
	return f;
}

static void glitch_destroy(void *data)
{
	bfree(data);
}

static void glitch_tick(void *data, float seconds)
{
	struct glitch *f = data;
	f->time += seconds;
	if (f->time > 3600.0f)
		f->time -= 3600.0f;

	f->seed_timer += seconds;
	if (f->seed_timer >= 1.0f / fmaxf(f->rate, 1.0f)) {
		f->seed_timer = 0.0f;
		f->seed = fmodf(f->seed + 13.37f, 997.0f);
	}

	if (os_atomic_set_long(&f->trigger, 0))
		f->burst = 1.0f;

	if (f->bpm_on && f->bpm > 0.0f) {
		long long beat = (long long)floorf(f->time * f->bpm / 60.0f);
		if (beat != f->last_beat && f->bpm_every > 0 && beat % f->bpm_every == 0)
			f->burst = 1.0f;
		f->last_beat = beat;
	}

	if (f->burst > 0.0f)
		f->burst = fmaxf(0.0f, f->burst - seconds / fmaxf(f->burst_dur, 0.01f));
}

static void glitch_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct glitch *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t cx = obs_source_get_base_width(target), cy = obs_source_get_base_height(target);
	float amount = f->base + f->burst * f->burst * f->burst_gain;
	if (!cx || !cy || (amount <= 0.0f && f->warp <= 0.0f && f->scan <= 0.0f) ||
	    !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	gs_effect_t *e = f->effect;
	struct vec2 size;
	vec2_set(&size, (float)cx, (float)cy);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "size"), &size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "time"), f->time);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "seed"), f->seed);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "amount"), amount);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "rgb_split"), f->rgb_split);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "block_amt"), f->block);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "block_size"), f->block_size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "tear"), f->tear);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "warp"), f->warp);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "warp_scale"), f->warp_scale);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "warp_speed"), f->warp_speed);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "color_amt"), f->color);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "scan_noise"), f->scan);

	obs_source_process_filter_end(f->context, e, 0, 0);
}

struct obs_source_info kagee_glitch_filter = {
	.id = "kagee_glitch_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = glitch_name,
	.create = glitch_create,
	.destroy = glitch_destroy,
	.update = glitch_update,
	.get_defaults = glitch_defaults,
	.get_properties = glitch_props,
	.video_tick = glitch_tick,
	.video_render = glitch_render,
};
