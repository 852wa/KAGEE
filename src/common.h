#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/axisang.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>

#define KG_PI 3.14159265358979f
#define KG_DEG2RAD(x) ((x) * (KG_PI / 180.0f))
#define T_(s) obs_module_text(s)

#define blog_kg(level, fmt, ...) blog(level, "[Kagee] " fmt, ##__VA_ARGS__)

/* ---- math / animation ---- */

enum kg_easing {
	KG_EASE_CUT = 0,
	KG_EASE_LINEAR,
	KG_EASE_SMOOTH,     /* cubic in-out */
	KG_EASE_OUT,        /* expo out: fast start, gentle landing */
	KG_EASE_IN,         /* cubic in */
	KG_EASE_BACK,       /* slight overshoot */
	KG_EASE_ELASTIC,
};

float kg_ease(int type, float t);
float kg_clampf(float v, float lo, float hi);
float kg_lerpf(float a, float b, float t);
/* smooth 1D value noise in [-1, 1] and fractal sum */
float kg_noise1(float x);
float kg_fbm1(float x, int octaves);
float kg_rand01(uint32_t *state);

/* ---- graphics helpers ---- */

gs_effect_t *kg_load_effect(const char *file);
void kg_add_easing_list(obs_property_t *p);

/* Renders a filter's input (the target source) into `tr`, premultiplied.
 * Returns false if nothing should be drawn (fallback to skip). */
bool kg_filter_capture(obs_source_t *filter, gs_texrender_t *tr, uint32_t *out_cx, uint32_t *out_cy);

/* Draws a premultiplied texture at 0,0,cx,cy with the default effect. */
void kg_draw_premultiplied(gs_texture_t *tex, uint32_t cx, uint32_t cy);

/* Draws tex into tr (size cx,cy) with effect/technique; effect params must be set by caller. */
gs_texture_t *kg_pass(gs_texrender_t *tr, gs_effect_t *effect, const char *tech, gs_texture_t *tex, uint32_t cx,
		      uint32_t cy);

/* Separable gaussian blur with automatic downsampling for large radii. */
struct kg_blur {
	gs_effect_t *effect;
	gs_texrender_t *down;
	gs_texrender_t *a;
	gs_texrender_t *b;
	uint32_t out_cx, out_cy;
};

void kg_blur_init(struct kg_blur *b);
void kg_blur_free(struct kg_blur *b);
/* Returns blurred texture (possibly smaller than cx,cy; see b->out_cx/out_cy). `dir` is an
 * optional unit direction for a 1D (directional) blur, or NULL for a 2D gaussian. */
gs_texture_t *kg_blur_apply(struct kg_blur *b, gs_texture_t *src, uint32_t cx, uint32_t cy, float radius,
			    const struct vec2 *dir);
