"""Reproduces 'empty stage placed on top of a scene' and checks the one-click import."""
import asyncio, os
from PIL import Image, ImageChops, ImageStat
from obsws import OBS

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
results = []

def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0

def check(name, ok, detail):
    results.append(ok); print(("PASS " if ok else "FAIL ") + name + "  " + detail)

async def grab(obs, name):
    path = os.path.join(out, name + ".png")
    await obs.screenshot("UserLike", path, width=960)
    return Image.open(path).convert("RGB")

async def main():
    async with OBS() as obs:
        await obs.call("SetCurrentProgramScene", {"sceneName": "UserLike"})
        sid = (await obs.call("GetSceneItemId", {"sceneName": "UserLike", "sourceName": "UStage"}))["sceneItemId"]
        await obs.call("SetSceneItemEnabled", {"sceneName": "UserLike", "sceneItemId": sid, "sceneItemEnabled": False})
        await asyncio.sleep(0.8)
        without = await grab(obs, "I_without_stage")
        await obs.call("SetSceneItemEnabled", {"sceneName": "UserLike", "sceneItemId": sid, "sceneItemEnabled": True})
        await asyncio.sleep(0.8)
        empty = await grab(obs, "I_empty_stage")
        check("an empty stage no longer hides the scene", diff(without, empty) < 0.3, f"diff={diff(without, empty):.2f}")

        await obs.settings("UStage", {"dof_on": False, "hand_amount": 0, "idle_mode": 0, "edit_smooth": 0})
        await obs.call("PressInputPropertiesButton", {"inputName": "UStage", "propertyName": "btn_import"})
        await asyncio.sleep(1.5)
        imported = await grab(obs, "I_imported_home")
        check("after import the home view looks the same", diff(without, imported) < 2.0, f"diff={diff(without, imported):.2f}")
        hidden = []
        for n in ("UBg", "UCard"):
            i = (await obs.call("GetSceneItemId", {"sceneName": "UserLike", "sourceName": n}))["sceneItemId"]
            hidden.append(not (await obs.call("GetSceneItemEnabled", {"sceneName": "UserLike", "sceneItemId": i}))["sceneItemEnabled"])
        check("original items are hidden", all(hidden), str(hidden))
        st = (await obs.call("GetInputSettings", {"inputName": "UStage"}))["inputSettings"]
        check("two layers were created", st.get("l1_src") == "UBg" and st.get("l2_src") == "UCard" and st.get("layer_count") == 2,
              f"l1={st.get('l1_src')} l2={st.get('l2_src')} count={st.get('layer_count')}")
        check("full-screen background gets auto-fill, the card does not",
              st.get("l1_autofill") is True and not st.get("l2_autofill"), f"{st.get('l1_autofill')} {st.get('l2_autofill')}")

        await obs.settings("UStage", {"cam_yaw": 20, "cam_dolly": 300})
        await asyncio.sleep(0.8)
        moved = await grab(obs, "I_imported_orbit")
        check("camera move now changes the picture (3D)", diff(imported, moved) > 3.0, f"diff={diff(imported, moved):.2f}")
        await obs.settings("UStage", {"cam_yaw": 0, "cam_dolly": 0})
        await obs.call("SetCurrentProgramScene", {"sceneName": "Main"})
    print(f"{sum(results)}/{len(results)} passed")

asyncio.run(main())
