/*
 * macOS smoke test: loads kagee.plugin into libobs (taken from an OBS Studio release) the same way
 * OBS does, initialises it and checks that every source/filter is registered and localised.
 * Missing libobs / frontend symbols, wrong architecture, bad bundle layout or missing locale data
 * all fail here. Built and run by tools/macos-smoke-test.sh.
 */
#include <obs.h>
#include <stdio.h>
#include <string.h>

static const char *ids[] = {
	"kagee_stage",        "kagee_adjustment_layer", "kagee_grade_filter", "kagee_light_filter",
	"kagee_lens_filter",  "kagee_retro_filter",     "kagee_glow_filter",  "kagee_blur_filter",
	"kagee_glitch_filter", "kagee_trail_filter",
};

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s path/to/kagee.plugin\n", argv[0]);
		return 1;
	}
	char bin[4096], data[4096];
	snprintf(bin, sizeof(bin), "%s/Contents/MacOS/kagee", argv[1]);
	snprintf(data, sizeof(data), "%s/Contents/Resources", argv[1]);

	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return 2;
	}
	obs_module_t *module = NULL;
	int ret = obs_open_module(&module, bin, data);
	if (ret != MODULE_SUCCESS) {
		fprintf(stderr, "obs_open_module failed: %d\n", ret);
		return 3;
	}
	if (!obs_init_module(module)) {
		fprintf(stderr, "obs_init_module failed\n");
		return 4;
	}

	int failed = 0;
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		const char *name = obs_source_get_display_name(ids[i]);
		/* a localised name starts with "Kagee"; a missing locale would return the raw key */
		int ok = name && strncmp(name, "Kagee", 5) == 0;
		printf("%s %-24s %s\n", ok ? "PASS" : "FAIL", ids[i], name ? name : "(not registered)");
		failed += !ok;
	}
	obs_shutdown();
	printf("%s\n", failed ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED");
	return failed ? 5 : 0;
}
