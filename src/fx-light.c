/* Lighting filter: ambient darkness plus up to 4 animated lights (spot / beam / rim). */
#include "common.h"

#define MAX_LIGHTS 4

enum light_type { LT_SPOT = 0, LT_BEAM, LT_RIM };
enum light_anim { LA_NONE = 0, LA_SWEEP, LA_PULSE, LA_FLICKER, LA_BEAT, LA_ORBIT };

struct light {
	bool on;
	int type, anim;
	struct vec4 color;
	float intensity, x, y, radius, soft, angle, width;
	float speed, anim_amt, phase;
};

struct lightfx {
	obs_source_t *context;
	gs_effect_t *effect;
	struct light lights[MAX_LIGHTS];
	struct vec3 ambient;
	bool haze_alpha;
	float bpm;
	float time;
};

static const char *light_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Light.Name");
}

static void light_update(void *data, obs_data_t *s)
{
	struct lightfx *f = data;
	struct vec4 amb;
	vec4_from_rgba(&amb, (uint32_t)obs_data_get_int(s, "ambient_color"));
	float level = (float)obs_data_get_double(s, "ambient") / 100.0f;
	vec3_set(&f->ambient, amb.x * level, amb.y * level, amb.z * level);
	f->haze_alpha = obs_data_get_bool(s, "haze_alpha");
	f->bpm = (float)obs_data_get_double(s, "bpm");

	char key[32];
	for (int i = 0; i < MAX_LIGHTS; i++) {
		struct light *l = &f->lights[i];
		int n = i + 1;
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)
		l->on = obs_data_get_bool(s, K("L%d_on"));
		l->type = (int)obs_data_get_int(s, K("L%d_type"));
		l->anim = (int)obs_data_get_int(s, K("L%d_anim"));
		vec4_from_rgba(&l->color, (uint32_t)obs_data_get_int(s, K("L%d_color")));
		l->intensity = (float)obs_data_get_double(s, K("L%d_int")) / 100.0f;
		l->x = (float)obs_data_get_double(s, K("L%d_x")) / 100.0f;
		l->y = (float)obs_data_get_double(s, K("L%d_y")) / 100.0f;
		l->radius = (float)obs_data_get_double(s, K("L%d_radius")) / 100.0f;
		l->soft = (float)obs_data_get_double(s, K("L%d_soft")) / 100.0f;
		l->angle = (float)obs_data_get_double(s, K("L%d_angle"));
		l->width = (float)obs_data_get_double(s, K("L%d_width"));
		l->speed = (float)obs_data_get_double(s, K("L%d_speed"));
		l->anim_amt = (float)obs_data_get_double(s, K("L%d_anim_amt")) / 100.0f;
		l->phase = (float)i * 1.7f;
#undef K
	}
}

static void light_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "ambient", 55.0);
	obs_data_set_default_int(s, "ambient_color", 0xFFFFE8E0); /* ABGR: slightly cool */
	obs_data_set_default_bool(s, "haze_alpha", true);
	obs_data_set_default_double(s, "bpm", 120.0);

	/* a ready-to-use rig: key spot, two crossing beams, rim light */
	static const struct {
		bool on;
		int type;
		uint32_t color;
		double x, y, radius, angle, width, intensity;
		int anim;
	} rig[MAX_LIGHTS] = {
		{true, LT_SPOT, 0xFFD8ECFF, 50, 45, 45, 0, 30, 90, LA_NONE},
		{true, LT_BEAM, 0xFFFF9050, 20, -5, 120, 25, 18, 70, LA_SWEEP},
		{true, LT_BEAM, 0xFFC060FF, 80, -5, 120, -25, 18, 70, LA_SWEEP},
		{false, LT_RIM, 0xFFFFF0D0, 50, 0, 50, 45, 6, 120, LA_NONE},
	};
	char key[32];
	for (int i = 0; i < MAX_LIGHTS; i++) {
		int n = i + 1;
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)
		obs_data_set_default_bool(s, K("L%d_on"), rig[i].on);
		obs_data_set_default_int(s, K("L%d_type"), rig[i].type);
		obs_data_set_default_int(s, K("L%d_color"), rig[i].color);
		obs_data_set_default_double(s, K("L%d_int"), rig[i].intensity);
		obs_data_set_default_double(s, K("L%d_x"), rig[i].x);
		obs_data_set_default_double(s, K("L%d_y"), rig[i].y);
		obs_data_set_default_double(s, K("L%d_radius"), rig[i].radius);
		obs_data_set_default_double(s, K("L%d_soft"), 70.0);
		obs_data_set_default_double(s, K("L%d_angle"), rig[i].angle);
		obs_data_set_default_double(s, K("L%d_width"), rig[i].width);
		obs_data_set_default_int(s, K("L%d_anim"), rig[i].anim);
		obs_data_set_default_double(s, K("L%d_speed"), 0.4);
		obs_data_set_default_double(s, K("L%d_anim_amt"), 50.0);
#undef K
	}
}

static bool type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	char key[32];
	for (int n = 1; n <= MAX_LIGHTS; n++) {
		snprintf(key, sizeof(key), "L%d_type", n);
		int t = (int)obs_data_get_int(s, key);
#define VIS(fmt, cond) (snprintf(key, sizeof(key), fmt, n), obs_property_set_visible(obs_properties_get(props, key), cond))
		VIS("L%d_x", t != LT_RIM);
		VIS("L%d_y", t != LT_RIM);
		VIS("L%d_radius", t != LT_RIM);
		VIS("L%d_soft", t != LT_RIM);
		VIS("L%d_angle", t != LT_SPOT);
		VIS("L%d_width", t != LT_SPOT);
#undef VIS
	}
	return true;
}

static obs_properties_t *light_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "ambient", T_("Light.Ambient"), 0.0, 150.0, 1.0);
	obs_properties_add_color(props, "ambient_color", T_("Light.AmbientColor"));
	obs_properties_add_bool(props, "haze_alpha", T_("Light.HazeAlpha"));
	obs_properties_add_float_slider(props, "bpm", T_("Beat.BPM"), 30.0, 300.0, 0.1);

	char key[32], title[64];
	for (int n = 1; n <= MAX_LIGHTS; n++) {
		obs_properties_t *g = obs_properties_create();
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)
		obs_property_t *p = obs_properties_add_list(g, K("L%d_type"), T_("Light.Type"), OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, T_("Light.Spot"), LT_SPOT);
		obs_property_list_add_int(p, T_("Light.Beam"), LT_BEAM);
		obs_property_list_add_int(p, T_("Light.Rim"), LT_RIM);
		obs_property_set_modified_callback(p, type_modified);
		obs_properties_add_color(g, K("L%d_color"), T_("Light.Color"));
		obs_properties_add_float_slider(g, K("L%d_int"), T_("Light.Intensity"), 0.0, 300.0, 1.0);
		obs_properties_add_float_slider(g, K("L%d_x"), T_("Light.X"), -50.0, 150.0, 0.5);
		obs_properties_add_float_slider(g, K("L%d_y"), T_("Light.Y"), -50.0, 150.0, 0.5);
		obs_properties_add_float_slider(g, K("L%d_radius"), T_("Light.Radius"), 1.0, 300.0, 0.5);
		obs_properties_add_float_slider(g, K("L%d_soft"), T_("Light.Softness"), 0.0, 100.0, 1.0);
		obs_properties_add_float_slider(g, K("L%d_angle"), T_("Light.Angle"), -180.0, 180.0, 0.5);
		obs_properties_add_float_slider(g, K("L%d_width"), T_("Light.Width"), 1.0, 120.0, 0.5);
		p = obs_properties_add_list(g, K("L%d_anim"), T_("Light.Anim"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, T_("Light.AnimNone"), LA_NONE);
		obs_property_list_add_int(p, T_("Light.AnimSweep"), LA_SWEEP);
		obs_property_list_add_int(p, T_("Light.AnimOrbit"), LA_ORBIT);
		obs_property_list_add_int(p, T_("Light.AnimPulse"), LA_PULSE);
		obs_property_list_add_int(p, T_("Light.AnimFlicker"), LA_FLICKER);
		obs_property_list_add_int(p, T_("Light.AnimBeat"), LA_BEAT);
		obs_properties_add_float_slider(g, K("L%d_speed"), T_("Light.Speed"), 0.0, 5.0, 0.05);
		obs_properties_add_float_slider(g, K("L%d_anim_amt"), T_("Light.AnimAmount"), 0.0, 100.0, 1.0);
		snprintf(title, sizeof(title), "%s %d", T_("Light.Light"), n);
		obs_properties_add_group(props, K("L%d_on"), title, OBS_GROUP_CHECKABLE, g);
#undef K
	}
	return props;
}

static void *light_create(obs_data_t *settings, obs_source_t *source)
{
	struct lightfx *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/light.effect");
	obs_leave_graphics();
	if (!f->effect) {
		bfree(f);
		return NULL;
	}
	light_update(f, settings);
	return f;
}

static void light_destroy(void *data)
{
	bfree(data);
}

static void light_tick(void *data, float seconds)
{
	struct lightfx *f = data;
	f->time += seconds;
	if (f->time > 36000.0f)
		f->time -= 36000.0f;
}

/* animated parameters for one light at the current time */
static void animate(const struct lightfx *f, const struct light *l, float *x, float *y, float *angle, float *intensity)
{
	float t = f->time * l->speed + l->phase;
	float amt = l->anim_amt;
	*x = l->x;
	*y = l->y;
	*angle = l->angle;
	*intensity = l->intensity;
	switch (l->anim) {
	case LA_SWEEP:
		if (l->type == LT_SPOT)
			*x += sinf(t * 2.0f * KG_PI) * 0.4f * amt;
		else
			*angle += sinf(t * 2.0f * KG_PI) * 40.0f * amt;
		break;
	case LA_ORBIT:
		*x += cosf(t * 2.0f * KG_PI) * 0.3f * amt;
		*y += sinf(t * 2.0f * KG_PI) * 0.2f * amt;
		*angle += t * 360.0f * amt;
		break;
	case LA_PULSE:
		*intensity *= 1.0f - amt + amt * (0.5f + 0.5f * sinf(t * 2.0f * KG_PI));
		break;
	case LA_FLICKER:
		*intensity *= 1.0f - amt * (0.5f + 0.5f * kg_noise1(f->time * (4.0f + l->speed * 20.0f) + l->phase * 10.0f));
		break;
	case LA_BEAT: {
		float beat = 60.0f / kg_clampf(f->bpm, 1.0f, 999.0f);
		float since = fmodf(f->time + l->phase * 0.0f, beat);
		*intensity *= 1.0f - amt + amt * expf(-since * 6.0f / beat);
		break;
	}
	default:
		break;
	}
}

static void light_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct lightfx *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t cx = obs_source_get_base_width(target), cy = obs_source_get_base_height(target);
	if (!cx || !cy || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	gs_effect_t *e = f->effect;
	struct vec2 size;
	vec2_set(&size, (float)cx, (float)cy);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "size"), &size);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "time"), f->time);
	gs_effect_set_vec3(gs_effect_get_param_by_name(e, "ambient"), &f->ambient);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "haze_alpha"), f->haze_alpha ? 1.0f : 0.0f);

	char name[8];
	for (int i = 0; i < MAX_LIGHTS; i++) {
		const struct light *l = &f->lights[i];
		float x, y, angle, intensity;
		animate(f, l, &x, &y, &angle, &intensity);
		struct vec4 a, b, c;
		vec4_set(&a, x, y, l->radius, l->soft);
		vec4_set(&b, l->color.x, l->color.y, l->color.z, intensity);
		float width = l->type == LT_BEAM ? KG_DEG2RAD(l->width) : l->width;
		vec4_set(&c, (float)l->type, KG_DEG2RAD(angle), width, l->on ? 1.0f : 0.0f);
		snprintf(name, sizeof(name), "l%da", i);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &a);
		snprintf(name, sizeof(name), "l%db", i);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &b);
		snprintf(name, sizeof(name), "l%dc", i);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &c);
	}
	obs_source_process_filter_end(f->context, e, 0, 0);
}

struct obs_source_info kagee_light_filter = {
	.id = "kagee_light_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = light_name,
	.create = light_create,
	.destroy = light_destroy,
	.update = light_update,
	.get_defaults = light_defaults,
	.get_properties = light_props,
	.video_tick = light_tick,
	.video_render = light_render,
};
