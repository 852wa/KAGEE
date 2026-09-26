"""Importing more items than the 8-layer limit: the 8 nearest the stage become layers and are hidden,
the rest stay visible in the scene."""
import asyncio

from obsws import OBS

results = []


def check(name, ok, detail):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + "  " + detail)


async def main():
    async with OBS() as obs:
        await obs.call("PressInputPropertiesButton", {"inputName": "MStage", "propertyName": "btn_import"})
        await asyncio.sleep(1.0)
        st = (await obs.call("GetInputSettings", {"inputName": "MStage"}))["inputSettings"]
        layers = [st.get(f"l{i}_src") for i in range(1, 9)]
        check("8 layers created from the 8 items nearest the stage (M2..M9)",
              layers == [f"M{i}" for i in range(2, 10)] and st.get("layer_count") == 8, f"{layers}")
        visible = {}
        for i in range(10):
            iid = (await obs.call("GetSceneItemId", {"sceneName": "Many", "sourceName": f"M{i}"}))["sceneItemId"]
            visible[i] = (await obs.call("GetSceneItemEnabled", {"sceneName": "Many", "sceneItemId": iid}))["sceneItemEnabled"]
        check("items that did not fit (M0, M1) stay visible", visible[0] and visible[1], str(visible))
        check("imported items (M2..M9) are hidden", not any(visible[i] for i in range(2, 10)), str(visible))
    print(f"{sum(results)}/{len(results)} passed")


asyncio.run(main())
