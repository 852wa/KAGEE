"""Behavioural checks for Kagee camera motion features, using frame differences."""
import asyncio
import io
import os

from PIL import Image, ImageChops, ImageStat

from obsws import OBS

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "out")
os.makedirs(out, exist_ok=True)
HOME = {"cam_x": 0, "cam_y": 0, "cam_dolly": 0, "cam_yaw": 0, "cam_pitch": 0, "cam_roll": 0, "cam_fov": 40}
results = []


async def grab(obs, name=None):
    path = os.path.join(out, (name or "_tmp") + ".png")
    await obs.screenshot("Stage", path, width=480)
    with open(path, "rb") as f:
        return Image.open(io.BytesIO(f.read())).convert("RGB")


def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0


def check(name, ok, detail):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + "  " + detail)


async def main():
    async with OBS() as obs:
        base = {**HOME, "edit_smooth": 0, "hand_amount": 0, "idle_mode": 0, "auto_mode": 0, "beat_pulse": 0,
                "dof_on": True}
        await obs.settings("Stage", base)
        await asyncio.sleep(0.6)
        a = await grab(obs)
        await asyncio.sleep(0.5)
        b = await grab(obs)
        check("static camera is stable", diff(a, b) < 0.5, f"diff={diff(a, b):.2f}")

        await obs.settings("Stage", {"hand_amount": 60, "hand_speed": 1.0})
        await asyncio.sleep(0.5)
        a = await grab(obs)
        await asyncio.sleep(0.7)
        b = await grab(obs)
        check("handheld shake moves the camera", diff(a, b) > 1.0, f"diff={diff(a, b):.2f}")
        await obs.settings("Stage", {"hand_amount": 0})

        await obs.settings("Stage", {"idle_mode": 1, "idle_amp": 60, "idle_period": 2.0})
        await asyncio.sleep(0.3)
        a = await grab(obs)
        await asyncio.sleep(0.5)
        b = await grab(obs)
        check("idle sway animates", diff(a, b) > 1.0, f"diff={diff(a, b):.2f}")
        await obs.settings("Stage", {"idle_mode": 0})
        await asyncio.sleep(0.4)

        a = await grab(obs)
        await obs.hotkey("Kagee.Impact", "Stage")
        await asyncio.sleep(0.08)
        b = await grab(obs, "M_impact")
        await asyncio.sleep(1.2)
        c = await grab(obs)
        check("impact shakes then settles", diff(a, b) > 1.0 and diff(a, c) < 0.5,
              f"during={diff(a, b):.2f} after={diff(a, c):.2f}")

        # two shots, next / prev hotkeys
        shots = {"shot1_valid": True, **{f"shot1_{k[4:]}": v for k, v in HOME.items()}, "shot1_yaw": -15,
                 "shot2_valid": True, **{f"shot2_{k[4:]}": v for k, v in HOME.items()}, "shot2_dolly": 700,
                 "trans_dur": 0.3, "trans_ease": 2,
                 **{f"shot{i}_valid": False for i in range(3, 9)}}
        await obs.settings("Stage", shots)
        await obs.hotkey("Kagee.Shot1", "Stage")
        await asyncio.sleep(0.8)
        s1 = await grab(obs, "M_shot1")
        await obs.hotkey("Kagee.Next", "Stage")
        await asyncio.sleep(0.8)
        s2 = await grab(obs, "M_shot2")
        await obs.hotkey("Kagee.Prev", "Stage")
        await asyncio.sleep(0.8)
        s1b = await grab(obs)
        check("next/prev hotkeys switch shots", diff(s1, s2) > 3.0 and diff(s1, s1b) < 0.5,
              f"s1-s2={diff(s1, s2):.2f} s1-s1'={diff(s1, s1b):.2f}")

        # auto switch on beats: 240 bpm, every 2 beats => switch every 0.5 s
        await obs.settings("Stage", {"auto_mode": 2, "bpm": 240, "auto_beats": 2, "auto_order": 0})
        seen = []
        for _ in range(16):
            await asyncio.sleep(0.25)
            f = await grab(obs)
            seen.append(min(diff(f, s1), diff(f, s2)) < 1.0 and (diff(f, s1) < diff(f, s2)))
        changes = sum(1 for i in range(1, len(seen)) if seen[i] != seen[i - 1])
        check("BPM auto switch alternates shots", changes >= 3, f"pattern={seen}")
        await obs.hotkey("Kagee.AutoToggle", "Stage")
        await asyncio.sleep(0.6)
        f1 = await grab(obs)
        await asyncio.sleep(1.2)
        f2 = await grab(obs)
        check("auto toggle hotkey pauses switching", diff(f1, f2) < 0.5, f"diff={diff(f1, f2):.2f}")
        await obs.settings("Stage", {"auto_mode": 0})

        # live camera hotkey returns home
        await obs.hotkey("Kagee.Live", "Stage")
        await asyncio.sleep(0.8)
        h = await grab(obs)
        check("live hotkey returns to live camera", diff(h, a) < 0.8, f"diff={diff(h, a):.2f}")

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")


asyncio.run(main())

