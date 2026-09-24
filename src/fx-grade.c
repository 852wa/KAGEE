/*
 * Color Grade filter.
 *
 * All grading (preset look x strength, manual correction, optional .cube LUT x strength) is
 * evaluated on the CPU into a single 33^3 3D LUT whenever settings change; the GPU only does
 * one trilinear LUT lookup per pixel. The combined grade can be exported as a .cube file.
 */
#include "common.h"
#include <util/dstr.h>
#include <util/platform.h>
#include <stdio.h>
#include <ctype.h>

#define LUT_N 33

struct grade {
	float exposure; /* stops */
	float temp, tint;
	float contrast, pivot;
	float sat, vib, hue; /* hue in degrees */
	float lift, gamma, gain;
	float sh_col[3], sh_amt;
	float mid_col[3], mid_amt;
	float hi_col[3], hi_amt;
	float fade, bleach, mono;
	float mono_col[3];
};

static void grade_identity(struct grade *g)
{
	memset(g, 0, sizeof(*g));
	g->contrast = 1.0f;
	g->pivot = 0.45f;
	g->sat = 1.0f;
	g->gamma = 1.0f;
	g->gain = 1.0f;
	g->mono_col[0] = g->mono_col[1] = g->mono_col[2] = 1.0f;
}

/* ------------------------------------------------------------------------- */
/* presets                                                                   */

struct preset {
	const char *key;
	void (*make)(struct grade *g);
};

static void set3(float *d, float r, float g, float b)
{
	d[0] = r;
	d[1] = g;
	d[2] = b;
}

static void p_teal_orange(struct grade *g)
{
	set3(g->sh_col, 0.0f, 0.55f, 0.65f);
	g->sh_amt = 0.45f;
	set3(g->hi_col, 1.0f, 0.62f, 0.3f);
	g->hi_amt = 0.35f;
	g->contrast = 1.15f;
	g->sat = 1.1f;
}
static void p_warm(struct grade *g)
{
	g->temp = 0.35f;
	g->contrast = 1.1f;
	g->fade = 0.03f;
	set3(g->hi_col, 1.0f, 0.8f, 0.5f);
	g->hi_amt = 0.2f;
	g->sat = 1.05f;
}
static void p_cool(struct grade *g)
{
	g->temp = -0.35f;
	set3(g->sh_col, 0.1f, 0.25f, 0.6f);
	g->sh_amt = 0.3f;
	g->contrast = 1.12f;
	g->sat = 0.9f;
}
static void p_vivid(struct grade *g)
{
	g->sat = 1.3f;
	g->vib = 0.35f;
	g->contrast = 1.15f;
	g->exposure = 0.1f;
}
static void p_vintage(struct grade *g)
{
	g->fade = 0.12f;
	g->sat = 0.78f;
	set3(g->sh_col, 0.2f, 0.35f, 0.45f);
	g->sh_amt = 0.3f;
	set3(g->hi_col, 1.0f, 0.85f, 0.6f);
	g->hi_amt = 0.35f;
	g->contrast = 0.92f;
	g->temp = 0.1f;
}
static void p_matte(struct grade *g)
{
	g->fade = 0.2f;
	g->contrast = 0.85f;
	g->sat = 0.88f;
	g->gain = 0.96f;
}
static void p_bleach(struct grade *g)
{
	g->bleach = 0.7f;
	g->sat = 0.55f;
	g->contrast = 1.2f;
}
static void p_mono(struct grade *g)
{
	g->mono = 1.0f;
	g->contrast = 1.2f;
}
static void p_sepia(struct grade *g)
{
	g->mono = 1.0f;
	set3(g->mono_col, 1.0f, 0.82f, 0.62f);
	g->fade = 0.05f;
	g->contrast = 1.05f;
}
static void p_neon(struct grade *g)
{
	g->temp = -0.2f;
	g->tint = 0.25f;
	set3(g->sh_col, 0.15f, 0.0f, 0.7f);
	g->sh_amt = 0.4f;
	set3(g->hi_col, 1.0f, 0.25f, 0.8f);
	g->hi_amt = 0.3f;
	g->sat = 1.25f;
	g->contrast = 1.2f;
}

static const struct preset presets[] = {
	{"Grade.P.TealOrange", p_teal_orange}, {"Grade.P.Warm", p_warm},     {"Grade.P.Cool", p_cool},
	{"Grade.P.Vivid", p_vivid},            {"Grade.P.Vintage", p_vintage}, {"Grade.P.Matte", p_matte},
	{"Grade.P.Bleach", p_bleach},          {"Grade.P.Mono", p_mono},       {"Grade.P.Sepia", p_sepia},
	{"Grade.P.Neon", p_neon},
};
#define PRESET_COUNT ((int)(sizeof(presets) / sizeof(presets[0])))

/* ------------------------------------------------------------------------- */
/* CPU grading pipeline (values are display-encoded sRGB, 0..1)              */

static float to_lin(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float to_srgb(float c)
{
	c = fmaxf(c, 0.0f);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float luma(const float *c)
{
	return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
}

static void apply_tint(float *c, const float *col, float amt, float weight)
{
	if (amt <= 0.0f || weight <= 0.0f)
		return;
	float l = luma(col);
	for (int i = 0; i < 3; i++)
		c[i] += (col[i] - l) * amt * weight * 0.5f;
}

static void grade_apply(const struct grade *g, float *c)
{
	/* exposure + white balance in linear light */
	float k = powf(2.0f, g->exposure);
	float wb[3] = {1.0f + 0.25f * g->temp, 1.0f - 0.25f * g->tint, 1.0f - 0.25f * g->temp};
	for (int i = 0; i < 3; i++)
		c[i] = to_srgb(to_lin(c[i]) * k * wb[i]);

	/* lift / gamma / gain */
	for (int i = 0; i < 3; i++) {
		float v = c[i] * g->gain + g->lift * (1.0f - c[i]);
		c[i] = powf(kg_clampf(v, 0.0f, 4.0f), 1.0f / fmaxf(g->gamma, 0.05f));
	}

	/* contrast around pivot, with a soft toe/shoulder */
	for (int i = 0; i < 3; i++)
		c[i] = (c[i] - g->pivot) * g->contrast + g->pivot;

	/* split toning by luminance */
	float l = kg_clampf(luma(c), 0.0f, 1.0f);
	apply_tint(c, g->sh_col, g->sh_amt, (1.0f - l) * (1.0f - l));
	apply_tint(c, g->mid_col, g->mid_amt, 1.0f - fabsf(l - 0.5f) * 2.0f);
	apply_tint(c, g->hi_col, g->hi_amt, l * l);

	/* hue rotation (YIQ) */
	if (g->hue != 0.0f) {
		float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
		float ii = 0.596f * c[0] - 0.274f * c[1] - 0.322f * c[2];
		float q = 0.211f * c[0] - 0.523f * c[1] + 0.312f * c[2];
		float a = KG_DEG2RAD(g->hue), ca = cosf(a), sa = sinf(a);
		float i2 = ii * ca - q * sa, q2 = ii * sa + q * ca;
		c[0] = y + 0.956f * i2 + 0.621f * q2;
		c[1] = y - 0.272f * i2 - 0.647f * q2;
		c[2] = y - 1.106f * i2 + 1.703f * q2;
	}

	/* saturation + vibrance (boosts muted colours more) */
	l = luma(c);
	float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
	float chroma = mx - mn;
	float s = g->sat * (1.0f + g->vib * (1.0f - kg_clampf(chroma * 2.0f, 0.0f, 1.0f)));
	for (int i = 0; i < 3; i++)
		c[i] = l + (c[i] - l) * s;

	/* bleach bypass: overlay the luminance layer */
	if (g->bleach > 0.0f) {
		l = kg_clampf(luma(c), 0.0f, 1.0f);
		for (int i = 0; i < 3; i++) {
			float b = kg_clampf(c[i], 0.0f, 1.0f);
			float ov = l < 0.5f ? 2.0f * b * l : 1.0f - 2.0f * (1.0f - b) * (1.0f - l);
			c[i] = kg_lerpf(c[i], ov, g->bleach);
		}
	}

	/* monochrome (optionally toned) */
	if (g->mono > 0.0f) {
		l = luma(c);
		for (int i = 0; i < 3; i++)
			c[i] = kg_lerpf(c[i], l * g->mono_col[i], g->mono);
	}

	/* fade: lifted blacks, slightly lowered whites */
	for (int i = 0; i < 3; i++)
		c[i] = kg_clampf(c[i] * (1.0f - g->fade * 1.3f) + g->fade, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------------- */
/* .cube LUT                                                                 */

struct cube {
	int n;
	float *data; /* n^3 * 3, red fastest */
	float dmin[3], dmax[3];
};

static void cube_free(struct cube *c)
{
	bfree(c->data);
	memset(c, 0, sizeof(*c));
}

static bool cube_load(struct cube *c, const char *path)
{
	cube_free(c);
	char *text = os_quick_read_utf8_file(path);
	if (!text)
		return false;
	c->dmax[0] = c->dmax[1] = c->dmax[2] = 1.0f;
	size_t count = 0, cap = 0;
	char *save = NULL;
	for (char *line = strtok_s(text, "\r\n", &save); line; line = strtok_s(NULL, "\r\n", &save)) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		if (strncmp(line, "LUT_3D_SIZE", 11) == 0) {
			c->n = atoi(line + 11);
			if (c->n < 2 || c->n > 256)
				break;
			cap = (size_t)c->n * c->n * c->n;
			c->data = bmalloc(cap * 3 * sizeof(float));
		} else if (strncmp(line, "DOMAIN_MIN", 10) == 0) {
			sscanf(line + 10, "%f %f %f", &c->dmin[0], &c->dmin[1], &c->dmin[2]);
		} else if (strncmp(line, "DOMAIN_MAX", 10) == 0) {
			sscanf(line + 10, "%f %f %f", &c->dmax[0], &c->dmax[1], &c->dmax[2]);
		} else if (isdigit((unsigned char)*line) || *line == '-' || *line == '.') {
			float r, g, b;
			if (c->data && count < cap && sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
				c->data[count * 3 + 0] = r;
				c->data[count * 3 + 1] = g;
				c->data[count * 3 + 2] = b;
				count++;
			}
		}
	}
	bfree(text);
	if (!c->data || count != cap) {
		blog_kg(LOG_WARNING, "invalid or unsupported .cube file (1D LUTs are not supported): %s", path);
		cube_free(c);
		return false;
	}
	return true;
}

static void cube_sample(const struct cube *c, float *rgb)
{
	float p[3];
	int i0[3], i1[3];
	float f[3];
	for (int k = 0; k < 3; k++) {
		float t = (rgb[k] - c->dmin[k]) / fmaxf(c->dmax[k] - c->dmin[k], 1e-6f);
		p[k] = kg_clampf(t, 0.0f, 1.0f) * (float)(c->n - 1);
		i0[k] = (int)floorf(p[k]);
		i1[k] = i0[k] + 1 < c->n ? i0[k] + 1 : i0[k];
		f[k] = p[k] - (float)i0[k];
	}
	float out[3] = {0};
	for (int corner = 0; corner < 8; corner++) {
		int ir = (corner & 1) ? i1[0] : i0[0];
		int ig = (corner & 2) ? i1[1] : i0[1];
		int ib = (corner & 4) ? i1[2] : i0[2];
		float w = ((corner & 1) ? f[0] : 1.0f - f[0]) * ((corner & 2) ? f[1] : 1.0f - f[1]) *
			  ((corner & 4) ? f[2] : 1.0f - f[2]);
		const float *v = &c->data[(((size_t)ib * c->n + ig) * c->n + ir) * 3];
		for (int k = 0; k < 3; k++)
			out[k] += v[k] * w;
	}
	memcpy(rgb, out, sizeof(out));
}

/* ------------------------------------------------------------------------- */
/* filter                                                                    */

struct gradefx {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texture_t *lut;
	float lut_rgb[LUT_N * LUT_N * LUT_N * 3];

	int preset;
	float preset_amt;
	struct grade manual;
	char *cube_path;
	struct cube cube;
	float cube_amt;
	float amount;
	bool compare;
	float split;
};

static const char *grade_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Grade.Name");
}

static uint16_t f2h(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	uint32_t sign = (x >> 16) & 0x8000;
	int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
	uint32_t m = x & 0x7fffff;
	if (e <= 0)
		return (uint16_t)sign;
	if (e >= 31)
		return (uint16_t)(sign | 0x7c00);
	return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

static void build_lut(struct gradefx *f)
{
	struct grade pg;
	grade_identity(&pg);
	bool has_preset = f->preset > 0 && f->preset <= PRESET_COUNT && f->preset_amt > 0.0f;
	if (has_preset)
		presets[f->preset - 1].make(&pg);

	for (int b = 0; b < LUT_N; b++)
		for (int g = 0; g < LUT_N; g++)
			for (int r = 0; r < LUT_N; r++) {
				float in[3] = {(float)r / (LUT_N - 1), (float)g / (LUT_N - 1), (float)b / (LUT_N - 1)};
				float c[3] = {in[0], in[1], in[2]};
				if (has_preset) {
					float p[3] = {c[0], c[1], c[2]};
					grade_apply(&pg, p);
					for (int k = 0; k < 3; k++)
						c[k] = kg_lerpf(c[k], p[k], f->preset_amt);
				}
				grade_apply(&f->manual, c);
				if (f->cube.data && f->cube_amt > 0.0f) {
					float q[3] = {c[0], c[1], c[2]};
					cube_sample(&f->cube, q);
					for (int k = 0; k < 3; k++)
						c[k] = kg_lerpf(c[k], q[k], f->cube_amt);
				}
				float *o = &f->lut_rgb[(((size_t)b * LUT_N + g) * LUT_N + r) * 3];
				for (int k = 0; k < 3; k++)
					o[k] = kg_clampf(kg_lerpf(in[k], c[k], f->amount), 0.0f, 1.0f);
			}

	uint16_t *px = bmalloc(sizeof(uint16_t) * 4 * LUT_N * LUT_N * LUT_N);
	for (size_t i = 0; i < (size_t)LUT_N * LUT_N * LUT_N; i++) {
		px[i * 4 + 0] = f2h(f->lut_rgb[i * 3 + 0]);
		px[i * 4 + 1] = f2h(f->lut_rgb[i * 3 + 1]);
		px[i * 4 + 2] = f2h(f->lut_rgb[i * 3 + 2]);
		px[i * 4 + 3] = 0x3c00; /* 1.0 */
	}
	obs_enter_graphics();
	gs_voltexture_destroy(f->lut);
	f->lut = gs_voltexture_create(LUT_N, LUT_N, LUT_N, GS_RGBA16F, 1, (const uint8_t **)&px, 0);
	obs_leave_graphics();
	bfree(px);
}

static void read_color(obs_data_t *s, const char *key, float *out)
{
	struct vec4 v;
	vec4_from_rgba(&v, (uint32_t)obs_data_get_int(s, key));
	set3(out, v.x, v.y, v.z);
}

static void grade_update(void *data, obs_data_t *s)
{
	struct gradefx *f = data;
	f->preset = (int)obs_data_get_int(s, "preset");
	f->preset_amt = (float)obs_data_get_double(s, "preset_amt") / 100.0f;
	f->amount = (float)obs_data_get_double(s, "amount") / 100.0f;
	f->compare = obs_data_get_bool(s, "compare");
	f->split = (float)obs_data_get_double(s, "split") / 100.0f;

	struct grade *g = &f->manual;
	grade_identity(g);
	g->exposure = (float)obs_data_get_double(s, "exposure");
	g->temp = (float)obs_data_get_double(s, "temp") / 100.0f;
	g->tint = (float)obs_data_get_double(s, "tint") / 100.0f;
	g->contrast = (float)obs_data_get_double(s, "contrast") / 100.0f;
	g->sat = (float)obs_data_get_double(s, "sat") / 100.0f;
	g->vib = (float)obs_data_get_double(s, "vib") / 100.0f;
	g->hue = (float)obs_data_get_double(s, "hue");
	g->lift = (float)obs_data_get_double(s, "lift") / 100.0f;
	g->gamma = (float)obs_data_get_double(s, "gamma") / 100.0f;
	g->gain = (float)obs_data_get_double(s, "gain") / 100.0f;
	read_color(s, "sh_col", g->sh_col);
	g->sh_amt = (float)obs_data_get_double(s, "sh_amt") / 100.0f;
	read_color(s, "mid_col", g->mid_col);
	g->mid_amt = (float)obs_data_get_double(s, "mid_amt") / 100.0f;
	read_color(s, "hi_col", g->hi_col);
	g->hi_amt = (float)obs_data_get_double(s, "hi_amt") / 100.0f;
	g->fade = (float)obs_data_get_double(s, "fade") / 100.0f;

	const char *path = obs_data_get_string(s, "cube");
	if (!f->cube_path || strcmp(f->cube_path, path) != 0) {
		bfree(f->cube_path);
		f->cube_path = bstrdup(path);
		if (*path)
			cube_load(&f->cube, path);
		else
			cube_free(&f->cube);
	}
	f->cube_amt = (float)obs_data_get_double(s, "cube_amt") / 100.0f;
	build_lut(f);
}

static void grade_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "preset", 1);
	obs_data_set_default_double(s, "preset_amt", 70.0);
	obs_data_set_default_double(s, "amount", 100.0);
	obs_data_set_default_double(s, "split", 50.0);
	obs_data_set_default_double(s, "contrast", 100.0);
	obs_data_set_default_double(s, "sat", 100.0);
	obs_data_set_default_double(s, "gamma", 100.0);
	obs_data_set_default_double(s, "gain", 100.0);
	obs_data_set_default_int(s, "sh_col", 0xFFB08040); /* ABGR: teal-blue */
	obs_data_set_default_int(s, "mid_col", 0xFFFFFFFF);
	obs_data_set_default_int(s, "hi_col", 0xFF60A0FF); /* ABGR: orange */
	obs_data_set_default_double(s, "cube_amt", 100.0);
}

static bool btn_export(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct gradefx *f = data;
	obs_data_t *s = obs_source_get_settings(f->context);
	const char *path = obs_data_get_string(s, "export_path");
	if (path && *path) {
		struct dstr out = {0};
		dstr_printf(&out, "TITLE \"%s\"\nLUT_3D_SIZE %d\n", obs_source_get_name(f->context), LUT_N);
		for (size_t i = 0; i < (size_t)LUT_N * LUT_N * LUT_N; i++)
			dstr_catf(&out, "%.6f %.6f %.6f\n", f->lut_rgb[i * 3], f->lut_rgb[i * 3 + 1], f->lut_rgb[i * 3 + 2]);
		if (!os_quick_write_utf8_file(path, out.array, out.len, false))
			blog_kg(LOG_WARNING, "could not write %s", path);
		else
			blog_kg(LOG_INFO, "exported LUT to %s", path);
		dstr_free(&out);
	}
	obs_data_release(s);
	return false;
}

static bool compare_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, "split"), obs_data_get_bool(s, "compare"));
	return true;
}

static obs_properties_t *grade_props(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_list(props, "preset", T_("Grade.Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("None"), 0);
	for (int i = 0; i < PRESET_COUNT; i++)
		obs_property_list_add_int(p, T_(presets[i].key), i + 1);
	obs_properties_add_float_slider(props, "preset_amt", T_("Grade.PresetAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "amount", T_("Grade.Amount"), 0.0, 100.0, 1.0);
	p = obs_properties_add_bool(props, "compare", T_("Grade.Compare"));
	obs_property_set_modified_callback(p, compare_modified);
	obs_properties_add_float_slider(props, "split", T_("Grade.Split"), 0.0, 100.0, 0.5);

	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "exposure", T_("Grade.Exposure"), -3.0, 3.0, 0.01);
	obs_properties_add_float_slider(g, "temp", T_("Grade.Temp"), -100.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "tint", T_("Grade.Tint"), -100.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "contrast", T_("Grade.Contrast"), 0.0, 200.0, 1.0);
	obs_properties_add_float_slider(g, "sat", T_("Grade.Saturation"), 0.0, 200.0, 1.0);
	obs_properties_add_float_slider(g, "vib", T_("Grade.Vibrance"), -100.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "hue", T_("Grade.Hue"), -180.0, 180.0, 1.0);
	obs_properties_add_float_slider(g, "fade", T_("Grade.Fade"), 0.0, 50.0, 0.5);
	obs_properties_add_group(props, "grp_basic", T_("Grade.Basic"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_float_slider(g, "lift", T_("Grade.Lift"), -50.0, 50.0, 0.5);
	obs_properties_add_float_slider(g, "gamma", T_("Grade.Gamma"), 20.0, 300.0, 1.0);
	obs_properties_add_float_slider(g, "gain", T_("Grade.Gain"), 0.0, 200.0, 1.0);
	obs_properties_add_color(g, "sh_col", T_("Grade.ShadowColor"));
	obs_properties_add_float_slider(g, "sh_amt", T_("Grade.ShadowAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_color(g, "mid_col", T_("Grade.MidColor"));
	obs_properties_add_float_slider(g, "mid_amt", T_("Grade.MidAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_color(g, "hi_col", T_("Grade.HighColor"));
	obs_properties_add_float_slider(g, "hi_amt", T_("Grade.HighAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_group(props, "grp_wheels", T_("Grade.Wheels"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_path(g, "cube", T_("Grade.CubeFile"), OBS_PATH_FILE, "Cube LUT (*.cube);;All files (*.*)",
				NULL);
	obs_properties_add_float_slider(g, "cube_amt", T_("Grade.CubeAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_path(g, "export_path", T_("Grade.ExportPath"), OBS_PATH_FILE_SAVE, "Cube LUT (*.cube)",
				NULL);
	obs_properties_add_button2(g, "btn_export", T_("Grade.Export"), btn_export, data);
	obs_properties_add_group(props, "grp_lut", T_("Grade.LutGroup"), OBS_GROUP_NORMAL, g);
	return props;
}

static void *grade_create(obs_data_t *settings, obs_source_t *source)
{
	struct gradefx *f = bzalloc(sizeof(*f));
	f->context = source;
	obs_enter_graphics();
	f->effect = kg_load_effect("effects/grade.effect");
	obs_leave_graphics();
	if (!f->effect) {
		bfree(f);
		return NULL;
	}
	grade_update(f, settings);
	return f;
}

static void grade_destroy(void *data)
{
	struct gradefx *f = data;
	obs_enter_graphics();
	gs_voltexture_destroy(f->lut);
	obs_leave_graphics();
	cube_free(&f->cube);
	bfree(f->cube_path);
	bfree(f);
}

static void grade_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct gradefx *f = data;
	if (!f->lut || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	gs_effect_t *e = f->effect;
	struct vec3 scale, offset;
	float sc = (float)(LUT_N - 1) / (float)LUT_N, of = 0.5f / (float)LUT_N;
	vec3_set(&scale, sc, sc, sc);
	vec3_set(&offset, of, of, of);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "clut"), f->lut);
	gs_effect_set_vec3(gs_effect_get_param_by_name(e, "clut_scale"), &scale);
	gs_effect_set_vec3(gs_effect_get_param_by_name(e, "clut_offset"), &offset);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "split"), f->compare ? f->split : -1.0f);
	obs_source_process_filter_end(f->context, e, 0, 0);
}

struct obs_source_info kagee_grade_filter = {
	.id = "kagee_grade_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = grade_name,
	.create = grade_create,
	.destroy = grade_destroy,
	.update = grade_update,
	.get_defaults = grade_defaults,
	.get_properties = grade_props,
	.video_render = grade_render,
};
