"""Shots remember camera + layer layout; presets; auto shots; depth strength."""
import asyncio
import os

from PIL import Image, ImageChops, ImageStat

from obsws import OBS

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
results = []
HOME = {"cam_x": 0, "cam_y": 0, "cam_dolly": 0, "cam_yaw": 0, "cam_pitch": 0, "cam_roll": 0, "cam_fov": 40}


def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0


def check(name, ok, detail):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + "  " + detail)


async def grab(obs, name):
    path = os.path.join(out, name + ".png")
    await obs.screenshot("Stage", path, width=480)
    return Image.open(path).convert("RGB")


async def press(obs, button):
    await obs.call("PressInputPropertiesButton", {"inputName": "Stage", "propertyName": button})


async def settings(obs):
    return (await obs.call("GetInputSettings", {"inputName": "Stage"}))["inputSettings"]


async def main():
    async with OBS() as obs:
        await obs.settings("Stage", {**HOME, "hand_amount": 0, "idle_mode": 0, "auto_mode": 0, "edit_smooth": 0,
                                     "trans_dur": 0.6, "trans_ease": 1, "dof_on": False, "depth_scale": 100})
        # the reported case: arrange the LAYERS (not the camera), save 1; rearrange, save 2
        await obs.settings("Stage", {"l3_x": -400, "l3_z": 0})
        await asyncio.sleep(0.4)
        await obs.settings("Stage", {"shot_slot": 1})
        await press(obs, "btn_save_shot")
        await obs.settings("Stage", {"l3_x": 400, "l3_z": 600})
        await asyncio.sleep(0.4)
        await obs.settings("Stage", {"shot_slot": 2})
        await press(obs, "btn_save_shot")
        await asyncio.sleep(0.4)
        st = await settings(obs)
        l1 = st.get("shot1_layers", [{}] * 8)[2]
        l2 = st.get("shot2_layers", [{}] * 8)[2]
        check("shot 1 and 2 store different layouts", l1.get("x") == -400 and l2.get("x") == 400,
              f"shot1 l3.x={l1.get('x')} shot2 l3.x={l2.get('x')}")

        await obs.hotkey("Kagee.Shot1", "Stage")
        await asyncio.sleep(1.0)
        a = await grab(obs, "S_shot1")
        await obs.hotkey("Kagee.Shot2", "Stage")
        await asyncio.sleep(0.3)
        mid = await grab(obs, "S_shot_mid")
        await asyncio.sleep(0.8)
        b = await grab(obs, "S_shot2")
        check("shot 1 and 2 look different", diff(a, b) > 3.0, f"diff={diff(a, b):.2f}")
        check("layers animate during the transition", diff(mid, a) > 1.0 and diff(mid, b) > 1.0,
              f"mid-1={diff(mid, a):.2f} mid-2={diff(mid, b):.2f}")
        await obs.hotkey("Kagee.Shot1", "Stage")
        await asyncio.sleep(1.0)
        a2 = await grab(obs, "S_shot1_again")
        check("going back to shot 1 restores it", diff(a, a2) < 0.5, f"diff={diff(a, a2):.2f}")

        # presets
        await obs.settings("Stage", {"shot_slot": 3, "shot_preset_pick": 4})  # from the left
        await press(obs, "btn_apply_preset")
        await asyncio.sleep(0.3)
        st = await settings(obs)
        check("preset fills the slot", st.get("shot3_valid") and st.get("shot3_yaw") == -22 and st.get("shot3_preset") == 4,
              f"valid={st.get('shot3_valid')} yaw={st.get('shot3_yaw')} preset={st.get('shot3_preset')}")
        await press(obs, "btn_auto_shots")
        await asyncio.sleep(0.3)
        st = await settings(obs)
        valid = [bool(st.get(f"shot{i}_valid")) for i in range(1, 9)]
        presets = [st.get(f"shot{i}_preset") for i in range(1, 9)]
        check("auto creates 8 different shots", all(valid) and len(set(presets)) == 8, f"{presets}")

        # depth strength changes how much layers separate
        await obs.hotkey("Kagee.Live", "Stage")
        await obs.settings("Stage", {**HOME, "cam_yaw": 20, "depth_scale": 100})
        await asyncio.sleep(0.8)
        d1 = await grab(obs, "S_depth100")
        await obs.settings("Stage", {"depth_scale": 250})
        await asyncio.sleep(0.8)
        d2 = await grab(obs, "S_depth250")
        check("depth strength changes the parallax", diff(d1, d2) > 2.0, f"diff={diff(d1, d2):.2f}")
        await obs.settings("Stage", {**HOME, "depth_scale": 100, "l3_x": 0, "l3_z": 0, "dof_on": True,
                                     **{f"shot{i}_valid": False for i in range(1, 9)},
                                     "edit_smooth": 0.25})
    print(f"{sum(results)}/{len(results)} passed")


asyncio.run(main())

