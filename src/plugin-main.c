#include "common.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kagee", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Kagee: 3D multiplane stage, virtual camera work and cinematic screen effects";
}

extern struct obs_source_info kagee_stage_source;
extern struct obs_source_info kagee_adjust_source;
extern struct obs_source_info kagee_lens_filter;
extern struct obs_source_info kagee_retro_filter;
extern struct obs_source_info kagee_glow_filter;
extern struct obs_source_info kagee_blur_filter;
extern struct obs_source_info kagee_glitch_filter;
extern struct obs_source_info kagee_trail_filter;
extern struct obs_source_info kagee_grade_filter;
extern struct obs_source_info kagee_light_filter;

void kagee_dock_load(void);
void kagee_dock_unload(void);

bool obs_module_load(void)
{
	obs_register_source(&kagee_stage_source);
	obs_register_source(&kagee_adjust_source);
	obs_register_source(&kagee_lens_filter);
	obs_register_source(&kagee_retro_filter);
	obs_register_source(&kagee_glow_filter);
	obs_register_source(&kagee_blur_filter);
	obs_register_source(&kagee_glitch_filter);
	obs_register_source(&kagee_trail_filter);
	obs_register_source(&kagee_grade_filter);
	obs_register_source(&kagee_light_filter);
	kagee_dock_load();
	blog_kg(LOG_INFO, "loaded (version %s)", KAGEE_VERSION);
	return true;
}

void obs_module_unload(void)
{
	kagee_dock_unload();
}

