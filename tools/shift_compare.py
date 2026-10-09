"""Median sub-pixel shift between captured bursts.

usage: shift_compare.py <reference prefix> <prefix> [<prefix> ...]
Each prefix names <prefix>-<i>.png from burst.ps1. Frames of a burst are averaged, then the shift of
each burst relative to the reference is estimated with block Lucas-Kanade (median over blocks).
"""
import glob
import sys

import numpy as np
from PIL import Image


def mean_luma(prefix):
    files = sorted(glob.glob(prefix + "-*.png"))
    acc = None
    for f in files:
        a = np.asarray(Image.open(f).convert("RGB"), dtype=np.float64)
        y = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
        acc = y if acc is None else acc + y
    return acc / len(files)


def shift(a, b, block=48):
    gy, gx = np.gradient(0.5 * (a + b))
    dt = b - a
    out = []
    h, w = a.shape
    for y in range(4, h - block - 4, block):
        for x in range(4, w - block - 4, block):
            sx, sy, st = gx[y:y + block, x:x + block], gy[y:y + block, x:x + block], dt[y:y + block, x:x + block]
            m = np.array([[np.sum(sx * sx), np.sum(sx * sy)], [np.sum(sx * sy), np.sum(sy * sy)]])
            if np.linalg.cond(m) > 30 or np.trace(m) < 50:
                continue
            v = -np.linalg.solve(m, np.array([np.sum(sx * st), np.sum(sy * st)]))
            if np.all(np.abs(v) < 2):
                out.append(v)
    if not out:
        return None, 0
    return np.median(np.array(out), axis=0), len(out)


ref = mean_luma(sys.argv[1])
for p in sys.argv[2:]:
    s, n = shift(ref, mean_luma(p))
    name = p.rsplit("\\", 1)[-1]
    print(f"{name:>16}: shift vs reference {('(%+.3f, %+.3f)' % (s[0], s[1])) if s is not None else 'n/a'}  blocks {n}")
