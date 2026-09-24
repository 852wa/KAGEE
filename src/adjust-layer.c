/*
 * Kagee Adjustment Layer
 *
 * A source that re-renders every scene item *below itself* in its scene (same transforms,
 * crop and blend modes) so that the filters added to this source act on "everything below",
 * i.e. an adjustment layer. Put it at the top of a scene and it
 * becomes a post effect for the whole scene; put it in the middle and the layers above it
 * are unaffected. Works with any OBS filter, not only Kagee ones.
 *
 * The scene item list is snapshotted in video_tick (graphics thread, outside the scene's
 * render lock); item transforms are read at render time so they are always current.
 */
#include "common.h"

#define MAX_BELOW 64

struct adjust {
	obs_source_t *context;
	char *scene_name; /* "" = auto: the scene/group that contains this layer */
	int depth;        /* only the N items directly below (0 = all) */
	uint32_t fill_color;

	obs_sceneitem_t *below[MAX_BELOW];
	int below_count;
	uint32_t cx, cy;

	gs_texrender_t *crop_tr[MAX_BELOW];
};

struct blend_params {
	enum gs_blend_type src_c, src_a, dst_c, dst_a;
	enum gs_blend_op_type op;
};

/* same table as libobs' scene renderer (premultiplied alpha) */
static const struct blend_params item_blend[] = {
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_ADD},
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_REVERSE_SUBTRACT},
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	{GS_BLEND_DSTCOLOR, GS_BLEND_DSTALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MAX},
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MIN},
};

static const char *adjust_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Adjust.Name");
}

static void release_snapshot(struct adjust *a)
{
	for (int i = 0; i < a->below_count; i++)
		obs_sceneitem_release(a->below[i]);
	a->below_count = 0;
}

/* ------------------------------------------------------------------------- */
/* finding our place in the scene tree                                       */

struct collect_ctx {
	struct adjust *a;
	obs_sceneitem_t *items[MAX_BELOW];
	int count;
	bool found;
};

static bool collect_items(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct collect_ctx *ctx = param;
	if (obs_sceneitem_get_source(item) == ctx->a->context) {
		ctx->found = true;
		return false; /* everything above us is irrelevant */
	}
	if (ctx->count < MAX_BELOW)
		ctx->items[ctx->count++] = item;
	return true;
}

/* Returns true (and fills the snapshot) if `scene_src` directly contains this layer. */
static bool try_scene(struct adjust *a, obs_source_t *scene_src)
{
	obs_scene_t *scene = obs_group_or_scene_from_source(scene_src);
	if (!scene)
		return false;
	struct collect_ctx ctx = {.a = a};
	obs_scene_enum_items(scene, collect_items, &ctx);
	if (!ctx.found)
		return false;

	int start = (a->depth > 0 && ctx.count > a->depth) ? ctx.count - a->depth : 0;
	for (int i = start; i < ctx.count; i++) {
		obs_sceneitem_addref(ctx.items[i]);
		a->below[a->below_count++] = ctx.items[i];
	}
	a->cx = obs_source_get_width(scene_src);
	a->cy = obs_source_get_height(scene_src);
	return true;
}

static bool find_scene_cb(void *param, obs_source_t *scene_src)
{
	return !try_scene(param, scene_src); /* stop at the first scene containing us */
}

static void adjust_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct adjust *a = data;
	release_snapshot(a);
	a->cx = a->cy = 0;

	bool found = false;
	if (a->scene_name && *a->scene_name) {
		obs_source_t *src = obs_get_source_by_name(a->scene_name);
		if (src) {
			found = try_scene(a, src);
			obs_source_release(src);
		}
	} else {
		obs_enum_scenes(find_scene_cb, a);
		found = a->cx > 0 && a->cy > 0;
	}

	if (!found || !a->cx || !a->cy) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			a->cx = ovi.base_width;
			a->cy = ovi.base_height;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* rendering                                                                 */

static void draw_item(struct adjust *a, int idx, obs_sceneitem_t *item)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src || !obs_sceneitem_visible(item))
		return;
	uint32_t w = obs_source_get_width(src), h = obs_source_get_height(src);
	if (!w || !h)
		return;

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	int mode = (int)obs_sceneitem_get_blending_mode(item);
	if (mode < 0 || mode >= (int)(sizeof(item_blend) / sizeof(item_blend[0])))
		mode = 0;
	bool cropped = crop.left || crop.top || crop.right || crop.bottom;

	struct matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	gs_matrix_push();
	gs_matrix_mul(&m);

	if (!cropped && mode == 0) {
		obs_source_video_render(src);
	} else {
		/* like the scene renderer: crop into a texture, then draw it with the blend mode */
		int cx = (int)w - crop.left - crop.right;
		int cy = (int)h - crop.top - crop.bottom;
		if (cx > 0 && cy > 0) {
			if (!a->crop_tr[idx])
				a->crop_tr[idx] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			gs_texrender_t *tr = a->crop_tr[idx];
			gs_texrender_reset(tr);
			if (gs_texrender_begin(tr, (uint32_t)cx, (uint32_t)cy)) {
				struct vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
				gs_matrix_translate3f(-(float)crop.left, -(float)crop.top, 0.0f);
				gs_blend_state_push();
				gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
							   GS_BLEND_INVSRCALPHA);
				obs_source_video_render(src);
				gs_blend_state_pop();
				gs_texrender_end(tr);

				gs_texture_t *tex = gs_texrender_get_texture(tr);
				gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				const struct blend_params *bp = &item_blend[mode];
				gs_blend_state_push();
				gs_blend_function_separate(bp->src_c, bp->dst_c, bp->src_a, bp->dst_a);
				gs_blend_op(bp->op);
				gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
				while (gs_effect_loop(e, "Draw"))
					gs_draw_sprite(tex, 0, (uint32_t)cx, (uint32_t)cy);
				gs_blend_op(GS_BLEND_OP_ADD);
				gs_blend_state_pop();
			}
		}
	}
	gs_matrix_pop();
}

static void adjust_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct adjust *a = data;

	/* optional backdrop, so filters that move pixels (distortion, shake...) don't reveal
	 * the unprocessed layers underneath */
	struct vec4 fill;
	vec4_from_rgba(&fill, a->fill_color);
	if (fill.w > 0.0f) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		vec4_set(&fill, fill.x * fill.w, fill.y * fill.w, fill.z * fill.w, fill.w);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &fill);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(NULL, 0, a->cx, a->cy);
		gs_blend_state_pop();
	}

	gs_blend_state_push();
	gs_reset_blend_state();
	for (int i = 0; i < a->below_count; i++)
		draw_item(a, i, a->below[i]);
	gs_blend_state_pop();
}

/* ------------------------------------------------------------------------- */

static void adjust_update(void *data, obs_data_t *s)
{
	struct adjust *a = data;
	bfree(a->scene_name);
	a->scene_name = bstrdup(obs_data_get_string(s, "scene"));
	a->depth = (int)obs_data_get_int(s, "depth");
	a->fill_color = (uint32_t)obs_data_get_int(s, "fill_color");
}

static void adjust_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "scene", "");
	obs_data_set_default_int(s, "depth", 0);
	obs_data_set_default_int(s, "fill_color", 0x00000000);
}

static bool add_scene(void *param, obs_source_t *src)
{
	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_property_list_add_string(param, name, name);
	return true;
}

static obs_properties_t *adjust_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "help", T_("Adjust.Help"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "scene", T_("Adjust.Scene"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Adjust.SceneAuto"), "");
	obs_enum_scenes(add_scene, p);
	obs_properties_add_int(props, "depth", T_("Adjust.Depth"), 0, MAX_BELOW, 1);
	obs_properties_add_color_alpha(props, "fill_color", T_("Adjust.Fill"));
	return props;
}

static void *adjust_create(obs_data_t *settings, obs_source_t *source)
{
	struct adjust *a = bzalloc(sizeof(*a));
	a->context = source;
	adjust_update(a, settings);
	return a;
}

static void adjust_destroy(void *data)
{
	struct adjust *a = data;
	release_snapshot(a);
	a->cx = a->cy = 0;
	obs_enter_graphics();
	for (int i = 0; i < MAX_BELOW; i++)
		gs_texrender_destroy(a->crop_tr[i]);
	obs_leave_graphics();
	bfree(a->scene_name);
	bfree(a);
}

static uint32_t adjust_width(void *data)
{
	return ((struct adjust *)data)->cx;
}

static uint32_t adjust_height(void *data)
{
	return ((struct adjust *)data)->cy;
}

struct obs_source_info kagee_adjust_source = {
	.id = "kagee_adjustment_layer",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
	.get_name = adjust_name,
	.create = adjust_create,
	.destroy = adjust_destroy,
	.update = adjust_update,
	.get_defaults = adjust_defaults,
	.get_properties = adjust_props,
	.get_width = adjust_width,
	.get_height = adjust_height,
	.video_tick = adjust_tick,
	.video_render = adjust_render,
};
