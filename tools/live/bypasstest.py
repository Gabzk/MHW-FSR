"""Bypass on/off: the plugin must give the game's anti-aliasing setting back and take it again.
Also prints the game's dedicated GPU memory before and after a run of Mode switches (leak check).
usage: bypasstest.py [switches]
"""
import os
import subprocess
import sys
import time
import types

# The helpers of modetest.py (everything before its main part).
m = types.SimpleNamespace()
_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "modetest.py"), encoding="utf-8").read()
_ns = {}
exec(_src.split("start = time.strftime")[0].replace("cycles = int(sys.argv[1])", ""), _ns)
m.__dict__.update(_ns)


def _set_key(key, value):
    lines = open(m.INI, "rb").read().decode("ascii").split("\r\n")
    lines = [f"{key}={value}" if l.startswith(key + "=") else l for l in lines]
    open(m.INI, "wb").write("\r\n".join(lines).encode("ascii"))


m.set_key = _set_key
switches = int(sys.argv[1]) if len(sys.argv) > 1 else 0


def gpu_mb():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"((Get-Counter '\\GPU Process Memory(pid_{m.pid()}*)\\Dedicated Usage').CounterSamples | "
                          "Measure-Object CookedValue -Sum).Sum / 1MB"], capture_output=True, text=True).stdout.strip()
    return float(out.replace(",", ".")) if out else -1


for value in ("1", "0"):
    n = len(m.log_lines())
    m.set_key("Bypass", value)
    time.sleep(3)
    print(f"Bypass={value}: {m.aa_state()}")
    for l in m.log_lines()[n:]:
        if "anti-aliasing" in l or "ini reloaded" in l:
            print("   ", l[:160])

if switches:
    m.set_key("Mode", "DLAA")
    time.sleep(4)
    before = gpu_mb()
    for i in range(switches):
        m.set_key("Mode", "Performance" if i % 2 == 0 else "DLAA")
        time.sleep(2.5)
    m.set_key("Mode", "DLAA")
    time.sleep(4)
    print(f"dedicated GPU memory: {before:.0f} MB before, {gpu_mb():.0f} MB after {switches} switches")
