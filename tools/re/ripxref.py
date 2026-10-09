"""Find RIP-relative references (disp32 with ModRM mod=00 rm=101) to target VAs in a PE's .text.

usage: ripxref.py <exe> <va> [<va> ...]
Prints the VA of each candidate instruction start (heuristic: opcode byte before the ModRM byte).
"""
import struct
import sys

path = sys.argv[1]
targets = {int(a, 0) for a in sys.argv[2:]}
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
imagebase = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
sec = pe + 24 + optsize
for i in range(nsec):
    name = data[sec:sec + 8].rstrip(b"\0").decode()
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    sec += 40
    if name != ".text":
        continue
    text = data[rawptr:rawptr + rawsize]
    base = imagebase + va
    # ModRM byte with mod=00, rm=101: (b & 0xC7) == 0x05
    for off in range(1, len(text) - 4):
        b = text[off]
        if (b & 0xC7) != 0x05:
            continue
        disp = struct.unpack_from("<i", text, off + 1)[0]
        # instruction end = modrm + 4 (+ imm for some opcodes; ignore those)
        for extra in (0, 1, 4):
            target = base + off + 5 + extra + disp
            if target in targets:
                op = text[off - 1]
                start = off - 1
                if start > 0 and text[start - 1] in (0x48, 0x4C, 0x49, 0x4D, 0x44, 0x41, 0x40) :
                    start -= 1
                print(f"{base + start:#x} -> {target:#x} (opcode {op:#04x}, imm {extra})")
