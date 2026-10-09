"""Find direct call/jmp rel32 (E8/E9) instructions targeting VAs.  usage: callfind.py <exe> <va> [...]"""
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
imagebase = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
targets = {int(a, 0) for a in sys.argv[2:]}
sec = pe + 24 + optsize
for _ in range(nsec):
    name = data[sec:sec + 8].rstrip(b"\0").decode(errors="replace")
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    flags = struct.unpack_from("<I", data, sec + 36)[0]
    sec += 40
    if not flags & 0x20000000:  # executable
        continue
    base = imagebase + va
    for op in (0xE8, 0xE9):
        pos = rawptr
        end = rawptr + rawsize - 5
        while True:
            pos = data.find(bytes([op]), pos, end)
            if pos < 0:
                break
            rel = struct.unpack_from("<i", data, pos + 1)[0]
            ins = base + (pos - rawptr)
            if ins + 5 + rel in targets:
                print(f"{name} {ins:#x} {'call' if op == 0xE8 else 'jmp'} {ins + 5 + rel:#x}")
            pos += 1
