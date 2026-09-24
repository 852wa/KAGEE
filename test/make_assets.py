"""Generate test layer images with alpha for Kagee visual tests."""
import math
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

out = os.path.join(os.path.dirname(__file__), "assets")
os.makedirs(out, exist_ok=True)

# subject: a figure-like silhouette with a bright rim and label (transparent background)
W, H = 900, 1000
im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.ellipse((330, 60, 570, 300), fill=(255, 214, 180, 255), outline=(255, 255, 255, 255), width=6)
d.rounded_rectangle((250, 300, 650, 900), radius=120, fill=(40, 120, 220, 255), outline=(255, 255, 255, 255), width=6)
d.rectangle((300, 880, 600, 1000), fill=(30, 30, 60, 255))
try:
    font = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 90)
except OSError:
    font = ImageFont.load_default()
d.text((W // 2, 560), "SUBJECT", font=font, fill=(255, 255, 255, 255), anchor="mm")
im.save(os.path.join(out, "subject.png"))

# mid layer: silhouetted foliage / pillars with holes
W, H = 1920, 1080
im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
for i, x in enumerate(range(80, W, 360)):
    d.rectangle((x, 250, x + 90, H), fill=(60, 30, 90, 255))
    d.ellipse((x - 60, 150, x + 150, 330), fill=(90, 50, 130, 255))
for i in range(40):
    x = (i * 173) % W
    y = 900 + (i * 37) % 180
    d.ellipse((x, y, x + 140, y + 90), fill=(20, 70, 50, 255))
im.save(os.path.join(out, "mid.png"))

# foreground: blurry bokeh lights (additive-friendly)
im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
cols = [(255, 200, 120), (255, 120, 200), (120, 200, 255)]
for i in range(18):
    x = (i * 331) % W
    y = (i * 197) % H
    r = 40 + (i * 13) % 50
    c = cols[i % 3]
    d.ellipse((x - r, y - r, x + r, y + r), fill=c + (200,))
im = im.filter(ImageFilter.GaussianBlur(6))
im.save(os.path.join(out, "fg_bokeh.png"))

# background: dusk gradient with glowing rings and stars (generated, no external images)
W, H = 3840, 2160
grad = Image.linear_gradient("L").resize((W, H))
top, bottom = Image.new("RGB", (W, H), (70, 20, 140)), Image.new("RGB", (W, H), (230, 60, 120))
bg = Image.composite(bottom, top, grad)
d = ImageDraw.Draw(bg)
for r, c in ((1300, (255, 120, 200)), (1240, (150, 90, 255))):
    d.ellipse((W // 2 - r, H // 2 - r + 500, W // 2 + r, H // 2 + r + 500), outline=c, width=26)
bg = bg.filter(ImageFilter.GaussianBlur(10))
d = ImageDraw.Draw(bg)
for i in range(60):
    x, y, r = (i * 997) % W, (i * 613) % (H // 2), 3 + i % 5
    d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 240, 220))
bg.save(os.path.join(out, "bg.png"))

# two opaque "cards" (stand-ins for a browser / window capture)
for name, hue in (("card.png", (30, 160, 220)), ("card2.png", (240, 120, 40))):
    cw, ch = 1600, 1000
    im = Image.new("RGB", (cw, ch), (18, 20, 28))
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, cw, 90), fill=hue)
    for k in range(6):
        d.rounded_rectangle((60, 150 + k * 130, cw - 60 - k * 120, 240 + k * 130), radius=20,
                            fill=tuple(int(v * (0.35 + 0.1 * k)) for v in hue))
    im.save(os.path.join(out, name))
print("ok")
