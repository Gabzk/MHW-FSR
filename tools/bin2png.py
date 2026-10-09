"""Converts the plugin's output.bin (uint32 w, h, float RGB) to a PNG with a simple tonemap.

usage: bin2png.py <output.bin> <out.png> [scale]
"""
import sys

import numpy as np
from PIL import Image

with open(sys.argv[1], "rb") as f:
    w, h = np.frombuffer(f.read(8), dtype=np.uint32)
    rgb = np.frombuffer(f.read(), dtype=np.float32).reshape(int(h), int(w), 3)
print(f"{w}x{h} min {rgb.min():.4f} max {rgb.max():.4f} mean {rgb.mean():.4f}")
ldr = (rgb / (1.0 + rgb)) ** (1 / 2.2)
img = Image.fromarray((np.clip(ldr, 0, 1) * 255).astype(np.uint8))
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
if scale != 1.0:
    img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
img.save(sys.argv[2])
