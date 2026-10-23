#!/usr/bin/env python3
# The package's pictures from the game's own art (assets/: the N64 box art and a gameplay shot, from
# Wikipedia's article; this port isn't released, so the game's art is fine here):
#   sce_sys/icon0.png  512x512, the home screen tile: the box's logo and Conker
#   sce_sys/pic1.png   1920x1080, the background behind it: the gameplay shot, blurred, with the box
#   make-art.py <assets dir> <sce_sys dir>
import sys

from PIL import Image, ImageEnhance, ImageFilter

assets, out = sys.argv[1], sys.argv[2]
cover = Image.open(f"{assets}/cover.jpg").convert("RGB")
shot = Image.open(f"{assets}/screenshot.jpg").convert("RGB")

# the tile: the logo and Conker (left of the box's red N64 strip, above the advisory line), on the
# box's black, squared
art = cover.crop((14, 6, 286, 246))
# without the publisher's badge in the top-left corner (cut in half by the crop)
art.paste((0, 0, 0), (0, 0, 64, 106))
side = max(art.size)
tile = Image.new("RGB", (side, side), (0, 0, 0))
tile.paste(art, ((side - art.width) // 2, (side - art.height) // 2))
tile.resize((512, 512), Image.LANCZOS).save(f"{out}/icon0.png")

# the background: the gameplay shot over the whole screen, softened and darkened, the box on the right
W, H = 1920, 1080
scale = max(W / shot.width, H / shot.height)
bg = shot.resize((round(shot.width * scale), round(shot.height * scale)), Image.LANCZOS)
left, top = (bg.width - W) // 2, (bg.height - H) // 2
bg = bg.crop((left, top, left + W, top + H)).filter(ImageFilter.GaussianBlur(6))
bg = ImageEnhance.Brightness(bg).enhance(0.6)
box_h = 860
box = cover.resize((round(cover.width * box_h / cover.height), box_h), Image.LANCZOS)
shadow = Image.new("RGB", (box.width + 24, box.height + 24), (0, 0, 0))
x, y = W - box.width - 140, (H - box_h) // 2
bg.paste(shadow, (x - 12 + 10, y - 12 + 10))
bg.paste(box, (x, y))
bg.save(f"{out}/pic1.png")
