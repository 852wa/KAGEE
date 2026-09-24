"""Drive the portable OBS through Kagee scenarios and save screenshots to test/out."""
import asyncio
import os
import sys

from obsws import OBS

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "out")
os.makedirs(out, exist_ok=True)
only = sys.argv[1:]  # optional scenario name filter


async def shot(obs, name, source="Stage", wait=0.4):
    await asyncio.sleep(wait)
    path = os.path.join(out, name + ".png")
    await obs.screenshot(source, path)
    print("saved", path)


async def stage_tests(obs):
    home = {"cam_x": 0, "cam_y": 0, "cam_dolly": 0, "cam_yaw": 0, "cam_pitch": 0, "cam_roll": 0, "cam_fov": 40}
    await obs.settings("Stage", {**home, "dof_on": False, "edit_smooth": 0})
    await shot(obs, "01_home_nodof", wait=1.0)
    await obs.settings("Stage", {"dof_on": True})
    await shot(obs, "02_home_dof")
    await obs.settings("Stage", {**home, "cam_yaw": 25})
    await shot(obs, "03_orbit_right25")
    await obs.settings("Stage", {**home, "cam_pitch": 15})
    await shot(obs, "04_orbit_up15")
    await obs.settings("Stage", {**home, "cam_dolly": 600, "cam_x": -150})
    await shot(obs, "05_dolly_in_pan_left")
    await obs.settings("Stage", {**home, "cam_roll": 10, "cam_fov": 30})
    await shot(obs, "06_roll10_fov30")

    # shots + animated transition
    await obs.settings("Stage", {**home, "cam_yaw": -20, "cam_dolly": 400,
                                 "shot1_valid": True, "shot1_x": 0, "shot1_y": 0, "shot1_dolly": 400,
                                 "shot1_yaw": -20, "shot1_pitch": 0, "shot1_roll": 0, "shot1_fov": 40,
                                 "trans_dur": 2.0, "trans_ease": 2})
    await obs.settings("Stage", home)
    await asyncio.sleep(0.5)
    await obs.hotkey("Kagee.Shot1", "Stage")
    await shot(obs, "07_transition_mid", wait=1.0)
    await shot(obs, "08_transition_end", wait=1.6)

    await obs.hotkey("Kagee.Flash", "Stage")
    await shot(obs, "09_flash", wait=0.05)
    await obs.settings("Stage", {**home, "edit_smooth": 0.25})


FILTERS = [
    ("lens", "kagee_lens_filter", {"vig_amount": 70, "ca": 60, "distortion": 40, "grain": 20, "lb_ratio": 2.39}),
    ("retro_vhs", "kagee_retro_filter", {"mode": 0}),
    ("retro_crt", "kagee_retro_filter", {"mode": 1}),
    ("retro_film", "kagee_retro_filter", {"mode": 2, "fade": 60}),
    ("retro_game", "kagee_retro_filter", {"mode": 3, "pixel_size": 8, "levels": 4}),
    ("retro_green4", "kagee_retro_filter", {"mode": 3, "pixel_size": 6, "palette": 1}),
    ("retro_led", "kagee_retro_filter", {"mode": 4, "pixel_size": 12}),
    ("retro_halftone", "kagee_retro_filter", {"mode": 5, "pixel_size": 10}),
    ("glow", "kagee_glow_filter", {}),
    ("glow_star", "kagee_glow_filter", {"flare": 3, "streak_len": 500}),
    ("glow_streak", "kagee_glow_filter", {"threshold": 80, "flare": 1, "streak_len": 900}),
    ("blur_iris", "kagee_blur_filter", {"mode": 1, "radius": 30}),
    ("blur_tilt", "kagee_blur_filter", {"mode": 2, "radius": 30, "angle": 10}),
    ("blur_dir", "kagee_blur_filter", {"mode": 3, "radius": 60, "angle": 0}),
    ("blur_zoom", "kagee_blur_filter", {"mode": 4, "zoom": 40, "spin": 10}),
    ("glitch", "kagee_glitch_filter", {"base": 100, "warp": 30}),
    ("trail", "kagee_trail_filter", {"decay": 90}),
    ("light_default", "kagee_light_filter", {}),
    ("light_rim", "kagee_light_filter", {"ambient": 35, "L1_on": False, "L2_on": False, "L3_on": False,
                                              "L4_on": True, "L4_width": 10, "L4_int": 200}),
    ("grade_p01", "kagee_grade_filter", {"preset": 1, "preset_amt": 100}),
    ("grade_p02", "kagee_grade_filter", {"preset": 2, "preset_amt": 100}),
    ("grade_p03", "kagee_grade_filter", {"preset": 3, "preset_amt": 100}),
    ("grade_p04", "kagee_grade_filter", {"preset": 4, "preset_amt": 100}),
    ("grade_p05", "kagee_grade_filter", {"preset": 5, "preset_amt": 100}),
    ("grade_p06", "kagee_grade_filter", {"preset": 6, "preset_amt": 100}),
    ("grade_p07", "kagee_grade_filter", {"preset": 7, "preset_amt": 100}),
    ("grade_p08", "kagee_grade_filter", {"preset": 8, "preset_amt": 100}),
    ("grade_p09", "kagee_grade_filter", {"preset": 9, "preset_amt": 100}),
    ("grade_p10", "kagee_grade_filter", {"preset": 10, "preset_amt": 100}),
    ("grade_split", "kagee_grade_filter", {"preset": 10, "preset_amt": 100, "compare": True}),
]


async def filter_tests(obs):
    for name, kind, settings in FILTERS:
        if only and not any(o in name for o in only):
            continue
        fname = "T_" + name
        await obs.call("CreateSourceFilter", {"sourceName": "Stage", "filterName": fname,
                                              "filterKind": kind, "filterSettings": settings})
        if name == "trail":
            # animate the camera so the trail has something to show
            await obs.settings("Stage", {"edit_smooth": 0.0})
            for i in range(12):
                await obs.settings("Stage", {"cam_x": -300 + i * 50})
                await asyncio.sleep(0.05)
            await shot(obs, "F_" + name, wait=0.0)
        else:
            await shot(obs, "F_" + name, wait=0.6)
        await obs.call("RemoveSourceFilter", {"sourceName": "Stage", "filterName": fname})
        if name == "trail":
            await obs.settings("Stage", {"cam_x": 0, "edit_smooth": 0.25})


async def main():
    async with OBS() as obs:
        if not only or "stage" in only:
            await stage_tests(obs)
        await filter_tests(obs)


asyncio.run(main())
