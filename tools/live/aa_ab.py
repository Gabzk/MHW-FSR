"""A/B of one of the game's anti-aliasing switches in the running game: captures with the switch as
is, then 0, then restored. scene+0xe9e4 is TAA, +0xe9e5 FXAA; the TAA and FXAA draws (0x142392150,
0x1423939d0) return without a pass when their byte is 0. With 0.7.2 and DLSS on, the plugin puts
the switches back to TAA only within a frame, so test with Bypass=1 or an older build.
usage: aa_ab.py <out prefix> [frames] [fxaa|taa]
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

prefix = sys.argv[1]
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 3
which = sys.argv[3] if len(sys.argv) > 3 else "fxaa"
cap = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "capture-window.ps1")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
pid = int(subprocess.check_output(
    ["powershell", "-NoProfile", "-Command", "(Get-Process MonsterHunterWorld).Id"]).decode().strip())
h = k32.OpenProcess(0x0438, False, pid)  # VM_READ | VM_WRITE | VM_OPERATION | QUERY_INFORMATION


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError(ctypes.get_last_error())
    return buf.raw


def write(addr, b):
    got = ctypes.c_size_t()
    if not k32.WriteProcessMemory(h, ctypes.c_void_p(addr), b, len(b), ctypes.byref(got)):
        raise OSError(ctypes.get_last_error())


def grab(name):
    for n in range(frames):
        subprocess.run(["pwsh", "-NoProfile", "-File", cap, "-Path", f"{prefix}_{name}_{n}.png"], check=True,
                       capture_output=True)
        time.sleep(0.2)


assert read(0x140000000, 2) == b"MZ"
scene = struct.unpack("<Q", read(0x1451c4368, 8))[0]
fx = scene + (0xe9e4 if which == "taa" else 0xe9e5)
orig = read(fx, 1)
print(f"scene {scene:#x}, {which} byte {orig[0]}")
try:
    grab("on")
    write(fx, b"\x00")
    time.sleep(0.5)
    grab("off")
finally:
    write(fx, orig)
print(f"restored {which} byte {read(fx, 1)[0]}")
time.sleep(0.5)
grab("on2")
