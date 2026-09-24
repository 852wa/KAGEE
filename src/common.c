#include "common.h"

/* ------------------------------------------------------------------------- */
/* math / animation                                                          */

float kg_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

float kg_lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

float kg_ease(int type, float t)
{
	t = kg_clampf(t, 0.0f, 1.0f);
	switch (type) {
	case KG_EASE_CUT:
		return 1.0f;
	case KG_EASE_LINEAR:
		return t;
	case KG_EASE_SMOOTH:
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
	case KG_EASE_OUT:
		return t >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
	case KG_EASE_IN:
		return t * t * t;
	case KG_EASE_BACK: {
		const float c1 = 1.70158f, c3 = c1 + 1.0f;
		float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	case KG_EASE_ELASTIC: {
		if (t <= 0.0f || t >= 1.0f)
			return t;
		const float c4 = (2.0f * KG_PI) / 3.0f;
		return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
	}
	}
	return t;
}

static float hash1(int32_t n)
{
	uint32_t x = (uint32_t)n * 0x27d4eb2dU;
	x ^= x >> 15;
	x *= 0x85ebca6bU;
	x ^= x >> 13;
	x *= 0xc2b2ae35U;
	x ^= x >> 16;
	return (float)(x & 0xffffff) / (float)0xffffff * 2.0f - 1.0f;
}

float kg_noise1(float x)
{
	float fl = floorf(x);
	float f = x - fl;
	float u = f * f * (3.0f - 2.0f * f);
	int32_t i = (int32_t)fl;
	return kg_lerpf(hash1(i), hash1(i + 1), u);
}

float kg_fbm1(float x, int octaves)
{
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int i = 0; i < octaves; i++) {
		sum += kg_noise1(x) * amp;
		norm += amp;
		x = x * 2.03f + 17.17f;
		amp *= 0.5f;
	}
	return norm > 0.0f ? sum / norm : 0.0f;
}

float kg_rand01(uint32_t *state)
{
	uint32_t x = *state ? *state : 0x9e3779b9U;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (float)(x & 0xffffff) / (float)0x1000000;
}

/* ------------------------------------------------------------------------- */
/* graphics helpers                                                          */

gs_effect_t *kg_load_effect(const char *file)
{
	char *path = obs_module_file(file);
	if (!path) {
		blog_kg(LOG_ERROR, "effect file not found: %s", file);
		return NULL;
	}
	char *errors = NULL;
	gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
	if (!effect)
		blog_kg(LOG_ERROR, "failed to compile %s:\n%s", file, errors ? errors : "(no log)");
	bfree(errors);
	bfree(path);
	return effect;
}

void kg_add_easing_list(obs_property_t *p)
{
	obs_property_list_add_int(p, T_("Ease.Cut"), KG_EASE_CUT);
	obs_property_list_add_int(p, T_("Ease.Linear"), KG_EASE_LINEAR);
	obs_property_list_add_int(p, T_("Ease.Smooth"), KG_EASE_SMOOTH);
	obs_property_list_add_int(p, T_("Ease.Out"), KG_EASE_OUT);
	obs_property_list_add_int(p, T_("Ease.In"), KG_EASE_IN);
	obs_property_list_add_int(p, T_("Ease.Back"), KG_EASE_BACK);
	obs_property_list_add_int(p, T_("Ease.Elastic"), KG_EASE_ELASTIC);
}

bool kg_filter_capture(obs_source_t *filter, gs_texrender_t *tr, uint32_t *out_cx, uint32_t *out_cy)
{
	obs_source_t *target = obs_filter_get_target(filter);
	obs_source_t *parent = obs_filter_get_parent(filter);
	if (!target || !parent)
		return false;

	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);
	if (!cx || !cy)
		return false;

	uint32_t flags = obs_source_get_output_flags(target);
	bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (flags & OBS_SOURCE_ASYNC) != 0;

	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, cx, cy))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	*out_cx = cx;
	*out_cy = cy;
	return true;
}

void kg_draw_premultiplied(gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, cx, cy);
	gs_blend_state_pop();
}

gs_texture_t *kg_pass(gs_texrender_t *tr, gs_effect_t *effect, const char *tech, gs_texture_t *tex, uint32_t cx,
		      uint32_t cy)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, cx, cy))
		return NULL;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_enable_blending(false);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (image && tex)
		gs_effect_set_texture(image, tex);
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(tex, 0, cx, cy);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	return gs_texrender_get_texture(tr);
}

/* ------------------------------------------------------------------------- */
/* blur                                                                      */

#define BLUR_TAPS 16 /* per side, must match blur.effect */

void kg_blur_init(struct kg_blur *b)
{
	memset(b, 0, sizeof(*b));
	b->effect = kg_load_effect("effects/blur.effect");
	b->down = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	b->a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	b->b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
}

void kg_blur_free(struct kg_blur *b)
{
	gs_texrender_destroy(b->down);
	gs_texrender_destroy(b->a);
	gs_texrender_destroy(b->b);
	/* effects created from file are cached by libobs; don't destroy */
	memset(b, 0, sizeof(*b));
}

static gs_texture_t *blur_1d(struct kg_blur *b, gs_texrender_t *tr, gs_texture_t *src, uint32_t cx, uint32_t cy,
			     float sigma_px, float dx, float dy)
{
	/* spread BLUR_TAPS samples over ~3 sigma */
	float spacing = fmaxf(1.0f, (3.0f * sigma_px) / (float)BLUR_TAPS);
	struct vec2 step;
	vec2_set(&step, dx * spacing / (float)cx, dy * spacing / (float)cy);
	gs_effect_set_vec2(gs_effect_get_param_by_name(b->effect, "tap_step"), &step);
	gs_effect_set_float(gs_effect_get_param_by_name(b->effect, "sigma"), fmaxf(sigma_px / spacing, 0.01f));
	return kg_pass(tr, b->effect, "Blur", src, cx, cy);
}

/* One blur axis, ping-ponging between b->a and b->b (slot = next buffer to write).
 * When the taps would be spread far apart (large sigma), pre-smooth along the same axis first
 * so the taps overlap; otherwise sharp highlights turn into a row of ghost copies. */
static gs_texture_t *blur_axis(struct kg_blur *b, gs_texture_t *src, uint32_t w, uint32_t h, float sigma, float dx,
			       float dy, int *slot)
{
	float spacing = fmaxf(1.0f, (3.0f * sigma) / (float)BLUR_TAPS);
	if (spacing > 1.5f) {
		float pre = spacing * 0.75f;
		src = blur_1d(b, *slot ? b->b : b->a, src, w, h, pre, dx, dy);
		if (!src)
			return NULL;
		*slot ^= 1;
		sigma = sqrtf(fmaxf(sigma * sigma - pre * pre, 0.25f));
	}
	gs_texture_t *t = blur_1d(b, *slot ? b->b : b->a, src, w, h, sigma, dx, dy);
	*slot ^= 1;
	return t;
}

gs_texture_t *kg_blur_apply(struct kg_blur *b, gs_texture_t *src, uint32_t cx, uint32_t cy, float radius,
			    const struct vec2 *dir)
{
	b->out_cx = cx;
	b->out_cy = cy;
	if (!b->effect || !src || radius < 0.5f || !cx || !cy)
		return src;

	/* downsample for large radii: keeps tap count constant and cost low */
	uint32_t ds = radius > 96.0f ? 8 : radius > 40.0f ? 4 : radius > 16.0f ? 2 : 1;
	uint32_t w = cx / ds, h = cy / ds;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;

	gs_texture_t *cur = src;
	if (ds > 1) {
		struct vec2 texel;
		vec2_set(&texel, 1.0f / (float)cx, 1.0f / (float)cy);
		gs_effect_set_vec2(gs_effect_get_param_by_name(b->effect, "src_texel"), &texel);
		gs_effect_set_float(gs_effect_get_param_by_name(b->effect, "down_scale"), (float)ds * 0.5f);
		cur = kg_pass(b->down, b->effect, "Down", src, w, h);
		if (!cur)
			return src;
	}

	float sigma = (radius / (float)ds) * 0.5f;
	int slot = 0;
	if (dir) {
		cur = blur_axis(b, cur, w, h, sigma, dir->x, dir->y, &slot);
	} else {
		gs_texture_t *t = blur_axis(b, cur, w, h, sigma, 1.0f, 0.0f, &slot);
		cur = t ? blur_axis(b, t, w, h, sigma, 0.0f, 1.0f, &slot) : NULL;
	}
	if (!cur)
		return src;
	b->out_cx = w;
	b->out_cy = h;
	return cur;
}
