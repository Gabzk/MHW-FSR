"""Find instructions in .text whose memory operand displacement equals one of the given values.

usage: dispfind.py <exe> <disp> [...]   (scans for the disp32 bytes, then disassembles backwards
candidates and keeps instructions that end right after... simpler: decodes from a few start offsets
before each hit and prints the instruction that contains the hit as its displacement)
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

path = sys.argv[1]
disps = [int(a, 0) for a in sys.argv[2:]]
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
imagebase = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
sec = pe + 24 + optsize
text = None
for _ in range(nsec):
    name = data[sec:sec + 8].rstrip(b"\0").decode()
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    if name == ".text":
        text = (va, rawptr, rawsize)
    sec += 40
va, rawptr, rawsize = text
t = data[rawptr:rawptr + rawsize]
base = imagebase + va
md = Cs(CS_ARCH_X86, CS_MODE_64)
seen = set()
for d in disps:
    pat = struct.pack("<I", d)
    i = t.find(pat)
    while i >= 0:
        for back in range(2, 10):
            s = i - back
            ins = next(md.disasm(t[s:s + 16], base + s), None)
            if ins and ins.address + ins.size > base + i + 3 and f"{d:#x}" in ins.op_str:
                if ins.address not in seen:
                    seen.add(ins.address)
                    print(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}")
                break
        i = t.find(pat, i + 1)
