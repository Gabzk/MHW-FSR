"""Relates the game's jitter values to the actual sub-pixel motion of the rendered image.

usage: jitter_analysis.py <dump dir>
Reads frame_<i>.bin (uint32 w, uint32 h, float32 luminance) and frames.txt (i jx jy x0 y0)
written by the plugin's DebugDumpFrames. For each consecutive pair it estimates the global
image shift with Lucas-Kanade on many blocks (median, so moving foliage does not dominate)
and fits shift = k * delta_jitter per axis. k ~ +1 means content moves with the jitter value,
k ~ -1 means it moves against it.
"""
import os
import sys

import numpy as np


def load(path):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        return np.frombuffer(f.read(), dtype=np.float32).reshape(int(h), int(w))


def block_shifts(a, b, block=64):
    """Median Lucas-Kanade shift (dx, dy) of b relative to a over well-textured blocks."""
    a = np.log1p(np.maximum(a, 0) * 50.0)
    b = np.log1p(np.maximum(b, 0) * 50.0)
    gy, gx = np.gradient(0.5 * (a + b))
    dt = b - a
    shifts = []
    h, w = a.shape
    for y in range(8, h - block - 8, block):
        for x in range(8, w - block - 8, block):
            sx, sy, st = gx[y:y + block, x:x + block], gy[y:y + block, x:x + block], dt[y:y + block, x:x + block]
            m = np.array([[np.sum(sx * sx), np.sum(sx * sy)], [np.sum(sx * sy), np.sum(sy * sy)]])
            if np.linalg.cond(m) > 50 or np.trace(m) < 1e-3:
                continue
            v = -np.linalg.solve(m, np.array([np.sum(sx * st), np.sum(sy * st)]))
            if np.all(np.abs(v) < 1.5):
                shifts.append(v)
    if not shifts:
        return None
    return np.median(np.array(shifts), axis=0), len(shifts)


def main(d):
    rows = [l.split() for l in open(os.path.join(d, "frames.txt")) if l.strip()]
    jit = np.array([[float(r[1]), float(r[2])] for r in rows])
    frames = [load(os.path.join(d, f"frame_{int(r[0])}.bin")) for r in rows]
    dj, ds = [], []
    for i in range(len(frames) - 1):
        res = block_shifts(frames[i], frames[i + 1])
        if res is None:
            continue
        s, n = res
        d_j = jit[i + 1] - jit[i]
        dj.append(d_j)
        ds.append(s)
        print(f"{i}->{i + 1}: jitter delta ({d_j[0]:+.4f}, {d_j[1]:+.4f})  image shift ({s[0]:+.4f}, {s[1]:+.4f})  blocks {n}")
    dj, ds = np.array(dj), np.array(ds)
    for axis, name in ((0, "x"), (1, "y")):
        k = np.sum(dj[:, axis] * ds[:, axis]) / max(np.sum(dj[:, axis] ** 2), 1e-9)
        resid = ds[:, axis] - k * dj[:, axis]
        print(f"axis {name}: image shift = {k:+.3f} * jitter delta  (residual rms {np.sqrt(np.mean(resid ** 2)):.4f} px)")


if __name__ == "__main__":
    main(sys.argv[1])
