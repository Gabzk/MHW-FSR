"""Do specular highlights follow the camera jitter like the geometry they sit on?

usage: specshift.py <dump dir> <render scale> x0,y0,x1,y1 [mindepth]
In the box (crop pixels, optionally depth > mindepth), splits textured pixels into "highlight"
(top 3% brightness of the box in frame i) and "rest". For each consecutive pair, fits the shift
that best aligns frame i-1 to frame i (after MVs scaled by 0.5,-0.5) separately for both sets,
and prints them next to the jitter shift. Also prints the alignment error at the jitter shift
for both sets, relative to the brightness of the set (flicker that jitter cannot explain).
"""
import os
import sys

import numpy as np

d, rs, box = sys.argv[1], float(sys.argv[2]), [int(v) for v in sys.argv[3].split(",")]
mind = float(sys.argv[4]) if len(sys.argv) > 4 else None
MV = np.array([0.5, -0.5])


def load(path, ch=1):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        a = np.frombuffer(f.read(), dtype=np.float32)
    return a.reshape(int(h), int(w), ch) if ch > 1 else a.reshape(int(h), int(w))


def sample(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    return ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x0 + 1] +
            (1 - fx) * fy * img[y0 + 1, x0] + fx * fy * img[y0 + 1, x0 + 1])


def fit(prev, cur_v, px, py):
    def err(sx, sy):
        e = np.abs(sample(prev, px + sx, py + sy) - cur_v)
        k = max(1, int(e.size * 0.9))
        return np.partition(e, k - 1)[:k].mean()
    best = (1e9, 0.0, 0.0)
    for sy in np.arange(-1.0, 1.01, 0.1):
        for sx in np.arange(-1.0, 1.01, 0.1):
            e = err(sx, sy)
            if e < best[0]:
                best = (e, sx, sy)
    c = best
    for sy in np.arange(c[2] - 0.1, c[2] + 0.101, 0.02):
        for sx in np.arange(c[1] - 0.1, c[1] + 0.101, 0.02):
            e = err(sx, sy)
            if e < best[0]:
                best = (e, sx, sy)
    return best, err


jit = {}
for line in open(os.path.join(d, "frames.txt")):
    i, jx, jy = line.split()[:3]
    jit[int(i)] = np.array([float(jx) / 2, -float(jy) / 2]) * rs
n = len([f for f in os.listdir(d) if f.startswith("frame_")])
x0, y0, x1, y1 = box
for i in range(2, n):  # frame 0 predates the reload, frame 1 has no jitter record
    prev_raw = load(os.path.join(d, f"frame_{i - 1}.bin"))
    cur_raw = load(os.path.join(d, f"frame_{i}.bin"))
    prev, cur = np.log1p(np.maximum(prev_raw, 0) * 50), np.log1p(np.maximum(cur_raw, 0) * 50)
    mv = load(os.path.join(d, f"motion_{i}.bin"), 2) * MV
    dep = load(os.path.join(d, f"depth_{i}.bin"))
    gy, gx = np.gradient(cur)
    m = np.zeros(cur.shape, bool)
    m[y0:y1, x0:x1] = True
    if mind is not None:
        m &= dep > mind
    thr = np.percentile(cur_raw[m], 97)
    hl = m & (cur_raw >= thr)
    rest = m & (cur_raw < thr) & (np.hypot(gx, gy) > 0.05)
    dj = jit[i - 1] - jit[i]
    out = []
    for name, sel in (("highlight", hl), ("rest", rest)):
        ys, xs = np.nonzero(sel)
        px, py = xs + mv[ys, xs, 0], ys + mv[ys, xs, 1]
        best, err = fit(prev, cur[ys, xs], px, py)
        ej = err(dj[0], dj[1])
        out.append(f"{name} {ys.size:5d} px: best {best[1]:+.2f},{best[2]:+.2f} err {best[0]:.3f}, at jitter {ej:.3f}")
    print(f"pair {i - 1}->{i} jitter {dj[0]:+.2f},{dj[1]:+.2f} | " + " | ".join(out))
