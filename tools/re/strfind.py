"""Find ASCII and UTF-16LE strings matching a regex in a PE; prints VA, encoding, string.

usage: strfind.py <exe> <regex> [maxlen]
"""
import re
import struct
import sys

path, pat = sys.argv[1], sys.argv[2]
maxlen = int(sys.argv[3]) if len(sys.argv) > 3 else 160
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


def off2va(off):
    for name, va, vsize, rawptr, rawsize in secs:
        if rawptr <= off < rawptr + rawsize:
            return imagebase + va + off - rawptr, name
    return None, None


rx = re.compile(pat, re.I)
for m in re.finditer(rb"[\x20-\x7e]{3,}", data):
    s = m.group().decode()
    if rx.search(s):
        va, name = off2va(m.start())
        print(f"{va:#x} {name} a {s[:maxlen]}" if va else f"off {m.start():#x} a {s[:maxlen]}")
for m in re.finditer(rb"(?:[\x20-\x7e]\x00){3,}", data):
    s = m.group().decode("utf-16le")
    if rx.search(s):
        va, name = off2va(m.start())
        print(f"{va:#x} {name} w {s[:maxlen]}" if va else f"off {m.start():#x} w {s[:maxlen]}")
