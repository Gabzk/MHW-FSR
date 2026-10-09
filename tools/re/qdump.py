"""Dump qwords of a PE range; resolves pointers to strings.  usage: qdump.py <exe> <va> <count>"""
import struct
import sys

path, start, count = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
imagebase = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
secs = []
sec = pe + 24 + optsize
for _ in range(nsec):
    name = data[sec:sec + 8].rstrip(b"\0").decode()
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    secs.append((name, va, vsize, rawptr, rawsize))
    sec += 40


def va2off(v):
    for name, va, vsize, rawptr, rawsize in secs:
        if imagebase + va <= v < imagebase + va + rawsize:
            return rawptr + v - imagebase - va
    return None


def cstr(v):
    o = va2off(v)
    if o is None:
        return None
    e = data.find(b"\0", o, o + 200)
    if e < 0:
        return None
    s = data[o:e]
    if len(s) >= 2 and all(0x20 <= c < 0x7f for c in s):
        return s.decode()
    return None


o = va2off(start)
for i in range(count):
    q = struct.unpack_from("<Q", data, o + i * 8)[0]
    s = cstr(q) if imagebase <= q < imagebase + 0x6000000 else None
    lo, hi = q & 0xffffffff, q >> 32
    extra = f'"{s}"' if s else f"(dwords {lo:#x} {hi:#x})"
    print(f"{start + i * 8:#x}: {q:#018x} {extra}")
