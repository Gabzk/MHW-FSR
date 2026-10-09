"""Checks the direction and scale of the game's motion vectors against the rendered frames.

usage: mv_analysis.py <dump dir>
Uses frame_<i>.bin / motion_<i>.bin from the plugin's DebugDumpFrames (best with a fixed
jitter so frames only differ by real motion). For each frame n it warps frame n-1 with
s * mv_n (sampling frame n-1 at x + s * mv_n(x)) for several s and reports the error on
moving pixels. DLSS wants vectors that point from the current pixel to its previous
position, i.e. the best s should be +1.
"""
import os
import sys

import numpy as np


def load(path, channels):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        a = np.frombuffer(f.read(), dtype=np.float32)
    return a.reshape(int(h), int(w), channels) if channels > 1 else a.reshape(int(h), int(w))


def bilinear(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    return ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x0 + 1] +
            (1 - fx) * fy * img[y0 + 1, x0] + fx * fy * img[y0 + 1, x0 + 1])


def main(d):
    n = len([f for f in os.listdir(d) if f.startswith("frame_")])
    scales = [-2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0]
    totals = {s: 0.0 for s in scales}
    count = 0
    for i in range(1, n):
        prev = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i - 1}.bin"), 1), 0) * 50)
        cur = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i}.bin"), 1), 0) * 50)
        mv = load(os.path.join(d, f"motion_{i}.bin"), 2)
        mag = np.hypot(mv[..., 0], mv[..., 1])
        mask = mag > 0.3
        if mask.sum() < 200:
            continue
        h, w = cur.shape
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
        row = []
        for s in scales:
            warped = bilinear(prev, xx + s * mv[..., 0], yy + s * mv[..., 1])
            err = float(np.mean(np.abs(warped - cur)[mask]))
            totals[s] += err
            row.append(f"{s:+.1f}:{err:.4f}")
        count += 1
        print(f"frame {i}: moving px {int(mask.sum())}, mean |mv| {mag[mask].mean():.2f}  " + "  ".join(row))
    if count:
        best = min(totals, key=totals.get)
        print("mean error per scale: " + "  ".join(f"{s:+.1f}:{totals[s] / count:.4f}" for s in scales))
        print(f"best scale {best:+.1f}")


if __name__ == "__main__":
    main(sys.argv[1])
