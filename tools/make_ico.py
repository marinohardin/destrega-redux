#!/usr/bin/env python3
"""Build the Windows icon (runtime/win/destrega.ico) from assets/icon/destrega.png.

The PNG is the original procedural artwork (tools/make_icon.py, no game assets). It carries
the macOS Big Sur transparent margin; this crops to the artwork's bounding box first, since
Windows icons fill their square. Needs Pillow. The .ico is committed, so a normal build does
not need this; rerun it when the PNG changes.

usage: python3 tools/make_ico.py [assets/icon/destrega.png [runtime/win/destrega.ico]]
"""
import os
import sys

from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "assets/icon/destrega.png")
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "runtime/win/destrega.ico")

img = Image.open(src).convert("RGBA")
bbox = img.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
if bbox:
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    s = max(w, h)
    cx, cy = (bbox[0] + bbox[2]) // 2, (bbox[1] + bbox[3]) // 2
    img = img.crop((cx - s // 2, cy - s // 2, cx - s // 2 + s, cy - s // 2 + s))
img = img.resize((256, 256), Image.LANCZOS)
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
img.save(dst, format="ICO", sizes=sizes)
print(f"make_ico: {dst} ({os.path.getsize(dst)} bytes, {len(sizes)} sizes)")
