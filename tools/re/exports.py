"""List a PE's exports whose names match a regex, with their VAs.  usage: exports.py <pe> <regex>"""
import re
import struct
import sys

path, pat = sys.argv[1], re.compile(sys.argv[2])
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
opt = pe + 24
imagebase = struct.unpack_from("<Q", data, opt + 24)[0]
exp_rva = struct.unpack_from("<I", data, opt + 112)[0]
secs = []
sec = opt + optsize
for _ in range(nsec):
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    secs.append((va, max(vsize, rawsize), rawptr))
    sec += 40


def off(rva):
    for va, size, rawptr in secs:
        if va <= rva < va + size:
            return rawptr + rva - va
    raise ValueError(hex(rva))


e = off(exp_rva)
nfuncs, nnames, afuncs, anames, aords = struct.unpack_from("<IIIII", data, e + 20)
for i in range(nnames):
    name_rva = struct.unpack_from("<I", data, off(anames) + 4 * i)[0]
    o = off(name_rva)
    name = data[o:data.index(b"\0", o)].decode()
    if not pat.search(name):
        continue
    ordinal = struct.unpack_from("<H", data, off(aords) + 2 * i)[0]
    rva = struct.unpack_from("<I", data, off(afuncs) + 4 * ordinal)[0]
    print(f"{imagebase + rva:#x} {name}")
