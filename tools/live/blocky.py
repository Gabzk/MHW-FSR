"""Render-grid blockiness of a capture region at a 2x upscale (Performance).

usage: blocky.py x0,y0,x1,y1 img...
Compares brightness steps across the boundaries of 2x2 output-pixel blocks with steps inside
them, over all phases (the render grid's phase in the output is unknown), and reports the
largest ratio. Smooth images give about 1; blocky (render-res) edges give clearly more.
Only pixels on edges of bright areas count (one side above the threshold).
"""
import sys

import numpy as np
from PIL import Image

x0, y0, x1, y1 = (int(v) for v in sys.argv[1].split(","))
for path in sys.argv[2:]:
    a = np.asarray(Image.open(path).convert("L"), dtype=float)[y0:y1, x0:x1]
    res = []
    for axis in (0, 1):
        d = np.abs(np.diff(a, axis=axis))
        hi = np.maximum(a[:-1, :] if axis == 0 else a[:, :-1], a[1:, :] if axis == 0 else a[:, 1:])
        sel = hi > 200
        best = 0.0
        for phase in (0, 1):
            idx = np.arange(d.shape[axis])
            across = (idx % 2) == phase
            if axis == 0:
                m_a, m_w = sel & across[:, None], sel & ~across[:, None]
            else:
                m_a, m_w = sel & across[None, :], sel & ~across[None, :]
            r = d[m_a].mean() / max(d[m_w].mean(), 1e-6)
            best = max(best, r)
        res.append(best)
    print(f"{path}: blockiness x {res[1]:.2f} y {res[0]:.2f}")
