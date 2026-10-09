"""Run-time Mode switch test for the GPU fault fix (0.7.2), plus the anti-aliasing hold.

Launches the game if needed and waits for the title, then switches the plugin's Mode through
the ini. After every switch it checks that the game is alive, that the plugin keeps evaluating,
and that the NVIDIA driver logged no error (System log, nvlddmkm). Stops at the first problem.
usage: modetest.py <cycles> [--no-launch]
"""
import ctypes
import os
import re
import struct
import subprocess
import sys
import time

GAME = r"F:\SteamLibrary\steamapps\common\Monster Hunter World"
INI = GAME + r"\nativePC\plugins\MHWDLSS.ini"
LOG = GAME + r"\nativePC\plugins\MHWDLSS.log"
cycles = int(sys.argv[1])
MODES = ["Performance", "Quality", "DLAA", "Balanced", "Performance", "DLAA"]


def ps(cmd):
    return subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.strip()


def pid():
    out = ps("(Get-Process MonsterHunterWorld -ErrorAction SilentlyContinue).Id")
    return int(out) if out.isdigit() else None


def driver_errors(since):
    return int(ps("@(Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; "
                  f"StartTime=[datetime]'{since}'}} -ErrorAction SilentlyContinue).Count") or 0)


def log_lines():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def set_mode(mode):
    lines = open(INI, "rb").read().decode("ascii").split("\r\n")
    lines = [f"Mode={mode}" if l.startswith("Mode=") else l for l in lines]
    open(INI, "wb").write("\r\n".join(lines).encode("ascii"))


def aa_state():
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    h = k32.OpenProcess(0x0410, False, pid())

    def read(a, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        return buf.raw if k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) else None

    q = lambda a: struct.unpack("<Q", read(a, 8))[0]
    scene = q(0x1451c4368)
    taa, fxaa = read(scene + 0xe9e4, 2)
    units = {}
    node = q(q(0x1451238c8) + 0x15d0)
    for _ in range(400):
        if not node:
            break
        vt = q(node)
        if vt in (0x14351c1a8, 0x14351bda8):
            units["FXAA" if vt == 0x14351c1a8 else "TAA2"] = struct.unpack("<I", read(node + 0x14, 4))[0]
        node = q(node + 0x30)
    k32.CloseHandle(h)
    return f"scene TAA {taa} FXAA {fxaa}, unit flags " + ", ".join(f"{k} {v:#x}" for k, v in units.items())


start = time.strftime("%Y-%m-%d %H:%M:%S")
base_errors = driver_errors(start)
if pid() is None and "--no-launch" not in sys.argv:
    t0 = time.time()
    os.startfile("steam://rungameid/582010")
    while time.time() - t0 < 150:
        time.sleep(2)
        if os.path.exists(LOG) and os.path.getmtime(LOG) > t0 and any("evaluate #512 ok" in l for l in log_lines()):
            break
print(f"game pid {pid()}, driver errors so far {base_errors}")
print("AA:", aa_state())
for l in log_lines():
    if "anti-aliasing" in l or "MHWDLSS 0." in l:
        print("  ", l[:200])

for c in range(cycles):
    for mode in MODES:
        n = len(log_lines())
        set_mode(mode)
        time.sleep(3.0)
        new = log_lines()[n:]
        alive = pid() is not None
        errs = driver_errors(start) - base_errors
        evals = [l for l in new if re.search(r"evaluate #\d+ ok", l)]
        reload_ok = any("ini reloaded: mode " + mode in l for l in new)
        bad = [l for l in new if "[ERR]" in l or "[WRN]" in l]
        print(f"cycle {c} -> {mode}: alive {alive}, reload {reload_ok}, progress lines {len(evals)}, "
              f"driver errors {errs}, warnings {len(bad)}")
        for l in bad[:3]:
            print("    ", l[:200])
        if not alive or errs or not evals:
            print("STOP")
            for l in new[-12:]:
                print("    ", l[:200])
            sys.exit(1)
print("all switches fine")
