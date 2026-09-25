"""Checks for the Kagee adjustment layer."""
import asyncio, io, os
from PIL import Image, ImageChops, ImageStat
from obsws import OBS

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
results = []

async def grab(obs, name):
    path = os.path.join(out, name + ".png")
    await obs.screenshot("Main", path, width=960)
    return Image.open(path).convert("RGB")

def diff(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0

def check(name, ok, detail):
    results.append(ok); print(("PASS " if ok else "FAIL ") + name + "  " + detail)

async def item_id(obs, name):
    r = await obs.call("GetSceneItemId", {"sceneName": "Main", "sourceName": name})
    return r["sceneItemId"]

async def main():
    async with OBS() as obs:
        await obs.settings("Stage", {"hand_amount": 0, "idle_mode": 0, "auto_mode": 0})
        adj = await item_id(obs, "Adjust")
        stage = await item_id(obs, "Stage")
        # crop the stage item to exercise the crop path
        await obs.call("SetSceneItemTransform", {"sceneName": "Main", "sceneItemId": stage,
                        "sceneItemTransform": {"cropLeft": 200, "cropTop": 100}})
        await obs.call("SetSceneItemEnabled", {"sceneName": "Main", "sceneItemId": adj, "sceneItemEnabled": False})
        await asyncio.sleep(0.6)
        base = await grab(obs, "A_base")
        await obs.call("SetSceneItemEnabled", {"sceneName": "Main", "sceneItemId": adj, "sceneItemEnabled": True})
        await asyncio.sleep(0.6)
        same = await grab(obs, "A_nofilter")
        check("no filters: identical to scene without the layer", diff(base, same) < 0.3, f"diff={diff(base, same):.3f}")

        await obs.call("CreateSourceFilter", {"sourceName": "Adjust", "filterName": "fx",
                        "filterKind": "kagee_retro_filter", "filterSettings": {"mode": 3, "pixel_size": 10}})
        await asyncio.sleep(0.6)
        fx = await grab(obs, "A_filtered")
        # TopCard area, from its real transform (canvas 1920 -> screenshot 960), shrunk 2px to avoid edges
        tid = await item_id(obs, "TopCard")
        tr = (await obs.call("GetSceneItemTransform", {"sceneName": "Main", "sceneItemId": tid}))["sceneItemTransform"]
        k = 960 / 1920
        top = (int(tr["positionX"] * k) + 2, int(tr["positionY"] * k) + 2,
               int((tr["positionX"] + tr["width"]) * k) - 2, int((tr["positionY"] + tr["height"]) * k) - 2)
        lower = (400, 250, 690, 460)   # stage only: avoids the Small card and TopCard
        check("layers below are processed", diff(base, fx, lower) > 3.0, f"diff={diff(base, fx, lower):.2f}")
        check("layer above is untouched", diff(base, fx, top) < 0.5, f"diff={diff(base, fx, top):.2f}")

        await obs.settings("Adjust", {"depth": 1})  # only the 'Small' card directly below
        await asyncio.sleep(0.6)
        d1 = await grab(obs, "A_depth1")
        check("depth=1 leaves deeper layers untouched", diff(base, d1, lower) < 3.0, f"diff={diff(base, d1, lower):.2f}")
        await obs.settings("Adjust", {"depth": 0})
        await obs.call("RemoveSourceFilter", {"sourceName": "Adjust", "filterName": "fx"})
        await obs.call("SetSceneItemTransform", {"sceneName": "Main", "sceneItemId": stage,
                        "sceneItemTransform": {"cropLeft": 0, "cropTop": 0}})
    print(f"{sum(results)}/{len(results)} passed")

asyncio.run(main())
