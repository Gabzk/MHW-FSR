"""Sharpness and temporal stability of captured bursts.

usage: metrics.py <prefix> [<prefix> ...]
Each prefix names files <prefix>-0.png, <prefix>-1.png, ... from burst.ps1.
Prints mean Laplacian energy (detail) and mean absolute difference between consecutive
frames (flicker), both on luma in 0..255.
"""
import glob
import sys

import numpy as np
from PIL import Image


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def laplacian_energy(y):
    lap = -4 * y[1:-1, 1:-1] + y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]
    return float(np.mean(np.abs(lap)))


for prefix in sys.argv[1:]:
    files = sorted(glob.glob(prefix + "-*.png"), key=lambda p: int(p.rsplit("-", 1)[1].split(".")[0]))
    # PrintWindow sometimes returns an all-black image (window in independent flip); skip those.
    frames = [f for f in (luma(f) for f in files) if f.mean() >= 1.0]
    sharp = np.mean([laplacian_energy(f) for f in frames])
    flicker = np.mean([np.mean(np.abs(a - b)) for a, b in zip(frames, frames[1:])]) if len(frames) > 1 else 0.0
    print(f"{prefix.rsplit(chr(92), 1)[-1]:>10}: detail {sharp:7.3f}  flicker {flicker:6.3f}  ({len(frames)} frames)")
