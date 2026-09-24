#!/usr/bin/env python3
"""thumbs.py <out.png> <png>...: a side-by-side strip of 540p-high thumbnails with labels."""
import sys
from PIL import Image, ImageDraw
out, files = sys.argv[1], sys.argv[2:]
thumbs = []
for f in files:
    im = Image.open(f).convert("RGB")
    h = 540
    w = max(1, im.width * h // im.height)
    t = im.resize((w, h))
    ImageDraw.Draw(t).text((8, 8), f.split("/")[-1], fill=(255, 0, 255))
    thumbs.append(t)
strip = Image.new("RGB", (sum(t.width for t in thumbs) + 8 * (len(thumbs) - 1), 540), (40, 40, 40))
x = 0
for t in thumbs:
    strip.paste(t, (x, 0))
    x += t.width + 8
strip.save(out)
print(out, strip.size)
