"""Builds the README images in docs/images from test/out (run the test suites first)."""
import os

from PIL import Image, ImageDraw, ImageFont

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "test", "out")
dst = os.path.join(root, "docs", "images")
os.makedirs(dst, exist_ok=True)
font = ImageFont.truetype("C:/Windows/Fonts/meiryob.ttc", 22)


def label(img, x, y, text):
    d = ImageDraw.Draw(img)
    w = d.textlength(text, font=font)
    d.rectangle((x, y, x + w + 16, y + 34), fill=(0, 0, 0))
    d.text((x + 8, y + 3), text, font=font, fill=(255, 255, 255))


def tiles(items, cols, size, out):
    tw, th = size
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGB", (tw * cols, th * rows), (16, 16, 20))
    for i, (name, text) in enumerate(items):
        im = Image.open(os.path.join(src, name)).convert("RGB").resize((tw, th), Image.LANCZOS)
        x, y = (i % cols) * tw, (i // cols) * th
        sheet.paste(im, (x, y))
        label(sheet, x, y, text)
    sheet.save(os.path.join(dst, out), optimize=True)


tiles([("I_without_stage.png", "元のシーン / Original"),
       ("I_imported_home.png", "取り込み直後 / Imported"),
       ("I_imported_orbit.png", "カメラを動かすと3D / Camera move")], 3, (640, 360), "hero.png")

tiles([("F_grade_p01.png", "カラグレ: ティール&オレンジ"), ("F_grade_p10.png", "カラグレ: ネオン"),
       ("F_light_default.png", "ライティング"), ("F_glow_star.png", "グロー&フレア"),
       ("F_retro_vhs.png", "レトロ: VHS"), ("F_retro_game.png", "レトロ: ゲーム"),
       ("F_glitch.png", "グリッチ"), ("F_lens.png", "レンズ")], 4, (480, 270), "effects.png")

dock = Image.open(os.path.join(src, "dock", "tab_0.png")).convert("RGB")
dock.save(os.path.join(dst, "dock-easy.png"), optimize=True)
print("images written to", dst)
