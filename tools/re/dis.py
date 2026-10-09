"""Disassemble a PE range with capstone; annotates RIP-relative targets (strings, qwords).

usage: dis.py <exe> <va> <len> [--func]   (--func: start at the pdata function containing va, len = its size)
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

path, start, length = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
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
    e = data.find(b"\0", o, o + 120)
    if e < 0:
        return None
    s = data[o:e]
    if len(s) >= 3 and all(0x20 <= c < 0x7f for c in s):
        return s.decode()
    return None


if "--func" in sys.argv:
    p = next(x for x in secs if x[0] == ".pdata")
    pd = data[p[3]:p[3] + p[4]]
    rva = start - imagebase
    for i in range(0, len(pd) - 12, 12):
        b, e, u = struct.unpack_from("<III", pd, i)
        if b <= rva < e:
            start, length = imagebase + b, e - b
            print(f"; function {start:#x}..{imagebase + e:#x}")
            break

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
o = va2off(start)
for ins in md.disasm(data[o:o + length], start):
    note = ""
    if "rip" in ins.op_str:
        import re
        m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
        if m:
            t = ins.address + ins.size + (int(m.group(2), 16) if m.group(1) == "+" else -int(m.group(2), 16))
            s = cstr(t)
            note = f"  ; {t:#x}" + (f' "{s}"' if s else "")
    print(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}{note}")
