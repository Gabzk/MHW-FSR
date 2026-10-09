"""Reads the game's anti-aliasing state in the running game (MHW:I 15.23, image base 0x140000000).

scene+0xe9e4 TAA / +0xe9e5 FXAA (set by 0x142287a00 from the AA option), the enable bit 0x2 in
+0x14 of the TAA2 and FXAA filter units (set by the AA apply 0x14043dcb0), the gate the FXAA draw
checks first ([0x1451c5320]+0x2cda), and the renderer's DLSS fields.
usage: aapeek.py
"""
import ctypes
import struct
import subprocess

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
pid = int(subprocess.check_output(
    ["powershell", "-NoProfile", "-Command", "(Get-Process MonsterHunterWorld).Id"]).decode().strip())
h = k32.OpenProcess(0x0410, False, pid)


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw


def q(a):
    d = read(a, 8)
    return struct.unpack("<Q", d)[0] if d else None


def u32(a):
    d = read(a, 4)
    return struct.unpack("<I", d)[0] if d else None


def u8(a):
    d = read(a, 1)
    return d[0] if d else None


def f32(a):
    d = read(a, 4)
    return struct.unpack("<f", d)[0] if d else None


assert read(0x140000000, 2) == b"MZ", "unexpected image base"
VT = {0x14351c1a8: "uFXAAFilter", 0x14351bda8: "uTemporalAAFilter2"}

scene = q(0x1451c4368)
print(f"scene {scene:#x}: TAA {u8(scene + 0xe9e4)}, FXAA {u8(scene + 0xe9e5)}")
gate = q(0x1451c5320)
print(f"FXAA draw gate [0x1451c5320]={gate:#x} +0x2cda = {u8(gate + 0x2cda)}")
r = q(0x1451c4480)
print(f"renderer {r:#x}: DLSS on {u8(r + 0xa5)}, DLISP on {u8(r + 0xa6)}, mode {u32(r + 0xa8)}, "
      f"feature bits {u32(r + 0xb4)}, render scale {f32(r + 0x200):.3f}/{f32(r + 0x204):.3f}")

lst = q(0x1451238c8)
node = q(lst + 0x15d0)
n = 0
while node and n < 400:
    vt = q(node)
    flags = u32(node + 0x14)
    name = VT.get(vt)
    if name:
        print(f"unit {node:#x} {name}: flags {flags:#x} (enable bit 0x2 = {bool(flags & 2)})")
    n += 1
    node = q(node + 0x30)
print(f"{n} units in the list")
