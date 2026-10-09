"""Reads MT Framework texture objects and natives in the running game.
usage: natpeek.py tex:<addr> ... nat:<addr> ..."""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys

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


def native(addr):
    d = read(addr, 0x30)
    if d is None:
        return f"{addr:#x}: unreadable"
    vt, rc = struct.unpack_from("<QI", d, 0)
    return f"{addr:#x}: vtable {vt:#x} refcount {rc} (+0x10 {struct.unpack_from('<Q', d, 0x10)[0]:#x} +0x20 {struct.unpack_from('<Q', d, 0x20)[0]:#x})"


for arg in sys.argv[1:]:
    kind, a = arg.split(":")
    a = int(a, 16)
    if kind == "tex":
        d = read(a, 0x100)
        if d is None:
            print(f"texture {a:#x}: unreadable")
            continue
        n60 = struct.unpack_from("<Q", d, 0x60)[0]
        nf0 = struct.unpack_from("<Q", d, 0xf0)[0]
        print(f"texture {a:#x}: vtable {struct.unpack_from('<Q', d, 0)[0]:#x} +0x60 {n60:#x} +0xf0 {nf0:#x}")
        for n in {n60, nf0} - {0}:
            print("   ", native(n))
    else:
        print("native", native(a))
