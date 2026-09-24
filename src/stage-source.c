/*
 * Kagee 3D Stage
 *
 * Composites up to MAX_LAYERS OBS sources as planes in 3D space (multiplane) and films them
 * with a virtual camera. Camera "shots" can be stored and recalled with eased transitions via
 * hotkeys, buttons, a timer or BPM. Adds handheld/idle camera motion, beat pulse, impact shake,
 * flash, and depth-of-field blur by distance from the focus plane.
 */
#include "common.h"
#include <callback/signal.h>
#include <util/dstr.h>
#include <stdlib.h>

#define MAX_LAYERS 8
#define MAX_SHOTS 8
#define RETRY_INTERVAL 1.0f

enum layer_type { LAYER_SOURCE = 0, LAYER_FILE };
enum fit_mode { FIT_ORIGINAL = 0, FIT_COVER, FIT_CONTAIN, FIT_STRETCH };
enum blend_mode { BLEND_NORMAL = 0, BLEND_ADD, BLEND_SCREEN, BLEND_MULTIPLY, BLEND_SUBTRACT };
enum idle_mode { IDLE_NONE = 0, IDLE_SWAY, IDLE_ORBIT, IDLE_BREATHE, IDLE_DRIFT };
enum auto_mode { AUTO_OFF = 0, AUTO_SECONDS, AUTO_BEATS };

struct cam {
	float x, y, dolly, yaw, pitch, roll, fov;
};

/* animatable layer transform: what a shot remembers about each layer */
struct ltf {
	float x, y, z, scale, yaw, pitch, roll, opacity;
};

struct layer {
	char *name;
	obs_weak_source_t *weak;
	float retry;
	bool warned;

	/* file layers: a private image/media source owned by the stage */
	char *file;
	obs_source_t *own;

	bool on;
	struct ltf live; /* as set in the settings */
	/* displayed (animated) transform, used by the renderer */
	float x, y, z, scale, yaw, pitch, roll, opacity;
	int fit, blend;
	bool compensate, dof, autofill;

	gs_texrender_t *tr;
	float view_z; /* per-frame, for sorting */
};

enum action {
	ACT_NONE = 0,
	ACT_SHOT_BASE = 1, /* ACT_SHOT_BASE + i (i = 0..MAX_SHOTS), 0 = live camera */
	ACT_NEXT = 100,
	ACT_PREV,
	ACT_RANDOM,
	ACT_IMPACT,
	ACT_FLASH,
	ACT_TAP,
	ACT_AUTO_TOGGLE,
};

struct stage {
	obs_source_t *context;
	pthread_mutex_t mutex;

	uint32_t cx, cy;
	uint32_t req_cx, req_cy;
	uint32_t bg_color;
	float base_fov;
	bool auto_fill;

	int layer_count;
	struct layer layers[MAX_LAYERS];

	/* camera */
	struct cam live;
	struct cam shots[MAX_SHOTS];
	bool shot_valid[MAX_SHOTS];
	int cur_shot; /* 0 = live, 1..MAX_SHOTS */
	struct cam from, to, base;
	struct ltf lfrom[MAX_LAYERS], lto[MAX_LAYERS];
	struct ltf shot_layers[MAX_SHOTS][MAX_LAYERS];
	bool shot_has_layers[MAX_SHOTS];
	float depth_scale;
	float anim_t, anim_dur;
	int anim_ease;
	float trans_dur, edit_smooth;
	int trans_ease;

	/* motion */
	float hand_amount, hand_speed;
	int idle_mode;
	float idle_amp, idle_period;
	float beat_pulse;

	/* auto switch / bpm */
	int auto_mode;
	bool auto_enabled;
	float auto_interval;
	int auto_beats;
	bool auto_random;
	float bpm;
	float auto_timer;
	double beat_origin;
	long long last_beat;
	uint64_t taps[8];
	int tap_count;

	/* triggers */
	float impact_amount, impact_dur, impact;
	uint32_t flash_color;
	float flash_dur, flash;

	/* dof */
	bool dof_on;
	float dof_focus, dof_strength, dof_max;

	double time;
	uint32_t rng;

	/* pending actions from hotkeys/buttons (processed in tick) */
	int pending[16];
	int pending_count;

	/* interaction */
	bool drag_left, drag_right, drag_middle;
	int32_t last_mx, last_my;
	bool cam_dirty;

	/* graphics */
	gs_effect_t *effect;
	gs_texrender_t *stage_tr;
	struct kg_blur blur;

	obs_hotkey_id hotkeys[32];
	int hotkey_count;
};

/* ------------------------------------------------------------------------- */

static const char *stage_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Stage.Name");
}

static void cam_lerp(struct cam *out, const struct cam *a, const struct cam *b, float t)
{
	out->x = kg_lerpf(a->x, b->x, t);
	out->y = kg_lerpf(a->y, b->y, t);
	out->dolly = kg_lerpf(a->dolly, b->dolly, t);
	out->yaw = kg_lerpf(a->yaw, b->yaw, t);
	out->pitch = kg_lerpf(a->pitch, b->pitch, t);
	out->roll = kg_lerpf(a->roll, b->roll, t);
	out->fov = kg_lerpf(a->fov, b->fov, t);
}

static bool cam_equal(const struct cam *a, const struct cam *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static void cam_read(struct cam *c, obs_data_t *d, const char *prefix)
{
	char key[64];
#define RD(field)                                                   \
	snprintf(key, sizeof(key), "%s_" #field, prefix);           \
	c->field = (float)obs_data_get_double(d, key);
	RD(x) RD(y) RD(dolly) RD(yaw) RD(pitch) RD(roll) RD(fov)
#undef RD
}

static void cam_write(const struct cam *c, obs_data_t *d, const char *prefix)
{
	char key[64];
#define WR(field)                                                   \
	snprintf(key, sizeof(key), "%s_" #field, prefix);           \
	obs_data_set_double(d, key, c->field);
	WR(x) WR(y) WR(dolly) WR(yaw) WR(pitch) WR(roll) WR(fov)
#undef WR
}

static void cam_defaults(obs_data_t *d, const char *prefix)
{
	char key[64];
	const char *fields[] = {"x", "y", "dolly", "yaw", "pitch", "roll"};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		snprintf(key, sizeof(key), "%s_%s", prefix, fields[i]);
		obs_data_set_default_double(d, key, 0.0);
	}
	snprintf(key, sizeof(key), "%s_fov", prefix);
	obs_data_set_default_double(d, key, 40.0);
}

static float ref_distance(const struct stage *s)
{
	float fov = kg_clampf(s->base_fov, 5.0f, 150.0f);
	return ((float)s->cy * 0.5f) / tanf(KG_DEG2RAD(fov) * 0.5f);
}

static void push_action(struct stage *s, int act)
{
	pthread_mutex_lock(&s->mutex);
	if (s->pending_count < (int)(sizeof(s->pending) / sizeof(s->pending[0])))
		s->pending[s->pending_count++] = act;
	pthread_mutex_unlock(&s->mutex);
}

static void ltf_get(const struct layer *l, struct ltf *t)
{
	*t = (struct ltf){l->x, l->y, l->z, l->scale, l->yaw, l->pitch, l->roll, l->opacity};
}

static void ltf_set(struct layer *l, const struct ltf *t)
{
	l->x = t->x;
	l->y = t->y;
	l->z = t->z;
	l->scale = t->scale;
	l->yaw = t->yaw;
	l->pitch = t->pitch;
	l->roll = t->roll;
	l->opacity = t->opacity;
}

static void ltf_lerp(struct ltf *o, const struct ltf *a, const struct ltf *b, float t)
{
	const float *fa = (const float *)a, *fb = (const float *)b;
	float *fo = (float *)o;
	for (size_t i = 0; i < sizeof(struct ltf) / sizeof(float); i++)
		fo[i] = kg_lerpf(fa[i], fb[i], t);
}

static bool ltf_equal(const struct ltf *a, const struct ltf *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* Animate camera AND layer layout to a target. layers == NULL means "the live layout". */
static void start_transition_full(struct stage *s, const struct cam *target, const struct ltf *layers, float dur,
				  int ease)
{
	s->from = s->base;
	s->to = *target;
	for (int i = 0; i < MAX_LAYERS; i++) {
		ltf_get(&s->layers[i], &s->lfrom[i]);
		s->lto[i] = layers ? layers[i] : s->layers[i].live;
	}
	s->anim_t = 0.0f;
	s->anim_dur = (ease == KG_EASE_CUT) ? 0.0f : dur;
	s->anim_ease = ease;
	if (s->anim_dur <= 0.0f) {
		s->base = s->to;
		for (int i = 0; i < MAX_LAYERS; i++)
			ltf_set(&s->layers[i], &s->lto[i]);
	}
}

static void start_transition(struct stage *s, const struct cam *target, float dur, int ease)
{
	start_transition_full(s, target, NULL, dur, ease);
}

static void go_to_shot(struct stage *s, int shot)
{
	if (shot < 0 || shot > MAX_SHOTS)
		return;
	if (shot > 0 && !s->shot_valid[shot - 1])
		return;
	s->cur_shot = shot;
	if (shot == 0)
		start_transition_full(s, &s->live, NULL, s->trans_dur, s->trans_ease);
	else
		start_transition_full(s, &s->shots[shot - 1], s->shot_has_layers[shot - 1] ? s->shot_layers[shot - 1] : NULL,
				      s->trans_dur, s->trans_ease);
}

/* ------------------------------------------------------------------------- */
/* shots: camera + layer layout, presets                                     */

static void write_shot_layers(struct stage *s, obs_data_t *settings, int slot)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int j = 0; j < MAX_LAYERS; j++) {
		const struct ltf *t = &s->layers[j].live;
		obs_data_t *o = obs_data_create();
		obs_data_set_double(o, "x", t->x);
		obs_data_set_double(o, "y", t->y);
		obs_data_set_double(o, "z", t->z);
		obs_data_set_double(o, "scale", t->scale);
		obs_data_set_double(o, "yaw", t->yaw);
		obs_data_set_double(o, "pitch", t->pitch);
		obs_data_set_double(o, "roll", t->roll);
		obs_data_set_double(o, "opacity", t->opacity);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	char key[32];
	snprintf(key, sizeof(key), "shot%d_layers", slot);
	obs_data_set_array(settings, key, arr);
	obs_data_array_release(arr);
}

/* Saves the live camera + live layer layout ("what you see now") into a shot. */
static void save_live_to_slot(struct stage *s, int slot)
{
	if (slot < 1 || slot > MAX_SHOTS)
		return;
	obs_data_t *settings = obs_source_get_settings(s->context);
	char key[32];
	snprintf(key, sizeof(key), "shot%d", slot);
	cam_write(&s->live, settings, key);
	write_shot_layers(s, settings, slot);
	snprintf(key, sizeof(key), "shot%d_valid", slot);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "shot%d_preset", slot);
	obs_data_set_int(settings, key, 0);
	obs_source_update(s->context, NULL);
	obs_data_release(settings);
}

enum shot_preset {
	SP_FRONT = 1,
	SP_CLOSE,
	SP_WIDE,
	SP_LEFT,
	SP_RIGHT,
	SP_LOW,
	SP_HIGH,
	SP_DUTCH,
	SP_TRUCK,
	SP_TELE,
	SP_WIDE_ANGLE,
	SP_DRAMA,
	SP_COUNT
};

static const char *preset_keys[SP_COUNT] = {
	NULL,         "Preset.Front", "Preset.Close", "Preset.Wide",  "Preset.Left", "Preset.Right",    "Preset.Low",
	"Preset.High", "Preset.Dutch", "Preset.Truck", "Preset.Tele", "Preset.WideAngle", "Preset.Drama"};

/* distance change that keeps the subject plane the same size when the FOV changes */
static float dolly_for_fov(const struct stage *s, float fov)
{
	float d = ref_distance(s);
	float d2 = ((float)s->cy * 0.5f) / tanf(KG_DEG2RAD(fov) * 0.5f);
	return d - d2;
}

static void preset_camera(const struct stage *s, int preset, struct cam *c)
{
	float d = ref_distance(s);
	memset(c, 0, sizeof(*c));
	c->fov = s->base_fov > 0.0f ? s->base_fov : 40.0f;
	switch (preset) {
	case SP_CLOSE:
		c->dolly = d * 0.45f;
		c->y = -(float)s->cy * 0.06f;
		break;
	case SP_WIDE:
		c->dolly = -d * 0.35f;
		break;
	case SP_LEFT:
		c->yaw = -22.0f;
		c->dolly = d * 0.1f;
		break;
	case SP_RIGHT:
		c->yaw = 22.0f;
		c->dolly = d * 0.1f;
		break;
	case SP_LOW:
		c->pitch = -12.0f;
		c->dolly = d * 0.15f;
		break;
	case SP_HIGH:
		c->pitch = 16.0f;
		break;
	case SP_DUTCH:
		c->roll = 8.0f;
		c->dolly = d * 0.2f;
		break;
	case SP_TRUCK:
		c->x = -(float)s->cx * 0.12f;
		c->yaw = -8.0f;
		break;
	case SP_TELE:
		c->fov = 18.0f;
		c->dolly = dolly_for_fov(s, 18.0f);
		break;
	case SP_WIDE_ANGLE:
		c->fov = 75.0f;
		c->dolly = dolly_for_fov(s, 75.0f) + d * 0.1f;
		break;
	case SP_DRAMA:
		c->dolly = d * 0.55f;
		c->yaw = 10.0f;
		c->roll = -6.0f;
		break;
	default:
		break;
	}
}

/* Puts a preset camera (with the current live layout) into a shot slot. */
static void apply_preset_to_slot(struct stage *s, int slot, int preset)
{
	if (slot < 1 || slot > MAX_SHOTS || preset < 1 || preset >= SP_COUNT)
		return;
	struct cam c;
	preset_camera(s, preset, &c);
	obs_data_t *settings = obs_source_get_settings(s->context);
	char key[32];
	snprintf(key, sizeof(key), "shot%d", slot);
	cam_write(&c, settings, key);
	write_shot_layers(s, settings, slot);
	snprintf(key, sizeof(key), "shot%d_valid", slot);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "shot%d_preset", slot);
	obs_data_set_int(settings, key, preset);
	obs_source_update(s->context, NULL);
	obs_data_release(settings);
}

/* One click: a varied sequence of 8 shots. */
static void auto_fill_shots(struct stage *s)
{
	static const int seq[MAX_SHOTS] = {SP_FRONT, SP_CLOSE, SP_LEFT, SP_RIGHT, SP_LOW, SP_WIDE, SP_DUTCH, SP_HIGH};
	for (int i = 0; i < MAX_SHOTS; i++)
		apply_preset_to_slot(s, i + 1, seq[i]);
}

static int valid_shot_list(struct stage *s, int *out)
{
	int n = 0;
	for (int i = 0; i < MAX_SHOTS; i++)
		if (s->shot_valid[i])
			out[n++] = i + 1;
	return n;
}

static void step_shot(struct stage *s, int dir, bool random)
{
	int list[MAX_SHOTS];
	int n = valid_shot_list(s, list);
	if (n == 0)
		return;
	if (random && n > 1) {
		int pick;
		do {
			pick = list[(int)(kg_rand01(&s->rng) * (float)n) % n];
		} while (pick == s->cur_shot);
		go_to_shot(s, pick);
		return;
	}
	int idx = -1;
	for (int i = 0; i < n; i++)
		if (list[i] == s->cur_shot)
			idx = i;
	idx = (idx < 0) ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
	go_to_shot(s, list[idx]);
}

/* ------------------------------------------------------------------------- */
/* child sources                                                             */

struct find_ctx {
	obs_source_t *target;
	bool found;
};

static void find_in_tree(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct find_ctx *ctx = param;
	if (child == ctx->target)
		ctx->found = true;
}

static void layer_detach(struct stage *s, struct layer *l)
{
	pthread_mutex_lock(&s->mutex);
	obs_weak_source_t *weak = l->weak;
	l->weak = NULL;
	pthread_mutex_unlock(&s->mutex);

	if (!weak)
		return;
	obs_source_t *src = obs_weak_source_get_source(weak);
	if (src) {
		obs_source_remove_active_child(s->context, src);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
}

static void layer_release_file(struct stage *s, struct layer *l)
{
	layer_detach(s, l);
	if (l->own) {
		obs_source_release(l->own);
		l->own = NULL;
	}
	bfree(l->file);
	l->file = NULL;
}

static bool is_image_file(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext)
		return false;
	static const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga", ".psd", ".jxr"};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (astrcmpi(ext, exts[i]) == 0)
			return true;
	return false;
}

static void layer_set_file(struct stage *s, struct layer *l, int index, const char *path)
{
	if (l->file && strcmp(l->file, path) == 0)
		return;
	layer_release_file(s, l);
	if (!*path)
		return;
	l->file = bstrdup(path);

	obs_data_t *settings = obs_data_create();
	const char *id;
	if (is_image_file(path)) {
		id = "image_source";
		obs_data_set_string(settings, "file", path);
	} else {
		id = "ffmpeg_source";
		obs_data_set_string(settings, "local_file", path);
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_bool(settings, "looping", true);
		obs_data_set_bool(settings, "restart_on_activate", false);
		obs_data_set_bool(settings, "hw_decode", true);
	}
	struct dstr name = {0};
	dstr_printf(&name, "%s / layer %d", obs_source_get_name(s->context), index + 1);
	l->own = obs_source_create_private(id, name.array, settings);
	dstr_free(&name);
	obs_data_release(settings);
	if (!l->own) {
		blog_kg(LOG_WARNING, "failed to open layer file '%s'", path);
		return;
	}
	obs_source_add_active_child(s->context, l->own);
	pthread_mutex_lock(&s->mutex);
	l->weak = obs_source_get_weak_source(l->own);
	pthread_mutex_unlock(&s->mutex);
}

static void layer_try_attach(struct stage *s, struct layer *l)
{
	if (!l->name || !*l->name || l->weak)
		return;

	obs_source_t *src = obs_get_source_by_name(l->name);
	if (!src)
		return;

	bool bad = (src == s->context);
	if (!bad) {
		struct find_ctx ctx = {s->context, false};
		obs_source_enum_active_tree(src, find_in_tree, &ctx);
		bad = ctx.found;
	}
	if (bad || !obs_source_add_active_child(s->context, src)) {
		if (!l->warned)
			blog_kg(LOG_WARNING, "'%s' cannot be used as a layer of '%s' (recursion)", l->name,
				obs_source_get_name(s->context));
		l->warned = true;
		obs_source_release(src);
		return;
	}

	pthread_mutex_lock(&s->mutex);
	l->weak = obs_source_get_weak_source(src);
	pthread_mutex_unlock(&s->mutex);
	obs_source_release(src);
}

static void stage_enum_active(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct stage *s = data;
	obs_source_t *children[MAX_LAYERS];
	int n = 0;

	pthread_mutex_lock(&s->mutex);
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (!s->layers[i].weak)
			continue;
		obs_source_t *src = obs_weak_source_get_source(s->layers[i].weak);
		if (src)
			children[n++] = src;
	}
	pthread_mutex_unlock(&s->mutex);

	for (int i = 0; i < n; i++) {
		cb(s->context, children[i], param);
		obs_source_release(children[i]);
	}
}

static void on_source_rename(void *data, calldata_t *cd)
{
	struct stage *s = data;
	const char *prev = calldata_string(cd, "prev_name");
	const char *next = calldata_string(cd, "new_name");
	if (!prev || !next)
		return;

	obs_data_t *settings = obs_source_get_settings(s->context);
	for (int i = 0; i < MAX_LAYERS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "l%d_src", i + 1);
		if (strcmp(obs_data_get_string(settings, key), prev) == 0)
			obs_data_set_string(settings, key, next);
		pthread_mutex_lock(&s->mutex);
		if (s->layers[i].name && strcmp(s->layers[i].name, prev) == 0) {
			bfree(s->layers[i].name);
			s->layers[i].name = bstrdup(next);
		}
		pthread_mutex_unlock(&s->mutex);
	}
	obs_data_release(settings);
}

/* ------------------------------------------------------------------------- */
/* settings                                                                  */

static void stage_update(void *data, obs_data_t *settings)
{
	struct stage *s = data;
	char key[64];

	struct obs_video_info ovi = {0};
	obs_get_video_info(&ovi);
	s->req_cx = (uint32_t)obs_data_get_int(settings, "width");
	s->req_cy = (uint32_t)obs_data_get_int(settings, "height");
	s->cx = s->req_cx ? s->req_cx : (ovi.base_width ? ovi.base_width : 1920);
	s->cy = s->req_cy ? s->req_cy : (ovi.base_height ? ovi.base_height : 1080);
	s->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	s->base_fov = (float)obs_data_get_double(settings, "base_fov");
	s->auto_fill = obs_data_get_bool(settings, "auto_fill");
	s->layer_count = (int)obs_data_get_int(settings, "layer_count");
	s->depth_scale = (float)obs_data_get_double(settings, "depth_scale") / 100.0f;
	bool layout_changed = false;

	for (int i = 0; i < MAX_LAYERS; i++) {
		struct layer *l = &s->layers[i];
		int n = i + 1;
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)
		int type = (int)obs_data_get_int(settings, K("l%d_type"));
		const char *name = obs_data_get_string(settings, K("l%d_src"));
		const char *file = obs_data_get_string(settings, K("l%d_file"));
		bool on = obs_data_get_bool(settings, K("l%d_on")) && i < s->layer_count;
		if (!on || type != LAYER_SOURCE)
			name = "";
		if (!on || type != LAYER_FILE)
			file = "";

		if (*file) {
			if (l->name && *l->name) {
				layer_detach(s, l);
				pthread_mutex_lock(&s->mutex);
				bfree(l->name);
				l->name = bstrdup("");
				pthread_mutex_unlock(&s->mutex);
			}
			layer_set_file(s, l, i, file);
		} else {
			if (l->file)
				layer_release_file(s, l);
			if (!l->name || strcmp(l->name, name) != 0) {
				layer_detach(s, l);
				pthread_mutex_lock(&s->mutex);
				bfree(l->name);
				l->name = bstrdup(name);
				l->retry = 0.0f;
				l->warned = false;
				pthread_mutex_unlock(&s->mutex);
			}
		}

		l->on = on;
		struct ltf lv;
		lv.x = (float)obs_data_get_double(settings, K("l%d_x"));
		lv.y = (float)obs_data_get_double(settings, K("l%d_y"));
		lv.z = (float)obs_data_get_double(settings, K("l%d_z"));
		lv.scale = (float)obs_data_get_double(settings, K("l%d_scale")) / 100.0f;
		lv.yaw = (float)obs_data_get_double(settings, K("l%d_yaw"));
		lv.pitch = (float)obs_data_get_double(settings, K("l%d_pitch"));
		lv.roll = (float)obs_data_get_double(settings, K("l%d_roll"));
		lv.opacity = (float)obs_data_get_double(settings, K("l%d_opacity")) / 100.0f;
		if (!ltf_equal(&lv, &l->live)) {
			l->live = lv;
			layout_changed = true;
		}
		l->fit = (int)obs_data_get_int(settings, K("l%d_fit"));
		l->blend = (int)obs_data_get_int(settings, K("l%d_blend"));
		l->compensate = obs_data_get_bool(settings, K("l%d_comp"));
		l->dof = obs_data_get_bool(settings, K("l%d_dof"));
		l->autofill = obs_data_get_bool(settings, K("l%d_autofill"));
#undef K
	}

	/* camera */
	struct cam live;
	cam_read(&live, settings, "cam");
	for (int i = 0; i < MAX_SHOTS; i++) {
		snprintf(key, sizeof(key), "shot%d", i + 1);
		cam_read(&s->shots[i], settings, key);
		snprintf(key, sizeof(key), "shot%d_valid", i + 1);
		s->shot_valid[i] = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "shot%d_layers", i + 1);
		obs_data_array_t *arr = obs_data_get_array(settings, key);
		s->shot_has_layers[i] = arr && obs_data_array_count(arr) == MAX_LAYERS;
		for (size_t j = 0; s->shot_has_layers[i] && j < MAX_LAYERS; j++) {
			obs_data_t *o = obs_data_array_item(arr, j);
			struct ltf *t = &s->shot_layers[i][j];
			t->x = (float)obs_data_get_double(o, "x");
			t->y = (float)obs_data_get_double(o, "y");
			t->z = (float)obs_data_get_double(o, "z");
			t->scale = (float)obs_data_get_double(o, "scale");
			t->yaw = (float)obs_data_get_double(o, "yaw");
			t->pitch = (float)obs_data_get_double(o, "pitch");
			t->roll = (float)obs_data_get_double(o, "roll");
			t->opacity = (float)obs_data_get_double(o, "opacity");
			obs_data_release(o);
		}
		obs_data_array_release(arr);
	}

	s->trans_dur = (float)obs_data_get_double(settings, "trans_dur");
	s->trans_ease = (int)obs_data_get_int(settings, "trans_ease");
	s->edit_smooth = (float)obs_data_get_double(settings, "edit_smooth");

	s->hand_amount = (float)obs_data_get_double(settings, "hand_amount") / 100.0f;
	s->hand_speed = (float)obs_data_get_double(settings, "hand_speed");
	s->idle_mode = (int)obs_data_get_int(settings, "idle_mode");
	s->idle_amp = (float)obs_data_get_double(settings, "idle_amp") / 100.0f;
	s->idle_period = (float)obs_data_get_double(settings, "idle_period");
	s->beat_pulse = (float)obs_data_get_double(settings, "beat_pulse") / 100.0f;

	s->auto_mode = (int)obs_data_get_int(settings, "auto_mode");
	s->auto_enabled = s->auto_mode != AUTO_OFF;
	s->auto_interval = (float)obs_data_get_double(settings, "auto_interval");
	s->auto_beats = (int)obs_data_get_int(settings, "auto_beats");
	s->auto_random = obs_data_get_int(settings, "auto_order") == 1;
	s->bpm = (float)obs_data_get_double(settings, "bpm");

	s->impact_amount = (float)obs_data_get_double(settings, "impact_amount");
	s->impact_dur = (float)obs_data_get_double(settings, "impact_dur");
	s->flash_color = (uint32_t)obs_data_get_int(settings, "flash_color");
	s->flash_dur = (float)obs_data_get_double(settings, "flash_dur");

	s->dof_on = obs_data_get_bool(settings, "dof_on");
	s->dof_focus = (float)obs_data_get_double(settings, "dof_focus");
	s->dof_strength = (float)obs_data_get_double(settings, "dof_strength");
	s->dof_max = (float)obs_data_get_double(settings, "dof_max");

	/* live camera edits: follow with a short smoothing, switch to live view */
	if (!cam_equal(&live, &s->live) || layout_changed) {
		s->live = live;
		if (!s->drag_left && !s->drag_right && !s->drag_middle) {
			s->cur_shot = 0;
			start_transition(s, &s->live, s->edit_smooth, s->edit_smooth > 0.0f ? KG_EASE_OUT : KG_EASE_CUT);
		}
	}
}

static void stage_defaults(obs_data_t *d)
{
	char key[64];
	obs_data_set_default_int(d, "width", 0);
	obs_data_set_default_int(d, "height", 0);
	obs_data_set_default_int(d, "bg_color", 0x00000000); /* transparent: an empty stage never hides the scene */
	obs_data_set_default_double(d, "base_fov", 40.0);
	obs_data_set_default_bool(d, "auto_fill", true);
	obs_data_set_default_double(d, "depth_scale", 100.0);
	obs_data_set_default_int(d, "layer_count", 4);

	static const float def_z[MAX_LAYERS] = {2000.0f, 900.0f, 0.0f, -250.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	for (int i = 0; i < MAX_LAYERS; i++) {
		int n = i + 1;
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)
		obs_data_set_default_bool(d, K("l%d_on"), i < 4);
		obs_data_set_default_double(d, K("l%d_z"), def_z[i]);
		obs_data_set_default_double(d, K("l%d_scale"), 100.0);
		obs_data_set_default_double(d, K("l%d_opacity"), 100.0);
		obs_data_set_default_int(d, K("l%d_fit"), i == 0 ? FIT_COVER : FIT_ORIGINAL);
		obs_data_set_default_bool(d, K("l%d_comp"), true);
		obs_data_set_default_bool(d, K("l%d_dof"), true);
#undef K
	}

	cam_defaults(d, "cam");
	for (int i = 0; i < MAX_SHOTS; i++) {
		snprintf(key, sizeof(key), "shot%d", i + 1);
		cam_defaults(d, key);
	}
	obs_data_set_default_int(d, "shot_slot", 1);
	obs_data_set_default_int(d, "shot_preset_pick", SP_CLOSE);

	obs_data_set_default_double(d, "trans_dur", 1.2);
	obs_data_set_default_int(d, "trans_ease", KG_EASE_SMOOTH);
	obs_data_set_default_double(d, "edit_smooth", 0.25);

	obs_data_set_default_double(d, "hand_amount", 15.0);
	obs_data_set_default_double(d, "hand_speed", 0.35);
	obs_data_set_default_int(d, "idle_mode", IDLE_SWAY);
	obs_data_set_default_double(d, "idle_amp", 10.0);
	obs_data_set_default_double(d, "idle_period", 12.0);
	obs_data_set_default_double(d, "beat_pulse", 0.0);

	obs_data_set_default_int(d, "auto_mode", AUTO_OFF);
	obs_data_set_default_double(d, "auto_interval", 6.0);
	obs_data_set_default_int(d, "auto_beats", 8);
	obs_data_set_default_int(d, "auto_order", 0);
	obs_data_set_default_double(d, "bpm", 120.0);

	obs_data_set_default_double(d, "impact_amount", 40.0);
	obs_data_set_default_double(d, "impact_dur", 0.6);
	obs_data_set_default_int(d, "flash_color", 0xFFFFFFFF);
	obs_data_set_default_double(d, "flash_dur", 0.35);

	obs_data_set_default_bool(d, "dof_on", true);
	obs_data_set_default_double(d, "dof_focus", 0.0);
	obs_data_set_default_double(d, "dof_strength", 30.0);
	obs_data_set_default_double(d, "dof_max", 40.0);
}

/* ------------------------------------------------------------------------- */
/* properties                                                                */

static bool add_source_to_list(void *param, obs_source_t *src)
{
	obs_property_t *p = param;
	uint32_t caps = obs_source_get_output_flags(src);
	if (!(caps & OBS_SOURCE_VIDEO))
		return true;
	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_property_list_add_string(p, name, name);
	return true;
}

static bool layer_count_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int count = (int)obs_data_get_int(settings, "layer_count");
	for (int i = 0; i < MAX_LAYERS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "l%d_on", i + 1);
		obs_property_set_visible(obs_properties_get(props, key), i < count);
	}
	return true;
}

static bool layer_type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	for (int i = 0; i < MAX_LAYERS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "l%d_type", i + 1);
		bool file = obs_data_get_int(settings, key) == LAYER_FILE;
		snprintf(key, sizeof(key), "l%d_file", i + 1);
		obs_property_set_visible(obs_properties_get(props, key), file);
		snprintf(key, sizeof(key), "l%d_src", i + 1);
		obs_property_set_visible(obs_properties_get(props, key), !file);
	}
	return true;
}

static bool auto_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int mode = (int)obs_data_get_int(settings, "auto_mode");
	obs_property_set_visible(obs_properties_get(props, "auto_interval"), mode == AUTO_SECONDS);
	obs_property_set_visible(obs_properties_get(props, "auto_beats"), mode == AUTO_BEATS);
	obs_property_set_visible(obs_properties_get(props, "auto_order"), mode != AUTO_OFF);
	return true;
}

/* ------------------------------------------------------------------------- */
/* one-click setup: turn the scene items below the stage into layers         */

struct import_ctx {
	obs_source_t *self;
	obs_sceneitem_t *items[64];
	int count;
	bool found;
};

static bool import_collect(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct import_ctx *ctx = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src == ctx->self) {
		ctx->found = true;
		return false;
	}
	const char *id = obs_source_get_unversioned_id(src);
	bool video = (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) != 0;
	bool kagee = strncmp(id, "kagee_", 6) == 0;
	if (video && !kagee && obs_sceneitem_visible(item) && ctx->count < 64)
		ctx->items[ctx->count++] = item;
	return true;
}

static bool import_find_scene(void *param, obs_source_t *scene_src)
{
	struct import_ctx *ctx = param;
	obs_scene_t *scene = obs_group_or_scene_from_source(scene_src);
	if (!scene)
		return true;
	ctx->count = 0;
	ctx->found = false;
	obs_scene_enum_items(scene, import_collect, ctx);
	return !ctx->found; /* stop at the scene that contains the stage */
}

/* Returns the number of layers created. The previous layer setup is replaced and the original
 * items are hidden (the stage now draws them), so the picture looks the same at the home camera. */
static int import_scene_items(struct stage *s)
{
	struct import_ctx ctx = {.self = s->context};
	obs_enum_scenes(import_find_scene, &ctx);
	if (!ctx.found || ctx.count == 0)
		return 0;

	int first = ctx.count > MAX_LAYERS ? ctx.count - MAX_LAYERS : 0; /* keep the ones nearest the stage */
	int n = ctx.count - first;
	float W = (float)s->cx, H = (float)s->cy;
	obs_data_t *settings = obs_source_get_settings(s->context);
	char key[32];
#define K(fmt, i) (snprintf(key, sizeof(key), fmt, i), key)
	for (int k = 0; k < MAX_LAYERS; k++)
		obs_data_set_bool(settings, K("l%d_on", k + 1), false);

	for (int k = 0; k < n; k++) {
		obs_sceneitem_t *item = ctx.items[first + k];
		obs_source_t *src = obs_sceneitem_get_source(item);
		int L = k + 1;
		uint32_t w = obs_source_get_width(src), h = obs_source_get_height(src);

		struct matrix4 m;
		obs_sceneitem_get_draw_transform(item, &m);
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		struct vec3 c, centre;
		vec3_set(&c, (float)w * 0.5f - (float)crop.left, (float)h * 0.5f - (float)crop.top, 0.0f);
		vec3_transform(&centre, &c, &m);
		float sx = sqrtf(m.x.x * m.x.x + m.x.y * m.x.y);
		float sy = sqrtf(m.y.x * m.y.x + m.y.y * m.y.y);
		float roll = atan2f(m.x.y, m.x.x) * 180.0f / KG_PI;

		obs_data_set_bool(settings, K("l%d_on", L), true);
		obs_data_set_int(settings, K("l%d_type", L), LAYER_SOURCE);
		obs_data_set_string(settings, K("l%d_src", L), obs_source_get_name(src));
		obs_data_set_int(settings, K("l%d_fit", L), FIT_ORIGINAL);
		obs_data_set_double(settings, K("l%d_x", L), centre.x - W * 0.5f);
		obs_data_set_double(settings, K("l%d_y", L), centre.y - H * 0.5f);
		obs_data_set_double(settings, K("l%d_scale", L), (sx + sy) * 50.0f);
		obs_data_set_double(settings, K("l%d_roll", L), roll);
		obs_data_set_double(settings, K("l%d_yaw", L), 0.0);
		obs_data_set_double(settings, K("l%d_pitch", L), 0.0);
		/* bottom item farthest away, top item at the focus plane */
		obs_data_set_double(settings, K("l%d_z", L), (double)(n - 1 - k) * 700.0);
		obs_data_set_bool(settings, K("l%d_comp", L), true);
		obs_data_set_bool(settings, K("l%d_dof", L), true);
		obs_data_set_double(settings, K("l%d_opacity", L), 100.0);
		obs_data_set_int(settings, K("l%d_blend", L), BLEND_NORMAL);
		/* an item that covers the whole canvas is a background: never let its edges show */
		float hw = (float)w * sx * 0.5f, hh = (float)h * sy * 0.5f;
		bool covers = fabsf(roll) < 0.5f && centre.x - hw <= 1.0f && centre.x + hw >= W - 1.0f &&
			      centre.y - hh <= 1.0f && centre.y + hh >= H - 1.0f;
		obs_data_set_bool(settings, K("l%d_autofill", L), covers);
	}
#undef K
	obs_data_set_int(settings, "layer_count", n);
	obs_data_release(settings);
	obs_source_update(s->context, NULL);

	for (int k = 0; k < n; k++)
		obs_sceneitem_set_visible(ctx.items[first + k], false);
	blog_kg(LOG_INFO, "'%s': imported %d scene items as layers", obs_source_get_name(s->context), n);
	return n;
}

static bool btn_import(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return import_scene_items(data) > 0;
}

static bool btn_save_shot(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	int slot = (int)obs_data_get_int(settings, "shot_slot");
	obs_data_release(settings);
	save_live_to_slot(s, slot);
	return true;
}

static bool btn_apply_preset(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	int slot = (int)obs_data_get_int(settings, "shot_slot");
	int preset = (int)obs_data_get_int(settings, "shot_preset_pick");
	obs_data_release(settings);
	apply_preset_to_slot(s, slot, preset);
	return true;
}

static bool btn_auto_shots(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	auto_fill_shots(data);
	return true;
}

static bool btn_load_shot(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	int slot = (int)obs_data_get_int(settings, "shot_slot");
	char key[32];
	snprintf(key, sizeof(key), "shot%d_valid", slot);
	if (slot >= 1 && slot <= MAX_SHOTS && obs_data_get_bool(settings, key)) {
		struct cam c;
		snprintf(key, sizeof(key), "shot%d", slot);
		cam_read(&c, settings, key);
		cam_write(&c, settings, "cam");
		if (s->shot_has_layers[slot - 1]) {
			for (int j = 0; j < MAX_LAYERS; j++) {
				const struct ltf *t = &s->shot_layers[slot - 1][j];
				char k2[32];
#define SETL(field, val) (snprintf(k2, sizeof(k2), "l%d_" field, j + 1), obs_data_set_double(settings, k2, val))
				SETL("x", t->x);
				SETL("y", t->y);
				SETL("z", t->z);
				SETL("scale", t->scale * 100.0f);
				SETL("yaw", t->yaw);
				SETL("pitch", t->pitch);
				SETL("roll", t->roll);
				SETL("opacity", t->opacity * 100.0f);
#undef SETL
			}
		}
		obs_source_update(s->context, NULL);
	}
	obs_data_release(settings);
	return true;
}

static bool btn_clear_shot(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	int slot = (int)obs_data_get_int(settings, "shot_slot");
	char key[32];
	snprintf(key, sizeof(key), "shot%d_valid", slot);
	obs_data_set_bool(settings, key, false);
	obs_source_update(s->context, NULL);
	obs_data_release(settings);
	return true;
}

static bool btn_play_shot(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	int slot = (int)obs_data_get_int(settings, "shot_slot");
	obs_data_release(settings);
	push_action(s, ACT_SHOT_BASE + slot);
	return false;
}

static bool btn_reset_cam(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct stage *s = data;
	obs_data_t *settings = obs_source_get_settings(s->context);
	const char *fields[] = {"cam_x", "cam_y", "cam_dolly", "cam_yaw", "cam_pitch", "cam_roll", "cam_fov"};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		obs_data_erase(settings, fields[i]);
	obs_source_update(s->context, NULL);
	obs_data_release(settings);
	return true;
}

static bool btn_impact(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	push_action(data, ACT_IMPACT);
	return false;
}

static bool btn_flash(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	push_action(data, ACT_FLASH);
	return false;
}

static void add_layer_props(obs_properties_t *props, int n, obs_source_t *self)
{
	char key[32], title[64];
	obs_properties_t *g = obs_properties_create();
#define K(fmt) (snprintf(key, sizeof(key), fmt, n), key)

	obs_property_t *type =
		obs_properties_add_list(g, K("l%d_type"), T_("Layer.Type"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, T_("Layer.TypeSource"), LAYER_SOURCE);
	obs_property_list_add_int(type, T_("Layer.TypeFile"), LAYER_FILE);
	obs_property_set_modified_callback(type, layer_type_modified);

	obs_properties_add_path(g, K("l%d_file"), T_("Layer.File"), OBS_PATH_FILE,
				"Media (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.tga *.mp4 *.mov *.mkv *.webm *.avi *.m4v "
				"*.ts *.flv);;All files (*.*)",
				NULL);

	obs_property_t *list =
		obs_properties_add_list(g, K("l%d_src"), T_("Layer.Source"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, T_("None"), "");
	obs_enum_scenes(add_source_to_list, list);
	obs_enum_sources(add_source_to_list, list);
	/* never offer the stage itself */
	const char *self_name = self ? obs_source_get_name(self) : NULL;
	if (self_name) {
		for (size_t i = 0; i < obs_property_list_item_count(list); i++) {
			if (strcmp(obs_property_list_item_string(list, i), self_name) == 0) {
				obs_property_list_item_remove(list, i);
				break;
			}
		}
	}

	obs_property_t *fit =
		obs_properties_add_list(g, K("l%d_fit"), T_("Layer.Fit"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fit, T_("Fit.Original"), FIT_ORIGINAL);
	obs_property_list_add_int(fit, T_("Fit.Cover"), FIT_COVER);
	obs_property_list_add_int(fit, T_("Fit.Contain"), FIT_CONTAIN);
	obs_property_list_add_int(fit, T_("Fit.Stretch"), FIT_STRETCH);

	obs_properties_add_float_slider(g, K("l%d_z"), T_("Layer.Depth"), -1500.0, 8000.0, 10.0);
	obs_properties_add_bool(g, K("l%d_comp"), T_("Layer.Compensate"));
	obs_properties_add_float_slider(g, K("l%d_x"), T_("Layer.X"), -4000.0, 4000.0, 1.0);
	obs_properties_add_float_slider(g, K("l%d_y"), T_("Layer.Y"), -4000.0, 4000.0, 1.0);
	obs_properties_add_float_slider(g, K("l%d_scale"), T_("Layer.Scale"), 1.0, 500.0, 0.5);
	obs_properties_add_float_slider(g, K("l%d_yaw"), T_("Layer.Yaw"), -180.0, 180.0, 0.5);
	obs_properties_add_float_slider(g, K("l%d_pitch"), T_("Layer.Pitch"), -180.0, 180.0, 0.5);
	obs_properties_add_float_slider(g, K("l%d_roll"), T_("Layer.Roll"), -180.0, 180.0, 0.5);
	obs_properties_add_float_slider(g, K("l%d_opacity"), T_("Layer.Opacity"), 0.0, 100.0, 1.0);

	obs_property_t *blend =
		obs_properties_add_list(g, K("l%d_blend"), T_("Layer.Blend"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(blend, T_("Blend.Normal"), BLEND_NORMAL);
	obs_property_list_add_int(blend, T_("Blend.Add"), BLEND_ADD);
	obs_property_list_add_int(blend, T_("Blend.Screen"), BLEND_SCREEN);
	obs_property_list_add_int(blend, T_("Blend.Multiply"), BLEND_MULTIPLY);
	obs_property_list_add_int(blend, T_("Blend.Subtract"), BLEND_SUBTRACT);

	obs_properties_add_bool(g, K("l%d_dof"), T_("Layer.DOF"));
	obs_properties_add_bool(g, K("l%d_autofill"), T_("Layer.AutoFill"));

	snprintf(title, sizeof(title), "%s %d", T_("Layer"), n);
	obs_properties_add_group(props, K("l%d_on"), title, OBS_GROUP_CHECKABLE, g);
#undef K
}

static obs_properties_t *stage_properties(void *data)
{
	struct stage *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	/* stage */
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_int(g, "width", T_("Stage.Width"), 0, 8192, 1);
	obs_properties_add_int(g, "height", T_("Stage.Height"), 0, 8192, 1);
	obs_properties_add_color_alpha(g, "bg_color", T_("Stage.Background"));
	obs_properties_add_float_slider(g, "base_fov", T_("Stage.BaseFov"), 10.0, 120.0, 0.5);
	obs_properties_add_float_slider(g, "depth_scale", T_("Stage.DepthScale"), 0.0, 300.0, 1.0);
	obs_properties_add_bool(g, "auto_fill", T_("Stage.AutoFill"));
	obs_properties_add_button2(g, "btn_import", T_("Stage.Import"), btn_import, s);
	p = obs_properties_add_int_slider(g, "layer_count", T_("Stage.LayerCount"), 1, MAX_LAYERS, 1);
	obs_property_set_modified_callback(p, layer_count_modified);
	obs_properties_add_group(props, "grp_stage", T_("Group.Stage"), OBS_GROUP_NORMAL, g);

	for (int i = 0; i < MAX_LAYERS; i++)
		add_layer_props(props, i + 1, s ? s->context : NULL);

	/* camera */
	g = obs_properties_create();
	p = obs_properties_add_text(g, "cam_help", T_("Camera.Help"), OBS_TEXT_INFO);
	obs_properties_add_float_slider(g, "cam_x", T_("Camera.X"), -4000.0, 4000.0, 1.0);
	obs_properties_add_float_slider(g, "cam_y", T_("Camera.Y"), -4000.0, 4000.0, 1.0);
	obs_properties_add_float_slider(g, "cam_dolly", T_("Camera.Dolly"), -6000.0, 2000.0, 1.0);
	obs_properties_add_float_slider(g, "cam_yaw", T_("Camera.Yaw"), -90.0, 90.0, 0.1);
	obs_properties_add_float_slider(g, "cam_pitch", T_("Camera.Pitch"), -80.0, 80.0, 0.1);
	obs_properties_add_float_slider(g, "cam_roll", T_("Camera.Roll"), -180.0, 180.0, 0.1);
	obs_properties_add_float_slider(g, "cam_fov", T_("Camera.Fov"), 5.0, 120.0, 0.1);
	obs_properties_add_button2(g, "btn_reset_cam", T_("Camera.Reset"), btn_reset_cam, s);
	obs_properties_add_group(props, "grp_cam", T_("Group.Camera"), OBS_GROUP_NORMAL, g);

	/* shots */
	g = obs_properties_create();
	obs_properties_add_int_slider(g, "shot_slot", T_("Shot.Slot"), 1, MAX_SHOTS, 1);
	obs_properties_add_button2(g, "btn_save_shot", T_("Shot.Save"), btn_save_shot, s);
	obs_properties_add_button2(g, "btn_play_shot", T_("Shot.Play"), btn_play_shot, s);
	obs_properties_add_button2(g, "btn_load_shot", T_("Shot.Load"), btn_load_shot, s);
	obs_properties_add_button2(g, "btn_clear_shot", T_("Shot.Clear"), btn_clear_shot, s);
	p = obs_properties_add_list(g, "shot_preset_pick", T_("Shot.Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 1; i < SP_COUNT; i++)
		obs_property_list_add_int(p, T_(preset_keys[i]), i);
	obs_properties_add_button2(g, "btn_apply_preset", T_("Shot.ApplyPreset"), btn_apply_preset, s);
	obs_properties_add_button2(g, "btn_auto_shots", T_("Shot.AutoShots"), btn_auto_shots, s);
	obs_properties_add_float_slider(g, "trans_dur", T_("Shot.Duration"), 0.0, 10.0, 0.05);
	p = obs_properties_add_list(g, "trans_ease", T_("Shot.Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	kg_add_easing_list(p);
	obs_properties_add_float_slider(g, "edit_smooth", T_("Shot.EditSmooth"), 0.0, 2.0, 0.05);
	obs_properties_add_group(props, "grp_shots", T_("Group.Shots"), OBS_GROUP_NORMAL, g);

	/* motion */
	g = obs_properties_create();
	obs_properties_add_float_slider(g, "hand_amount", T_("Motion.Handheld"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "hand_speed", T_("Motion.HandheldSpeed"), 0.05, 3.0, 0.05);
	p = obs_properties_add_list(g, "idle_mode", T_("Motion.Idle"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Idle.None"), IDLE_NONE);
	obs_property_list_add_int(p, T_("Idle.Sway"), IDLE_SWAY);
	obs_property_list_add_int(p, T_("Idle.Orbit"), IDLE_ORBIT);
	obs_property_list_add_int(p, T_("Idle.Breathe"), IDLE_BREATHE);
	obs_property_list_add_int(p, T_("Idle.Drift"), IDLE_DRIFT);
	obs_properties_add_float_slider(g, "idle_amp", T_("Motion.IdleAmount"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, "idle_period", T_("Motion.IdlePeriod"), 1.0, 60.0, 0.5);
	obs_properties_add_group(props, "grp_motion", T_("Group.Motion"), OBS_GROUP_NORMAL, g);

	/* auto switch + bpm */
	g = obs_properties_create();
	p = obs_properties_add_list(g, "auto_mode", T_("Auto.Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Auto.Off"), AUTO_OFF);
	obs_property_list_add_int(p, T_("Auto.Seconds"), AUTO_SECONDS);
	obs_property_list_add_int(p, T_("Auto.Beats"), AUTO_BEATS);
	obs_property_set_modified_callback(p, auto_mode_modified);
	obs_properties_add_float_slider(g, "auto_interval", T_("Auto.Interval"), 0.5, 120.0, 0.5);
	obs_properties_add_int_slider(g, "auto_beats", T_("Auto.EveryBeats"), 1, 64, 1);
	p = obs_properties_add_list(g, "auto_order", T_("Auto.Order"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Auto.Sequential"), 0);
	obs_property_list_add_int(p, T_("Auto.Random"), 1);
	obs_properties_add_float_slider(g, "bpm", T_("Beat.BPM"), 30.0, 300.0, 0.1);
	obs_properties_add_float_slider(g, "beat_pulse", T_("Beat.Pulse"), 0.0, 100.0, 1.0);
	obs_properties_add_group(props, "grp_auto", T_("Group.Auto"), OBS_GROUP_NORMAL, g);

	/* triggers */
	g = obs_properties_create();
	obs_properties_add_float_slider(g, "impact_amount", T_("Fx.ImpactAmount"), 0.0, 200.0, 1.0);
	obs_properties_add_float_slider(g, "impact_dur", T_("Fx.ImpactDuration"), 0.05, 3.0, 0.05);
	obs_properties_add_button2(g, "btn_impact", T_("Fx.ImpactTest"), btn_impact, s);
	obs_properties_add_color_alpha(g, "flash_color", T_("Fx.FlashColor"));
	obs_properties_add_float_slider(g, "flash_dur", T_("Fx.FlashDuration"), 0.05, 3.0, 0.05);
	obs_properties_add_button2(g, "btn_flash", T_("Fx.FlashTest"), btn_flash, s);
	obs_properties_add_group(props, "grp_fx", T_("Group.Triggers"), OBS_GROUP_NORMAL, g);

	/* dof */
	g = obs_properties_create();
	obs_properties_add_float_slider(g, "dof_focus", T_("Dof.Focus"), -3000.0, 8000.0, 10.0);
	obs_properties_add_float_slider(g, "dof_strength", T_("Dof.Strength"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(g, "dof_max", T_("Dof.Max"), 0.0, 128.0, 1.0);
	obs_properties_add_group(props, "dof_on", T_("Group.Dof"), OBS_GROUP_CHECKABLE, g);

	return props;
}

/* ------------------------------------------------------------------------- */
/* hotkeys                                                                   */

struct hotkey_def {
	const char *name;
	const char *text;
	int action;
};

static void record_tap(struct stage *s)
{
	pthread_mutex_lock(&s->mutex);
	uint64_t now = os_gettime_ns();
	if (s->tap_count > 0 && now - s->taps[(s->tap_count - 1) % 8] > 2000000000ULL)
		s->tap_count = 0;
	s->taps[s->tap_count % 8] = now;
	s->tap_count++;
	pthread_mutex_unlock(&s->mutex);
}

static void trigger_action(struct stage *s, int act)
{
	if (act == ACT_TAP)
		record_tap(s);
	push_action(s, act);
}

static void hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	struct stage *s = data;
	if (!pressed)
		return;
	for (int i = 0; i < s->hotkey_count; i++) {
		if (s->hotkeys[i] != id)
			continue;
		/* action is encoded in registration order, see register_hotkeys */
		int act;
		if (i <= MAX_SHOTS)
			act = ACT_SHOT_BASE + i; /* 0 = live */
		else
			act = ACT_NEXT + (i - MAX_SHOTS - 1);
		trigger_action(s, act);
		return;
	}
}

/* ------------------------------------------------------------------------- */
/* procedures: used by the Kagee dock (and callable by other plugins)       */

static void proc_shot(void *data, calldata_t *cd)
{
	long long i = calldata_int(cd, "index");
	if (i >= 0 && i <= MAX_SHOTS)
		trigger_action(data, ACT_SHOT_BASE + (int)i);
}

static void proc_save_shot(void *data, calldata_t *cd)
{
	save_live_to_slot(data, (int)calldata_int(cd, "index"));
}

static void proc_action(void *data, calldata_t *cd)
{
	static const struct {
		const char *name;
		int act;
	} map[] = {{"next", ACT_NEXT},   {"prev", ACT_PREV}, {"random", ACT_RANDOM},     {"impact", ACT_IMPACT},
		   {"flash", ACT_FLASH}, {"tap", ACT_TAP},   {"auto", ACT_AUTO_TOGGLE}};
	const char *name = calldata_string(cd, "name");
	for (size_t i = 0; name && i < sizeof(map) / sizeof(map[0]); i++)
		if (strcmp(name, map[i].name) == 0)
			trigger_action(data, map[i].act);
}

static void proc_state(void *data, calldata_t *cd)
{
	struct stage *s = data;
	calldata_set_int(cd, "shot", s->cur_shot);
	calldata_set_bool(cd, "auto", s->auto_enabled);
	calldata_set_float(cd, "bpm", s->bpm);
}

static void proc_import(void *data, calldata_t *cd)
{
	calldata_set_int(cd, "count", import_scene_items(data));
}

static void proc_layers(void *data, calldata_t *cd)
{
	struct stage *s = data;
	int attached = 0, configured = 0;
	pthread_mutex_lock(&s->mutex);
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (s->layers[i].on && ((s->layers[i].name && *s->layers[i].name) || s->layers[i].file))
			configured++;
		if (s->layers[i].on && s->layers[i].weak)
			attached++;
	}
	pthread_mutex_unlock(&s->mutex);
	calldata_set_int(cd, "configured", configured);
	calldata_set_int(cd, "attached", attached);
}

static void proc_apply_preset(void *data, calldata_t *cd)
{
	apply_preset_to_slot(data, (int)calldata_int(cd, "index"), (int)calldata_int(cd, "preset"));
}

static void proc_auto_shots(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto_fill_shots(data);
}

static void register_procs(struct stage *s)
{
	proc_handler_t *ph = obs_source_get_proc_handler(s->context);
	proc_handler_add(ph, "void kagee_apply_preset(in int index, in int preset)", proc_apply_preset, s);
	proc_handler_add(ph, "void kagee_auto_shots()", proc_auto_shots, s);
	proc_handler_add(ph, "void kagee_import_scene(out int count)", proc_import, s);
	proc_handler_add(ph, "void kagee_layers(out int configured, out int attached)", proc_layers, s);
	proc_handler_add(ph, "void kagee_shot(in int index)", proc_shot, s);
	proc_handler_add(ph, "void kagee_save_shot(in int index)", proc_save_shot, s);
	proc_handler_add(ph, "void kagee_action(in string name)", proc_action, s);
	proc_handler_add(ph, "void kagee_state(out int shot, out bool auto, out float bpm)", proc_state, s);
}

static void register_hotkeys(struct stage *s)
{
	char name[64], text[128];
	s->hotkey_count = 0;
	for (int i = 0; i <= MAX_SHOTS; i++) {
		if (i == 0) {
			snprintf(name, sizeof(name), "Kagee.Live");
			snprintf(text, sizeof(text), "%s", T_("Hotkey.Live"));
		} else {
			snprintf(name, sizeof(name), "Kagee.Shot%d", i);
			snprintf(text, sizeof(text), "%s %d", T_("Hotkey.Shot"), i);
		}
		s->hotkeys[s->hotkey_count++] = obs_hotkey_register_source(s->context, name, text, hotkey_pressed, s);
	}
	static const struct hotkey_def extra[] = {
		{"Kagee.Next", "Hotkey.Next", ACT_NEXT},
		{"Kagee.Prev", "Hotkey.Prev", ACT_PREV},
		{"Kagee.Random", "Hotkey.Random", ACT_RANDOM},
		{"Kagee.Impact", "Hotkey.Impact", ACT_IMPACT},
		{"Kagee.Flash", "Hotkey.Flash", ACT_FLASH},
		{"Kagee.Tap", "Hotkey.Tap", ACT_TAP},
		{"Kagee.AutoToggle", "Hotkey.AutoToggle", ACT_AUTO_TOGGLE},
	};
	for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
		s->hotkeys[s->hotkey_count++] =
			obs_hotkey_register_source(s->context, extra[i].name, T_(extra[i].text), hotkey_pressed, s);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */

static void *stage_create(obs_data_t *settings, obs_source_t *source)
{
	struct stage *s = bzalloc(sizeof(*s));
	s->context = source;
	s->rng = (uint32_t)os_gettime_ns() | 1;
	pthread_mutex_init_value(&s->mutex);
	if (pthread_mutex_init(&s->mutex, NULL) != 0) {
		bfree(s);
		return NULL;
	}

	obs_enter_graphics();
	s->effect = kg_load_effect("effects/stage.effect");
	s->stage_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (int i = 0; i < MAX_LAYERS; i++)
		s->layers[i].tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	kg_blur_init(&s->blur);
	obs_leave_graphics();

	stage_update(s, settings);
	s->base = s->live;
	s->to = s->live;
	for (int i = 0; i < MAX_LAYERS; i++)
		ltf_set(&s->layers[i], &s->layers[i].live);

	register_hotkeys(s);
	register_procs(s);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, s);
	return s;
}

static void stage_destroy(void *data)
{
	struct stage *s = data;
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, s);

	for (int i = 0; i < MAX_LAYERS; i++) {
		layer_release_file(s, &s->layers[i]);
		layer_detach(s, &s->layers[i]);
		bfree(s->layers[i].name);
	}

	obs_enter_graphics();
	gs_texrender_destroy(s->stage_tr);
	for (int i = 0; i < MAX_LAYERS; i++)
		gs_texrender_destroy(s->layers[i].tr);
	kg_blur_free(&s->blur);
	obs_leave_graphics();

	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static uint32_t stage_width(void *data)
{
	return ((struct stage *)data)->cx;
}

static uint32_t stage_height(void *data)
{
	return ((struct stage *)data)->cy;
}

/* ------------------------------------------------------------------------- */
/* tick: animation state                                                     */

static void write_live_camera(struct stage *s)
{
	obs_data_t *d = obs_data_create();
	cam_write(&s->live, d, "cam");
	obs_source_update(s->context, d);
	obs_data_release(d);
}

static void process_actions(struct stage *s)
{
	int acts[16];
	int n;
	pthread_mutex_lock(&s->mutex);
	n = s->pending_count;
	memcpy(acts, s->pending, sizeof(int) * (size_t)n);
	s->pending_count = 0;
	pthread_mutex_unlock(&s->mutex);

	for (int i = 0; i < n; i++) {
		int a = acts[i];
		if (a >= ACT_SHOT_BASE && a <= ACT_SHOT_BASE + MAX_SHOTS) {
			go_to_shot(s, a - ACT_SHOT_BASE);
			s->auto_timer = 0.0f;
		} else if (a == ACT_NEXT) {
			step_shot(s, 1, false);
		} else if (a == ACT_PREV) {
			step_shot(s, -1, false);
		} else if (a == ACT_RANDOM) {
			step_shot(s, 1, true);
		} else if (a == ACT_IMPACT) {
			s->impact = 1.0f;
		} else if (a == ACT_FLASH) {
			s->flash = 1.0f;
		} else if (a == ACT_AUTO_TOGGLE) {
			s->auto_enabled = !s->auto_enabled && s->auto_mode != AUTO_OFF;
			s->auto_timer = 0.0f;
		} else if (a == ACT_TAP) {
			pthread_mutex_lock(&s->mutex);
			int count = s->tap_count < 8 ? s->tap_count : 8;
			if (count >= 2) {
				uint64_t first = s->taps[(s->tap_count - count) % 8];
				uint64_t last = s->taps[(s->tap_count - 1) % 8];
				double avg = (double)(last - first) / 1e9 / (double)(count - 1);
				if (avg > 0.15 && avg < 2.5)
					s->bpm = (float)(60.0 / avg);
			}
			pthread_mutex_unlock(&s->mutex);
			s->beat_origin = s->time;
			s->last_beat = 0;
			obs_data_t *settings = obs_source_get_settings(s->context);
			obs_data_set_double(settings, "bpm", s->bpm);
			obs_data_release(settings);
		}
	}
}

static void stage_tick(void *data, float seconds)
{
	struct stage *s = data;
	s->time += seconds;

	/* keep canvas-sized stages in sync with canvas resolution changes */
	if (!s->req_cx || !s->req_cy) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			if (!s->req_cx)
				s->cx = ovi.base_width;
			if (!s->req_cy)
				s->cy = ovi.base_height;
		}
	}

	/* (re)attach layer sources lazily: sources may be created after the stage on load */
	for (int i = 0; i < MAX_LAYERS; i++) {
		struct layer *l = &s->layers[i];
		if (l->weak) {
			obs_source_t *src = obs_weak_source_get_source(l->weak);
			bool gone = !src || obs_source_removed(src);
			obs_source_release(src);
			if (gone)
				layer_detach(s, l);
		}
		if (!l->weak && l->name && *l->name) {
			l->retry -= seconds;
			if (l->retry <= 0.0f) {
				l->retry = RETRY_INTERVAL;
				layer_try_attach(s, l);
			}
		}
	}

	process_actions(s);

	if (s->cam_dirty && !s->drag_left && !s->drag_right && !s->drag_middle) {
		s->cam_dirty = false;
		write_live_camera(s);
	}

	/* camera transition */
	if (s->anim_dur > 0.0f && s->anim_t < 1.0f) {
		s->anim_t += seconds / s->anim_dur;
		if (s->anim_t >= 1.0f) {
			s->anim_t = 1.0f;
			s->base = s->to;
			for (int i = 0; i < MAX_LAYERS; i++)
				ltf_set(&s->layers[i], &s->lto[i]);
		} else {
			float e = kg_ease(s->anim_ease, s->anim_t);
			cam_lerp(&s->base, &s->from, &s->to, e);
			for (int i = 0; i < MAX_LAYERS; i++) {
				struct ltf t;
				ltf_lerp(&t, &s->lfrom[i], &s->lto[i], e);
				ltf_set(&s->layers[i], &t);
			}
		}
	}

	/* beats */
	float beat_len = 60.0f / kg_clampf(s->bpm, 1.0f, 999.0f);
	long long beat = (long long)floor((s->time - s->beat_origin) / beat_len);
	bool new_beat = beat != s->last_beat;
	s->last_beat = beat;

	/* auto switching */
	if (s->auto_enabled) {
		if (s->auto_mode == AUTO_SECONDS) {
			s->auto_timer += seconds;
			if (s->auto_timer >= s->auto_interval) {
				s->auto_timer = 0.0f;
				step_shot(s, 1, s->auto_random);
			}
		} else if (s->auto_mode == AUTO_BEATS && new_beat && s->auto_beats > 0 && beat % s->auto_beats == 0) {
			step_shot(s, 1, s->auto_random);
		}
	}

	if (s->impact > 0.0f)
		s->impact = fmaxf(0.0f, s->impact - seconds / fmaxf(s->impact_dur, 0.01f));
	if (s->flash > 0.0f)
		s->flash = fmaxf(0.0f, s->flash - seconds / fmaxf(s->flash_dur, 0.01f));
}

/* final camera = base + procedural motion */
static void compute_camera(struct stage *s, struct cam *c)
{
	*c = s->base;
	float t = (float)s->time;
	float scale = (float)s->cy / 1080.0f;

	if (s->hand_amount > 0.0f) {
		float k = t * s->hand_speed;
		float a = s->hand_amount;
		c->x += kg_fbm1(k + 0.0f, 3) * 14.0f * a * scale;
		c->y += kg_fbm1(k + 37.1f, 3) * 10.0f * a * scale;
		c->yaw += kg_fbm1(k + 71.3f, 3) * 1.2f * a;
		c->pitch += kg_fbm1(k + 113.7f, 3) * 0.8f * a;
		c->roll += kg_fbm1(k + 151.9f, 3) * 0.9f * a;
	}

	if (s->idle_mode != IDLE_NONE && s->idle_amp > 0.0f) {
		float ph = 2.0f * KG_PI * t / fmaxf(s->idle_period, 0.1f);
		float a = s->idle_amp;
		switch (s->idle_mode) {
		case IDLE_SWAY:
			c->yaw += sinf(ph) * 12.0f * a;
			break;
		case IDLE_ORBIT:
			c->yaw += sinf(ph) * 20.0f * a;
			c->pitch += sinf(ph * 2.0f) * 5.0f * a;
			break;
		case IDLE_BREATHE:
			c->dolly += (0.5f - 0.5f * cosf(ph)) * ref_distance(s) * 0.25f * a;
			break;
		case IDLE_DRIFT:
			c->x += sinf(ph) * 200.0f * a * scale;
			c->y += sinf(ph * 0.5f + 1.3f) * 60.0f * a * scale;
			break;
		}
	}

	if (s->beat_pulse > 0.0f) {
		float beat_len = 60.0f / kg_clampf(s->bpm, 1.0f, 999.0f);
		float since = fmodf((float)(s->time - s->beat_origin), beat_len);
		if (since < 0.0f)
			since += beat_len;
		c->fov *= 1.0f - s->beat_pulse * 0.12f * expf(-since * 9.0f);
	}

	if (s->impact > 0.0f) {
		float e = s->impact * s->impact;
		float k = t * 28.0f;
		c->x += kg_noise1(k) * s->impact_amount * e * scale;
		c->y += kg_noise1(k + 50.0f) * s->impact_amount * e * scale;
		c->roll += kg_noise1(k + 100.0f) * s->impact_amount * 0.05f * e;
	}
}

/* ------------------------------------------------------------------------- */
/* render                                                                    */

struct blend_params {
	enum gs_blend_type src_c, dst_c, src_a, dst_a;
	enum gs_blend_op_type op;
};

static const struct blend_params blend_table[] = {
	[BLEND_NORMAL] = {GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	[BLEND_ADD] = {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ZERO, GS_BLEND_ONE, GS_BLEND_OP_ADD},
	[BLEND_SCREEN] = {GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	[BLEND_MULTIPLY] = {GS_BLEND_DSTCOLOR, GS_BLEND_INVSRCALPHA, GS_BLEND_DSTALPHA, GS_BLEND_INVSRCALPHA,
			    GS_BLEND_OP_ADD},
	[BLEND_SUBTRACT] = {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ZERO, GS_BLEND_ONE, GS_BLEND_OP_REVERSE_SUBTRACT},
};

static void build_view(const struct stage *s, const struct cam *c, struct matrix4 *view)
{
	float dist = fmaxf(ref_distance(s) - c->dolly, 1.0f);
	struct axisang aa;
	struct vec3 t;

	matrix4_identity(view);
	axisang_set(&aa, 0.0f, 0.0f, 1.0f, KG_DEG2RAD(-c->roll));
	matrix4_rotate_aa_i(view, &aa, view);
	vec3_set(&t, 0.0f, 0.0f, dist);
	matrix4_translate3v_i(view, &t, view);
	axisang_set(&aa, 1.0f, 0.0f, 0.0f, KG_DEG2RAD(c->pitch));
	matrix4_rotate_aa_i(view, &aa, view);
	axisang_set(&aa, 0.0f, 1.0f, 0.0f, KG_DEG2RAD(c->yaw));
	matrix4_rotate_aa_i(view, &aa, view);
	vec3_set(&t, -c->x, -c->y, 0.0f);
	matrix4_translate3v_i(view, &t, view);
}

#define LZ(s, l) ((l)->z * (s)->depth_scale)

static void layer_world_size(const struct stage *s, const struct layer *l, uint32_t cw, uint32_t ch, float *w,
			     float *h)
{
	float W = (float)s->cx, H = (float)s->cy;
	float fw = (float)cw, fh = (float)ch;
	switch (l->fit) {
	case FIT_COVER: {
		float k = fmaxf(W / fw, H / fh);
		fw *= k;
		fh *= k;
		break;
	}
	case FIT_CONTAIN: {
		float k = fminf(W / fw, H / fh);
		fw *= k;
		fh *= k;
		break;
	}
	case FIT_STRETCH:
		fw = W;
		fh = H;
		break;
	default:
		break;
	}
	float k = l->scale;
	if (l->compensate) {
		float d = ref_distance(s);
		k *= fmaxf((d + LZ(s, l)) / d, 0.01f);
	}
	*w = fw * k;
	*h = fh * k;
}

/* With "keep apparent size/position", a layer pushed back in depth is scaled AND moved outwards so
 * it looks identical from the home camera; only camera moves then reveal the parallax. */
static void layer_world_pos(const struct stage *s, const struct layer *l, float *x, float *y)
{
	float k = 1.0f;
	if (l->compensate) {
		float d = ref_distance(s);
		k = fmaxf((d + LZ(s, l)) / d, 0.01f);
	}
	*x = l->x * k;
	*y = l->y * k;
}

/* For "fill stage" layers facing the camera: how much the plane must grow so the camera
 * frustum never sees past its edges (camera orbit / dolly / pan would otherwise reveal them). */
static float cover_fill_scale(const struct stage *s, const struct layer *l, const struct matrix4 *inv_view,
			      const struct cam *c, float ww, float wh)
{
	if (!s->auto_fill || (l->fit != FIT_COVER && !l->autofill) || l->yaw != 0.0f || l->pitch != 0.0f ||
	    l->roll != 0.0f)
		return 1.0f;

	float ty = tanf(KG_DEG2RAD(kg_clampf(c->fov, 1.0f, 170.0f)) * 0.5f);
	float tx = ty * (float)s->cx / (float)s->cy;
	struct vec4 origin, o;
	vec4_set(&origin, 0.0f, 0.0f, 0.0f, 1.0f);
	vec4_transform(&o, &origin, inv_view);

	float need_x = 0.0f, need_y = 0.0f, lx, ly;
	layer_world_pos(s, l, &lx, &ly);
	for (int i = 0; i < 4; i++) {
		struct vec4 dir, d;
		vec4_set(&dir, (i & 1) ? tx : -tx, (i & 2) ? ty : -ty, 1.0f, 0.0f);
		vec4_transform(&d, &dir, inv_view);
		if (d.z <= 0.001f)
			return 3.0f; /* looking away from the plane: grow as much as sensible */
		float t = (LZ(s, l) - o.z) / d.z;
		if (t <= 0.0f)
			return 1.0f; /* plane is behind the camera */
		need_x = fmaxf(need_x, fabsf(o.x + d.x * t - lx));
		need_y = fmaxf(need_y, fabsf(o.y + d.y * t - ly));
	}
	float k = fmaxf(need_x / (ww * 0.5f), need_y / (wh * 0.5f)) * 1.01f;
	return kg_clampf(k, 1.0f, 3.0f);
}

static gs_texture_t *render_child(struct layer *l, obs_source_t *src, uint32_t cw, uint32_t ch)
{
	gs_texrender_reset(l->tr);
	if (!gs_texrender_begin(l->tr, cw, ch))
		return NULL;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(src);
	gs_blend_state_pop();
	gs_texrender_end(l->tr);
	return gs_texrender_get_texture(l->tr);
}

static int cmp_depth(const void *a, const void *b)
{
	const struct layer *la = *(const struct layer *const *)a;
	const struct layer *lb = *(const struct layer *const *)b;
	/* far first */
	return (la->view_z < lb->view_z) - (la->view_z > lb->view_z);
}

static void stage_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct stage *s = data;
	if (!s->effect || !s->cx || !s->cy)
		return;

	struct cam c;
	compute_camera(s, &c);
	struct matrix4 view;
	build_view(s, &c, &view);
	float focus_z = fmaxf(ref_distance(s) - c.dolly, 1.0f) + s->dof_focus;

	/* libobs negates the view matrix Z column before projecting (it assumes a right-handed,
	 * -Z forward camera). Our camera looks down +Z, so pre-negate to cancel that out. */
	struct matrix4 inv_view;
	matrix4_inv(&inv_view, &view);
	struct matrix4 gpu_view = view;
	gpu_view.x.z = -gpu_view.x.z;
	gpu_view.y.z = -gpu_view.y.z;
	gpu_view.z.z = -gpu_view.z.z;
	gpu_view.t.z = -gpu_view.t.z;

	/* resolve children + sort by view depth */
	struct layer *order[MAX_LAYERS];
	obs_source_t *srcs[MAX_LAYERS];
	int n = 0;
	pthread_mutex_lock(&s->mutex);
	for (int i = 0; i < MAX_LAYERS; i++) {
		struct layer *l = &s->layers[i];
		if (!l->on || !l->weak || l->opacity <= 0.0f)
			continue;
		obs_source_t *src = obs_weak_source_get_source(l->weak);
		if (!src)
			continue;
		struct vec3 center, vc;
		float lx, ly;
		layer_world_pos(s, l, &lx, &ly);
		vec3_set(&center, lx, ly, LZ(s, l));
		vec3_transform(&vc, &center, &view);
		l->view_z = vc.z;
		order[n] = l;
		srcs[n] = src;
		n++;
	}
	pthread_mutex_unlock(&s->mutex);

	/* sort (keep srcs aligned) */
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && cmp_depth(&order[j - 1], &order[j]) > 0; j--) {
			struct layer *tl = order[j];
			order[j] = order[j - 1];
			order[j - 1] = tl;
			obs_source_t *ts = srcs[j];
			srcs[j] = srcs[j - 1];
			srcs[j - 1] = ts;
		}
	}

	gs_texrender_reset(s->stage_tr);
	if (gs_texrender_begin(s->stage_tr, s->cx, s->cy)) {
		struct vec4 bg;
		vec4_from_rgba(&bg, s->bg_color);
		bg.x *= bg.w;
		bg.y *= bg.w;
		bg.z *= bg.w;
		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

		enum gs_cull_mode prev_cull = gs_get_cull_mode();
		gs_set_cull_mode(GS_NEITHER);

		gs_eparam_t *p_image = gs_effect_get_param_by_name(s->effect, "image");
		gs_eparam_t *p_opacity = gs_effect_get_param_by_name(s->effect, "opacity");

		for (int i = 0; i < n; i++) {
			struct layer *l = order[i];
			uint32_t cw = obs_source_get_width(srcs[i]);
			uint32_t ch = obs_source_get_height(srcs[i]);
			if (!cw || !ch || l->view_z <= 1.0f)
				continue;

			gs_texture_t *tex = render_child(l, srcs[i], cw, ch);
			if (!tex)
				continue;
			float ww, wh;
			layer_world_size(s, l, cw, ch, &ww, &wh);
			float fill = cover_fill_scale(s, l, &inv_view, &c, ww, wh);
			ww *= fill;
			wh *= fill;

			if (s->dof_on && l->dof && s->dof_strength > 0.0f) {
				float r = fabsf(l->view_z - focus_z) * s->dof_strength * 0.0015f;
				/* blur radius is in texture pixels; normalise to on-screen size */
				float screen_per_tex = (ww / (float)cw) * (ref_distance(s) / l->view_z);
				if (screen_per_tex > 0.01f)
					r /= screen_per_tex;
				r = fminf(r, s->dof_max / fmaxf(screen_per_tex, 0.01f));
				tex = kg_blur_apply(&s->blur, tex, cw, ch, r, NULL);
			}

			gs_projection_push();
			gs_perspective(kg_clampf(c.fov, 1.0f, 170.0f), (float)s->cx / (float)s->cy, 1.0f, 100000.0f);
			gs_matrix_push();
			gs_matrix_set(&gpu_view);

			float lx, ly;
			layer_world_pos(s, l, &lx, &ly);
			gs_matrix_translate3f(lx, ly, LZ(s, l));
			gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, KG_DEG2RAD(-l->yaw));
			gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f, KG_DEG2RAD(l->pitch));
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, KG_DEG2RAD(l->roll));
			gs_matrix_scale3f(ww / (float)cw, wh / (float)ch, 1.0f);
			gs_matrix_translate3f(-(float)cw * 0.5f, -(float)ch * 0.5f, 0.0f);

			const struct blend_params *bp = &blend_table[l->blend >= 0 && l->blend <= BLEND_SUBTRACT
									     ? l->blend
									     : BLEND_NORMAL];
			gs_blend_state_push();
			gs_blend_function_separate(bp->src_c, bp->dst_c, bp->src_a, bp->dst_a);
			gs_blend_op(bp->op);

			gs_effect_set_texture(p_image, tex);
			gs_effect_set_float(p_opacity, kg_clampf(l->opacity, 0.0f, 1.0f));
			while (gs_effect_loop(s->effect, "Layer"))
				gs_draw_sprite(tex, 0, cw, ch);

			gs_blend_op(GS_BLEND_OP_ADD);
			gs_blend_state_pop();
			gs_matrix_pop();
			gs_projection_pop();
		}

		gs_set_cull_mode(prev_cull);

		/* flash overlay */
		if (s->flash > 0.0f) {
			gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
			struct vec4 fc;
			vec4_from_rgba(&fc, s->flash_color);
			float a = fc.w * s->flash * s->flash;
			vec4_set(&fc, fc.x * a, fc.y * a, fc.z * a, a);
			gs_ortho(0.0f, (float)s->cx, 0.0f, (float)s->cy, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &fc);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(NULL, 0, s->cx, s->cy);
			gs_blend_state_pop();
		}

		gs_texrender_end(s->stage_tr);
	}

	for (int i = 0; i < n; i++)
		obs_source_release(srcs[i]);

	gs_texture_t *out = gs_texrender_get_texture(s->stage_tr);
	if (out)
		kg_draw_premultiplied(out, s->cx, s->cy);
}

/* ------------------------------------------------------------------------- */
/* interaction: drag the camera in the Interact window                       */

static void stage_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			      uint32_t click_count)
{
	struct stage *s = data;
	bool down = !mouse_up;
	if (type == MOUSE_LEFT)
		s->drag_left = down;
	else if (type == MOUSE_RIGHT)
		s->drag_right = down;
	else if (type == MOUSE_MIDDLE)
		s->drag_middle = down;
	s->last_mx = event->x;
	s->last_my = event->y;

	/* double click: reset the live camera to home */
	if (down && type == MOUSE_LEFT && click_count >= 2) {
		struct cam home = {0};
		home.fov = s->live.fov;
		s->live = home;
		s->cur_shot = 0;
		start_transition(s, &s->live, 0.4f, KG_EASE_SMOOTH);
		s->cam_dirty = true;
	}
}

static void stage_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct stage *s = data;
	if (mouse_leave) {
		s->drag_left = s->drag_right = s->drag_middle = false;
		return;
	}
	float dx = (float)(event->x - s->last_mx);
	float dy = (float)(event->y - s->last_my);
	s->last_mx = event->x;
	s->last_my = event->y;
	if (!s->drag_left && !s->drag_right && !s->drag_middle)
		return;

	bool shift = (event->modifiers & INTERACT_SHIFT_KEY) != 0;
	bool ctrl = (event->modifiers & INTERACT_CONTROL_KEY) != 0;
	float fine = shift ? 0.2f : 1.0f;
	float d = ref_distance(s);
	float dist = fmaxf(d - s->live.dolly, 1.0f);

	if (s->drag_left && !ctrl) {
		/* pan: keep the target plane moving with the mouse */
		float k = (dist / d) * (s->live.fov / fmaxf(s->base_fov, 1.0f)) * fine;
		s->live.x -= dx * k;
		s->live.y -= dy * k;
	} else if (s->drag_right) {
		/* "grab the world": dragging right turns the scene right (camera moves left) */
		s->live.yaw = kg_clampf(s->live.yaw - dx * 0.15f * fine, -90.0f, 90.0f);
		s->live.pitch = kg_clampf(s->live.pitch + dy * 0.15f * fine, -80.0f, 80.0f);
	} else if (s->drag_middle || (s->drag_left && ctrl)) {
		s->live.roll = kg_clampf(s->live.roll + dx * 0.2f * fine, -180.0f, 180.0f);
	}

	s->cur_shot = 0;
	start_transition(s, &s->live, 0.0f, KG_EASE_CUT);
	s->cam_dirty = true;
}

static void stage_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	UNUSED_PARAMETER(x_delta);
	struct stage *s = data;
	float steps = (float)y_delta / 120.0f;
	if (event->modifiers & INTERACT_SHIFT_KEY) {
		s->live.fov = kg_clampf(s->live.fov - steps * 2.0f, 5.0f, 120.0f);
	} else {
		float d = ref_distance(s);
		s->live.dolly = kg_clampf(s->live.dolly + steps * d * 0.05f, -6000.0f, d - 10.0f);
	}
	s->cur_shot = 0;
	start_transition(s, &s->live, 0.12f, KG_EASE_OUT);
	s->cam_dirty = true;
}

/* ------------------------------------------------------------------------- */

struct obs_source_info kagee_stage_source = {
	.id = "kagee_stage",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = stage_get_name,
	.create = stage_create,
	.destroy = stage_destroy,
	.update = stage_update,
	.get_defaults = stage_defaults,
	.get_properties = stage_properties,
	.get_width = stage_width,
	.get_height = stage_height,
	.video_tick = stage_tick,
	.video_render = stage_render,
	.enum_active_sources = stage_enum_active,
	.mouse_click = stage_mouse_click,
	.mouse_move = stage_mouse_move,
	.mouse_wheel = stage_mouse_wheel,
};
