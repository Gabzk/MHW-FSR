"""Read raw texture dumps written by rdq.py scripts (NAME.bin + NAME.json) into numpy arrays.

  from texio import load
  img = load(r"...\\rds\\ssr_hist")   # float32 HxWxC (UNORM/SNORM/FLOAT formats decoded)
"""
import json

import numpy as np


def _r11g11b10(raw):
    v = raw.view(np.uint32)

    def unpack(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = (bits & ((1 << mbits) - 1)).astype(np.float32)
        out = np.where(e == 0, m / (1 << mbits) * 2.0 ** -14,
                       (1.0 + m / (1 << mbits)) * np.exp2(e.astype(np.float32) - 15))
        return np.where(e == 31, np.inf, out).astype(np.float32)

    r = unpack(v & 0x7FF, 6)
    g = unpack((v >> 11) & 0x7FF, 6)
    b = unpack((v >> 22) & 0x3FF, 5)
    return np.stack([r, g, b], axis=-1)


def load(base):
    meta = json.load(open(base + ".json"))
    raw = np.fromfile(base + ".bin", dtype=np.uint8)
    w, h, fmt = meta["w"], meta["h"], meta["fmt"]
    n = w * h
    if fmt == "R11G11B10_FLOAT":
        return _r11g11b10(raw[: n * 4]).reshape(h, w, 3)
    if fmt in ("R16G16B16A16_FLOAT", "R16G16B16A16_TYPELESS"):
        return raw[: n * 8].view(np.float16).astype(np.float32).reshape(h, w, 4)
    if fmt in ("R32_FLOAT", "R32_TYPELESS"):
        return raw[: n * 4].view(np.float32).reshape(h, w)
    if fmt in ("R16_FLOAT",):
        return raw[: n * 2].view(np.float16).astype(np.float32).reshape(h, w)
    if fmt in ("R16G16_FLOAT", "R16G16_TYPELESS"):
        return raw[: n * 4].view(np.float16).astype(np.float32).reshape(h, w, 2)
    if fmt in ("R8G8B8A8_UNORM", "R8G8B8A8_TYPELESS", "B8G8R8A8_UNORM", "R8G8B8A8_UNORM_SRGB"):
        return raw[: n * 4].reshape(h, w, 4).astype(np.float32) / 255.0
    if fmt in ("R8_UNORM",):
        return raw[:n].reshape(h, w).astype(np.float32) / 255.0
    if fmt in ("R8G8_SNORM",):
        return np.maximum(raw[: n * 2].view(np.int8).reshape(h, w, 2).astype(np.float32) / 127.0, -1.0)
    if fmt in ("R32_UINT",):
        return raw[: n * 4].view(np.uint32).reshape(h, w)
    if fmt in ("R10G10B10A2_UNORM", "R10G10B10A2_TYPELESS"):
        v = raw[: n * 4].view(np.uint32).reshape(h, w)
        return np.stack([(v & 1023) / 1023.0, ((v >> 10) & 1023) / 1023.0, ((v >> 20) & 1023) / 1023.0,
                         (v >> 30) / 3.0], axis=-1).astype(np.float32)
    raise ValueError("unsupported format " + fmt)
