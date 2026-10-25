#!/usr/bin/env python3
# The README's banner (docs/images/banner.png, 1600x480, laid out like Crema's): the box's logo and
# Conker on the left, glowing out of a dark backdrop, the name on the right.
#   make-banner.py <assets dir> <out png>
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

assets, out = sys.argv[1], sys.argv[2]
W, H = 1600, 480
cover = Image.open(f"{assets}/cover.jpg").convert("RGB")

# backdrop: warm brown in the left half fading to near black, like Crema's purple
bg = Image.new("RGB", (W, H))
px = bg.load()
for x in range(W):
    for y in range(H):
        d = ((x - 470) ** 2 / 700 ** 2 + (y - 240) ** 2 / 420 ** 2) ** 0.5
        t = max(0.0, 1.0 - d)
        px[x, y] = (int(14 + 52 * t), int(9 + 26 * t), int(6 + 8 * t))

# the art: the logo and Conker (as make-art.py crops them), its black melting into the backdrop
art = cover.crop((14, 6, 286, 246))
art.paste((0, 0, 0), (0, 0, 64, 106))
art = art.resize((round(art.width * 430 / art.height), 430), Image.LANCZOS)
lum = art.convert("L").point(lambda v: 255 if v > 24 else v * 10)
edge = Image.new("L", art.size, 0)
ImageDraw.Draw(edge).rounded_rectangle((18, 18, art.width - 18, art.height - 18), 40, fill=255)
edge = edge.filter(ImageFilter.GaussianBlur(14))
mask = Image.composite(lum, Image.new("L", art.size, 0), edge)
ax, ay = 470 - art.width // 2, (H - art.height) // 2
glow = Image.new("RGB", (W, H), (0, 0, 0))
glow.paste((255, 150, 40), (ax, ay), mask)
glow = glow.filter(ImageFilter.GaussianBlur(40))
bg = ImageChops.add(bg, glow.point(lambda v: v * 2 // 5))
bg.paste(art, (ax, ay), mask)

# the name: "Conker" in an orange-to-gold gradient, right-aligned, then two small lines
fonts = "C:/Windows/Fonts"
title_font = ImageFont.truetype(f"{fonts}/segoeuib.ttf", 190)
sub_font = ImageFont.truetype(f"{fonts}/segoeuil.ttf", 40)
right = 1520
text = "Conker"
l, t, r, b = title_font.getbbox(text)
tw, th = r - l, b - t
tmask = Image.new("L", (tw, th), 0)
ImageDraw.Draw(tmask).text((-l, -t), text, font=title_font, fill=255)
grad = Image.new("RGB", (tw, th))
gp = grad.load()
for x in range(tw):
    f = x / (tw - 1)
    c = (255, int(150 + 75 * f), int(40 + 40 * f))
    for y in range(th):
        gp[x, y] = c
ty = 130
bg.paste(grad, (right - tw, ty), tmask)

draw = ImageDraw.Draw(bg)
def spaced(y, s, font, fill, gap):
    w = sum(font.getlength(ch) for ch in s) + gap * (len(s) - 1)
    x = right - w
    for ch in s:
        draw.text((x, y), ch, font=font, fill=fill)
        x += font.getlength(ch) + gap
spaced(ty + th + 40, "BAD FUR DAY \u00b7 RECOMPILED", sub_font, (245, 236, 225), 4)
spaced(ty + th + 100, "PS4", ImageFont.truetype(f"{fonts}/segoeuil.ttf", 34), (255, 196, 110), 4)
bg.save(out)
