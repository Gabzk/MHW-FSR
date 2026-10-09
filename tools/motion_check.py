"""Checks the game's motion vectors against real motion in a frame dump taken while moving.

usage: motion_check.py <dump dir> [render scale]
Reads frame_<i>.bin (luminance), motion_<i>.bin (float2, render pixels, current -> previous),
depth_<i>.bin and frames.txt (camera jitter per frame) from the plugin's DebugDumpFrames.

1. Scale: frame i-1 is warped onto frame i with s * mv plus the jitter difference, for a range of
   s. DLSS wants the error to be lowest at s = 1. The same is done without the jitter term.
2. Dilation: on the background side of depth edges, the vector of the pixel right next to the
   edge is compared with the foreground vector and with the background vector two pixels further
   out. Dilated vectors (foreground motion leaking onto the background) should not be sent to
   DLSS, which dilates them itself.
"""
import os
import sys

import numpy as np


def load(path, channels=1):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        a = np.frombuffer(f.read(), dtype=np.float32)
    return a.reshape(int(h), int(w), channels) if channels > 1 else a.reshape(int(h), int(w))


def bilinear(img, x, y):
    h, w = img.shape
    valid = (x >= 0) & (x <= w - 1.001) & (y >= 0) & (y <= h - 1.001)
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    v = ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x0 + 1] +
         (1 - fx) * fy * img[y0 + 1, x0] + fx * fy * img[y0 + 1, x0 + 1])
    return v, valid


def robust_error(a, b, valid):
    e = np.abs(a - b)[valid]
    if e.size == 0:
        return float("nan")
    e = np.sort(e)
    return float(e[: int(e.size * 0.9)].mean())  # ignore the worst 10% (disocclusions, effects)


def main(d, render_scale):
    jit = {}
    with open(os.path.join(d, "frames.txt")) as f:
        for line in f:
            i, jx, jy = line.split()[:3]
            # Camera jitter h moves the image by (h.x / 2, -h.y / 2) * render scale render pixels.
            jit[int(i)] = np.array([float(jx) / 2, -float(jy) / 2]) * render_scale
    n = len([f for f in os.listdir(d) if f.startswith("frame_")])
    scales = [0.0, 0.5, 0.8, 0.9, 0.95, 1.0, 1.05, 1.1, 1.2, 1.5, 2.0]
    totals = {s: [] for s in scales}
    no_jitter = []
    dil_scores = []
    for i in range(1, n):
        prev = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i - 1}.bin")), 0) * 50)
        cur = np.log1p(np.maximum(load(os.path.join(d, f"frame_{i}.bin")), 0) * 50)
        mv = load(os.path.join(d, f"motion_{i}.bin"), 2)
        h, w = cur.shape
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
        dj = jit.get(i - 1, np.zeros(2)) - jit.get(i, np.zeros(2))
        mag = np.hypot(mv[..., 0], mv[..., 1])
        row = []
        for s in scales:
            warped, valid = bilinear(prev, xx + s * mv[..., 0] + dj[0], yy + s * mv[..., 1] + dj[1])
            err = robust_error(warped, cur, valid)
            totals[s].append(err)
            row.append(f"{s:.2f}:{err:.4f}")
        warped, valid = bilinear(prev, xx + mv[..., 0], yy + mv[..., 1])
        no_jitter.append(robust_error(warped, cur, valid))
        best = min(scales, key=lambda s: totals[s][-1])
        print(f"pair {i - 1}->{i}: |mv| mean {mag.mean():.2f} max {mag.max():.2f}, jitter diff {dj[0]:+.3f},{dj[1]:+.3f}; "
              f"best s {best:.2f}; " + " ".join(row))

        depth_path = os.path.join(d, f"depth_{i}.bin")
        if os.path.exists(depth_path):
            dep = load(depth_path)
            agree_fg = total = 0
            for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                # p = background pixel, q = p + (dy, dx) foreground neighbour, p2 = p - 2 * (dy, dx).
                ys, xs = np.mgrid[3:h - 3, 3:w - 3]
                p_d = dep[ys, xs]
                q_d = dep[ys + dy, xs + dx]
                p2_d = dep[ys - 2 * dy, xs - 2 * dx]
                # Reversed Z: closer = larger. Foreground clearly closer, p and p2 on the same surface.
                edge = (q_d > p_d * 1.3 + 1e-6) & (np.abs(p2_d - p_d) < 0.1 * p_d + 1e-7)
                mv_p = mv[ys, xs]
                mv_q = mv[ys + dy, xs + dx]
                mv_p2 = mv[ys - 2 * dy, xs - 2 * dx]
                distinct = np.linalg.norm(mv_q - mv_p2, axis=-1) > 0.5
                sel = edge & distinct
                if sel.any():
                    d_fg = np.linalg.norm(mv_p - mv_q, axis=-1)[sel]
                    d_bg = np.linalg.norm(mv_p - mv_p2, axis=-1)[sel]
                    agree_fg += int((d_fg < d_bg).sum())
                    total += int(sel.sum())
            if total:
                dil_scores.append(agree_fg / total)
                print(f"    depth edges with distinct motion: {total}, background pixel next to the edge moves "
                      f"like the foreground in {100 * agree_fg / total:.0f}%")
    if any(totals[1.0]):
        mean = {s: np.nanmean(totals[s]) for s in scales}
        best = min(mean, key=mean.get)
        print("mean error per scale: " + " ".join(f"{s:.2f}:{mean[s]:.4f}" for s in scales))
        print(f"best scale {best:.2f} (want 1.00); error at 1.00 with jitter term {mean[1.0]:.4f}, "
              f"without {np.nanmean(no_jitter):.4f}")
    if dil_scores:
        print(f"dilation: background pixels next to edges follow the foreground in {100 * np.mean(dil_scores):.0f}% "
              f"(about 0% = not dilated, about 100% = dilated by a pixel or more)")


if __name__ == "__main__":
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 1.0)
