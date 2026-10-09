"""Bypass on/off: the plugin must give the game's anti-aliasing setting back and take it again.
Also prints the game's dedicated GPU memory before and after a run of Mode switches (leak check).
usage: bypasstest.py [switches]
Exits with 1 when a reload is missing or the anti-aliasing switches do not follow.
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
switches = int(sys.argv[1]) if len(sys.argv) > 1 else 0


def gpu_mb():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"((Get-Counter '\\GPU Process Memory(pid_{m.pid()}*)\\Dedicated Usage').CounterSamples | "
                          "Measure-Object CookedValue -Sum).Sum / 1MB"], capture_output=True, text=True).stdout.strip()
    return float(out.replace(",", ".")) if out else -1


def set_and_check(key, value):
    n = len(m.log_lines())
    m.set_key(key, value)
    time.sleep(3)
    new = m.log_lines()[n:]
    if not any("ini reloaded" in l for l in new):
        sys.exit(f"{key}={value}: the plugin did not reload the ini")
    return new


failed = False
for value in ("1", "0"):
    new = set_and_check("Bypass", value)
    state = m.aa_state()
    print(f"Bypass={value}: {state}")
    for l in new:
        if "anti-aliasing" in l or "ini reloaded" in l:
            print("   ", l[:160])
    held = "scene TAA 1 FXAA 0" in state
    if value == "0" and not held:
        print("    FAIL: not held at TAA only after Bypass=0")
        failed = True

if switches:
    set_and_check("Mode", "DLAA")
    before = gpu_mb()
    for i in range(switches):
        set_and_check("Mode", "Performance" if i % 2 == 0 else "DLAA")
    set_and_check("Mode", "DLAA")
    time.sleep(1)
    print(f"dedicated GPU memory: {before:.0f} MB before, {gpu_mb():.0f} MB after {switches} switches")
sys.exit(1 if failed else 0)
