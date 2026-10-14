"""Reads (and optionally sets) the scene's SSLR switches in the running game (MHW:I 15.23).

scene = [0x1451c4368]. Reflected properties of the scene object (names from their registration in
0x142270c60): +0xb43e mGlobalEnvMapEachWaterAttn, +0xb43f mSSLRDitherResolve, +0xb440
mSSLRGBufferJitter. The SSLR resolve technique is picked from them by 0x14227ec90 (called from
0x142286260): FULL_RES, GJitter (+0xb440), Dither (+0xb43f), EachWaterAttn, EffectReflection.
usage: sslrpeek.py [gjitter=0|1] [dither=0|1]
"""
import ctypes
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
pid = int(subprocess.check_output(
    ["powershell", "-NoProfile", "-Command", "(Get-Process MonsterHunterWorld).Id"]).decode().strip())
h = k32.OpenProcess(0x0438, False, pid)  # VM_READ | VM_WRITE | VM_OPERATION | QUERY_INFORMATION


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw


def write(addr, data):
    got = ctypes.c_size_t()
    return k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(got))


assert read(0x140000000, 2) == b"MZ", "unexpected image base"
scene = struct.unpack("<Q", read(0x1451c4368, 8))[0]
OFF = {"waterattn": 0xb43e, "dither": 0xb43f, "gjitter": 0xb440}
for arg in sys.argv[1:]:
    k, v = arg.split("=")
    write(scene + OFF[k], bytes([int(v)]))
vals = {k: read(scene + o, 1)[0] for k, o in OFF.items()}
print(f"scene {scene:#x}: " + ", ".join(f"{k} {v}" for k, v in vals.items()))
# Trace resolution: the SSLR object at view + 0x630 (view = [renderer + 0x1f450]); its level at +0x228,
# capped by +0x22c or +0x230 (0x1425933b0, cap choice by scene + 0xe9a3). Level 0 picks the _FULL_RES
# techniques.
renderer = struct.unpack("<Q", read(0x1451c4480, 8))[0]
view = struct.unpack("<Q", read(renderer + 0x1f450, 8))[0]
sslr = struct.unpack("<Q", read(view + 0x630, 8))[0] if view else 0
if sslr:
    lvl, cap1, cap2 = struct.unpack("<III", read(sslr + 0x228, 12))
    print(f"view {view:#x} sslr {sslr:#x}: level {lvl}, caps {cap1}/{cap2}, scene+0xe9a3 {read(scene + 0xe9a3, 1)[0]}")
