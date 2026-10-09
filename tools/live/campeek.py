"""Reads the camera object of the running game: jitter at +0x38 and the first 0x400 bytes as floats."""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
pid = int(subprocess.check_output(
    ["powershell", "-NoProfile", "-Command", "(Get-Process MonsterHunterWorld).Id"]).decode().strip())
h = k32.OpenProcess(0x0410, False, pid)
mods = (wt.HMODULE * 1)()
need = wt.DWORD()
psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
base = mods[0]


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError(f"read {addr:#x} failed {ctypes.get_last_error()}")
    return buf.raw


q = lambda a: struct.unpack("<Q", read(a, 8))[0]
camera = q(base + int(sys.argv[1], 16))
renderer = q(base + int(sys.argv[2], 16))
scene = q(base + int(sys.argv[3], 16))
print(f"base {base:#x} camera {camera:#x} renderer {renderer:#x} scene {scene:#x}")
jit = struct.unpack("<2f", read(camera + 0x38, 8))
print("camera jitter", jit, "jitter frame", struct.unpack("<I", read(camera + 0x40, 4))[0])
print("renderer frame", q(renderer + 0x1f210), "render frame", q(renderer + 0x1f208),
      "scale", struct.unpack("<2f", read(renderer + 0x200, 8)))
print("halton", [round(v, 4) for v in struct.unpack("<40f", read(scene + 0xe6a0, 160))])
data = read(camera, 0x400)
fl = struct.unpack("<256f", data)
bad = [(i * 4, v) for i, v in enumerate(fl) if v != v or abs(v) > 1e7 and abs(v) < 1e30]
print("camera floats NaN/huge (offset, value):", [(hex(o), v) for o, v in bad[:40]])
for row in range(0, 0x400, 0x40):
    print(f"+{row:03x}", " ".join(f"{v:10.4g}" for v in fl[row // 4: row // 4 + 16]))
