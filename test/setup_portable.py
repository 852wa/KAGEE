"""Write an isolated portable-OBS config with a Kagee test scene.

Usage: python setup_portable.py <portable obs root>
"""
import json
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "obs-portable")
here = os.path.dirname(os.path.abspath(__file__))
assets = os.path.join(here, "assets")
cfg = os.path.join(root, "config", "obs-studio")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


renderer = os.environ.get("KAGEE_RENDERER", "Direct3D 11")  # "OpenGL" exercises the macOS/Linux shader path
ini = f"""[General]
FirstRun=true
EnableAutoUpdates=false
InfoIncrement=-1

[Video]
Renderer={renderer}

[Basic]
Profile=Test
ProfileDir=Test
SceneCollection=Test
SceneCollectionFile=Test
"""
write(os.path.join(cfg, "global.ini"), ini)
write(os.path.join(cfg, "user.ini"), ini)

write(os.path.join(cfg, "basic", "profiles", "Test", "basic.ini"), """[General]
Name=Test

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=30
""")

write(os.path.join(cfg, "plugin_config", "obs-websocket", "config.json"), json.dumps({
    "alerts_enabled": False,
    "auth_required": False,
    "first_load": False,
    "server_enabled": True,
    "server_password": "",
    "server_port": 4466,
}))


def image(name, path):
    return {"id": "image_source", "versioned_id": "image_source", "name": name,
            "settings": {"file": path.replace("\\", "/")}}


bg = os.path.join(assets, "bg.png")
fwd = lambda p: p.replace("\\", "/")
stage_settings = {
    "l1_on": True, "l1_type": 1, "l1_file": fwd(bg), "l1_fit": 1, "l1_z": 2000,
    "l2_on": True, "l2_type": 1, "l2_file": fwd(os.path.join(assets, "mid.png")), "l2_fit": 1, "l2_z": 700,
    "l3_on": True, "l3_type": 1, "l3_file": fwd(os.path.join(assets, "subject.png")), "l3_fit": 0, "l3_z": 0,
    "l3_scale": 90, "l3_y": 60,
    "l4_on": True, "l4_src": "Bokeh", "l4_fit": 1, "l4_z": -300, "l4_blend": 2,
    "hand_amount": 0, "idle_mode": 0,
    "shot1_valid": True, "shot1_fov": 40, "shot2_valid": True, "shot2_fov": 40, "shot2_dolly": 500,
    "shot3_valid": True, "shot3_fov": 40, "shot3_yaw": -20,
}
stage_filters = [
    {"id": "kagee_light_filter", "versioned_id": "kagee_light_filter", "name": "Lighting", "enabled": True, "settings": {}},
    {"id": "kagee_grade_filter", "versioned_id": "kagee_grade_filter", "name": "Grade", "enabled": True, "settings": {}},
    {"id": "kagee_lens_filter", "versioned_id": "kagee_lens_filter", "name": "Lens", "enabled": True, "settings": {}},
]
def item(name, i, pos=(0.0, 0.0), scale=1.0, rot=0.0, blend="OBS_BLEND_NORMAL"):
    return {"name": name, "id": i, "visible": True, "locked": False, "blend_type": blend,
            "pos": {"x": pos[0], "y": pos[1]}, "scale": {"x": scale, "y": scale}, "rot": rot,
            "align": 5, "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
            "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0}


sources = [
    image("Bokeh", os.path.join(assets, "fg_bokeh.png")),
    {"id": "scene", "versioned_id": "scene", "name": "Assets",
     "settings": {"id_counter": 1, "custom_size": False, "items": [item("Bokeh", 1)]}},
    {"id": "kagee_stage", "versioned_id": "kagee_stage", "name": "Stage",
     "settings": stage_settings, "filters": stage_filters if os.environ.get("KAGEE_TEST_FILTERS") else []},
    image("Small", os.path.join(assets, "card2.png")),
    image("TopCard", os.path.join(assets, "card.png")),
    {"id": "kagee_adjustment_layer", "versioned_id": "kagee_adjustment_layer", "name": "Adjust",
     "settings": {}, "filters": []},
    image("UBg", os.path.join(assets, "bg.png")),
    image("UCard", os.path.join(assets, "card.png")),
    {"id": "kagee_stage", "versioned_id": "kagee_stage", "name": "UStage", "settings": {}, "filters": []},
    {"id": "scene", "versioned_id": "scene", "name": "UserLike",
     "settings": {"id_counter": 3, "custom_size": False, "items": [
         item("UBg", 1, pos=(-393.0, -201.0), scale=0.72),
         item("UCard", 2, pos=(1100.0, 250.0), scale=0.2, rot=10.0),
         item("UStage", 3),
     ]}},
    {"id": "scene", "versioned_id": "scene", "name": "Main",
     "settings": {"id_counter": 4, "custom_size": False, "items": [
         item("Stage", 1),
         item("Small", 2, pos=(150.0, 600.0), scale=0.15, rot=15.0, blend="OBS_BLEND_SCREEN"),
         item("Adjust", 3),
         item("TopCard", 4, pos=(1400.0, 60.0), scale=0.12),
     ]}},
]

# "Many": 10 items under an empty stage, to check the 8-layer import limit
many_items = []
for i in range(10):
    name = f"M{i}"
    sources.append(image(name, os.path.join(assets, "card.png" if i % 2 else "card2.png")))
    many_items.append(item(name, i + 1, pos=(60.0 + (i % 5) * 360.0, 100.0 + (i // 5) * 450.0), scale=0.2))
sources.append({"id": "kagee_stage", "versioned_id": "kagee_stage", "name": "MStage", "settings": {}, "filters": []})
many_items.append(item("MStage", 11))
sources.append({"id": "scene", "versioned_id": "scene", "name": "Many",
                "settings": {"id_counter": 11, "custom_size": False, "items": many_items}})

start = os.environ.get("KAGEE_TEST_SCENE", "Main")
scene = {
    "name": "Test",
    "current_scene": start,
    "current_program_scene": start,
    "scene_order": [{"name": "Main"}, {"name": "Assets"}, {"name": "UserLike"}, {"name": "Many"}],
    "sources": sources,
    "groups": [],
    "transitions": [],
    "current_transition": "Fade",
    "transition_duration": 300,
}
write(os.path.join(cfg, "basic", "scenes", "Test.json"), json.dumps(scene, indent=2))
print("portable config written to", cfg)
