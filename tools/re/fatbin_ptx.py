"""Extracts the PTX modules embedded in CUDA fatbins of a binary (LZ4-compressed entries too).

usage: fatbin_ptx.py <binary> <out dir> [kernel substring]
Writes every PTX module whose entry names contain the substring (all when omitted).
"""
import os
import re
import struct
import sys


def lz4_block(src, size):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n or len(out) >= size:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        for k in range(ml):  # overlapping copies
            out.append(out[start + k])
    return bytes(out[:size])


data = open(sys.argv[1], "rb").read()
outdir = sys.argv[2]
want = sys.argv[3] if len(sys.argv) > 3 else ""
os.makedirs(outdir, exist_ok=True)
count = 0
for m in re.finditer(rb"\x50\xed\x55\xba", data):
    base = m.start()
    magic, ver, hsize, fsize = struct.unpack_from("<IHHQ", data, base)
    e = base + hsize
    end = base + hsize + fsize
    while e < end:
        kind, _, ehsize, padded, csize, _, minor, major, arch = struct.unpack_from("<HHIQIIHHI", data, e)
        flags, _, dsize = struct.unpack_from("<QQQ", data, e + 40)
        if ehsize == 0 or padded == 0:
            break
        payload = data[e + ehsize:e + ehsize + (csize if flags & 0x2000 else padded)]
        if kind == 1:
            text = lz4_block(payload, dsize) if flags & 0x2000 else payload
            text = text.split(b"\x00", 1)[0].decode("ascii", "replace")
            entries = re.findall(r"\.entry\s+(\w+)", text)
            print(f"fatbin {base:#x} PTX {major}.{minor} sm_{arch} {len(text)} chars: {entries[:6]}")
            if not want or any(want in x for x in entries):
                path = os.path.join(outdir, f"ptx_{base:x}_sm{arch}.ptx")
                open(path, "w", newline="").write(text)
                count += 1
        e += ehsize + padded
print(count, "written")
