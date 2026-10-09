"""Per-region sub-pixel shift between consecutive dumped frames, compared with the camera jitter.

usage: jitregion.py <dump dir> <render scale> <region spec>...
region spec: name:x0,y0,x1,y1[,mindepth]   (crop pixel box, optional reversed-depth minimum)
For each pair (i-1 -> i) and region, finds the shift (dx, dy) that best warps frame i-1 onto
frame i after the motion vectors (scaled by 0.5, -0.5), next to the expected jitter shift
d(i-1) - d(i) with d = (h.x / 2, -h.y / 2) * render scale. Unjittered content shows (0, 0).
"""
import os
import sys

import numpy as np

d = sys.argv[1]
rs = float(sys.argv[2])
MV = np.array([0.5, -0.5])
regions = []
for spec in sys.argv[3:]:
    name, nums = spec.split(":")
    v = [float(x) for x in nums.split(",")]
    regions.append((name, [int(x) for x in v[:4]], v[4] if len(v) > 4 else None))


def load(path, ch=1):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        a = np.frombuffer(f.read(), dtype=np.float32)
    return a.reshape(int(h), int(w), ch) if ch > 1 else a.reshape(int(h), int(w))


def sample(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    return ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x0 + 1] +
            (1 - fx) * fy * img[y0 + 1, x0] + fx * fy * img[y0 + 1, x0 + 1])


def err(prev, cur_v, px, py, sx, sy):
    e = np.abs(sample(prev, px + sx, py + sy) - cur_v)
    k = int(e.size * 0.9)
    return np.partition(e, k)[:k].mean()


jit = {}
for line in open(os.path.join(d, "frames.txt")):
    i, jx, jy = line.split()[:3]
    jit[int(i)] = np.array([float(jx) / 2, -float(jy) / 2]) * rs
n = len([f for f in os.listdir(d) if f.startswith("frame_")])

for i in range(1, n):
    prev = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i - 1}.bin")), 0) * 50)
    cur = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i}.bin")), 0) * 50)
    mv = load(os.path.join(d, f"motion_{i}.bin"), 2) * MV
    dep = load(os.path.join(d, f"depth_{i}.bin"))
    gy, gx = np.gradient(cur)
    textured = np.hypot(gx, gy) > 0.05
    dj = jit[i - 1] - jit[i]
    out = []
    for name, (x0, y0, x1, y1), mind in regions:
        m = np.zeros_like(textured)
        m[y0:y1, x0:x1] = True
        m &= textured
        if mind is not None:
            m &= dep > mind
        ys, xs = np.nonzero(m)
        if ys.size < 200:
            out.append(f"{name}: {ys.size} px")
            continue
        px = xs + mv[ys, xs, 0]
        py = ys + mv[ys, xs, 1]
        cv = cur[ys, xs]
        best = (1e9, 0.0, 0.0)
        for sy in np.arange(-1.0, 1.01, 0.1):
            for sx in np.arange(-1.0, 1.01, 0.1):
                e = err(prev, cv, px, py, sx, sy)
                if e < best[0]:
                    best = (e, sx, sy)
        c = best
        for sy in np.arange(c[2] - 0.1, c[2] + 0.101, 0.02):
            for sx in np.arange(c[1] - 0.1, c[1] + 0.101, 0.02):
                e = err(prev, cv, px, py, sx, sy)
                if e < best[0]:
                    best = (e, sx, sy)
        e0 = err(prev, cv, px, py, 0.0, 0.0)
        ej = err(prev, cv, px, py, dj[0], dj[1])
        mvmag = np.hypot(mv[ys, xs, 0], mv[ys, xs, 1]).mean()
        out.append(f"{name} ({ys.size} px, |mv| {mvmag:.2f}): best {best[1]:+.2f},{best[2]:+.2f} "
                   f"err {best[0]:.4f} | at jitter {ej:.4f} | at 0 {e0:.4f}")
    print(f"pair {i - 1}->{i}: jitter shift {dj[0]:+.2f},{dj[1]:+.2f}")
    for o in out:
        print("    " + o)
