"""Primary function start for addresses (follows chained unwind info), plus direct callers and
pointers to it.  usage: funcroot.py <exe> <va> [...]"""
import struct
import sys

path = sys.argv[1]
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


def rva2off(r):
    for name, va, vsize, rawptr, rawsize in secs:
        if va <= r < va + rawsize:
            return rawptr + r - va
    return None


p = next(x for x in secs if x[0] == ".pdata")
pd = data[p[3]:p[3] + p[4]]
entries = [struct.unpack_from("<III", pd, i) for i in range(0, len(pd) - 11, 12)]
entries = [e for e in entries if e[0]]


def entry_for(rva):
    lo, hi = 0, len(entries) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e, u = entries[mid]
        if rva < b:
            hi = mid - 1
        elif rva >= e:
            lo = mid + 1
        else:
            return entries[mid]
    return None


def root(rva):
    ent = entry_for(rva)
    while ent:
        b, e, u = ent
        o = rva2off(u & ~3 if u & 1 == 0 else u)
        ver_flags, prolog, ncodes, frame = data[o], data[o + 1], data[o + 2], data[o + 3]
        flags = ver_flags >> 3
        if not flags & 4:  # UNW_FLAG_CHAININFO
            return b
        co = o + 4 + ((ncodes + 1) & ~1) * 2
        b2, e2, u2 = struct.unpack_from("<III", data, co)
        ent = (b2, e2, u2)
    return None


text = next(x for x in secs if x[0] == ".text")
tdata = data[text[3]:text[3] + text[4]]
tbase = imagebase + text[1]
for a in sys.argv[2:]:
    va = int(a, 0)
    r = root(va - imagebase)
    if r is None:
        print(f"{va:#x}: no pdata")
        continue
    f = imagebase + r
    callers = []
    for i in range(len(tdata) - 5):
        if tdata[i] in (0xE8, 0xE9):
            if tbase + i + 5 + struct.unpack_from("<i", tdata, i + 1)[0] == f:
                callers.append(tbase + i)
    ptrs = [imagebase + secs[j][1] + m - secs[j][3]
            for j in range(len(secs)) for m in range(secs[j][3], secs[j][3] + secs[j][4] - 7, 8)
            if struct.unpack_from("<Q", data, m)[0] == f] if "--ptr" in sys.argv else []
    print(f"{va:#x}: in function {f:#x}; callers {[hex(c) for c in callers]}" + (f"; pointers {[hex(x) for x in ptrs]}" if ptrs else ""))
