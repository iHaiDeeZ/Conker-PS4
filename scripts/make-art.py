#!/usr/bin/env python3
# The package's pictures from CBFD-Recompiled's own launcher thumbnail (host/assets/thumbnail.png,
# an acorn on an autumn gradient; no game art):
#   sce_sys/icon0.png  512x512, the home screen tile
#   sce_sys/pic1.png   1920x1080, the background behind it (and the boot splash)
#   make-art.py <thumbnail.png> <sce_sys dir>
import sys

from PIL import Image, ImageDraw

thumb = Image.open(sys.argv[1]).convert("RGB")
out = sys.argv[2]

thumb.resize((512, 512), Image.LANCZOS).save(f"{out}/icon0.png")

# the thumbnail's gradient, stretched over 1080p, the acorn in the middle at twice its size
W, H = 1920, 1080
top = thumb.getpixel((256, 2))
bottom = thumb.getpixel((256, 509))
pic = Image.new("RGB", (W, H))
draw = ImageDraw.Draw(pic)
for y in range(H):
    t = y / (H - 1)
    draw.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
acorn = thumb.crop((140, 110, 372, 410)).resize((464, 600), Image.LANCZOS)
mask = Image.new("L", acorn.size, 0)
# only the acorn's own pixels: the ones that differ from the gradient behind them
for y in range(acorn.size[1]):
    t_src = (110 + y * 300 / 600) / 511
    bg = tuple(round(a + (b - a) * t_src) for a, b in zip(top, bottom))
    for x in range(acorn.size[0]):
        p = acorn.getpixel((x, y))
        if sum(abs(p[i] - bg[i]) for i in range(3)) > 24:
            mask.putpixel((x, y), 255)
pic.paste(acorn, ((W - acorn.size[0]) // 2, (H - acorn.size[1]) // 2), mask)
pic.save(f"{out}/pic1.png")
