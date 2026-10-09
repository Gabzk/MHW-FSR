"""Find 8-byte absolute pointers to target VAs anywhere in the PE file.  usage: ptrfind.py <exe> <va> [...]"""
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
for i in range(nsec):
    name = data[sec:sec + 8].rstrip(b"\0").decode()
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
    secs.append((name, va, rawsize, rawptr))
    sec += 40


def off2va(off):
    for name, va, size, raw in secs:
        if raw <= off < raw + size:
            return imagebase + va + off - raw, name
    return None, None


for a in sys.argv[2:]:
    needle = struct.pack("<Q", int(a, 0))
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        va, name = off2va(i)
        print(f"{a}: pointer at {va:#x} ({name})" if va else f"{a}: file offset {i:#x}")
        start = i + 1
