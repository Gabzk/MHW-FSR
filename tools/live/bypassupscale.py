"""Bypass in an upscaling mode must not release the anti-aliasing hold: DLSS still upscales at the
DLISP position and needs the TAA pass. Sets Mode=Performance and Bypass=1, checks that the switches
stay at TAA only and that DLISP keeps evaluating, then puts Bypass=0 and Mode=DLAA back.
usage: bypassupscale.py
"""
import os
import re
import sys
import time
import types

m = types.SimpleNamespace()
_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "modetest.py"), encoding="utf-8").read()
_ns = {}
exec(_src.split("start = time.strftime")[0].replace("cycles = int(sys.argv[1])", ""), _ns)
m.__dict__.update(_ns)

ok = True
m.set_key("LogLevel", "Debug")  # the evaluate progress lines are debug lines
m.set_key("Mode", "Performance")
time.sleep(4)
n = len(m.log_lines())
m.set_key("Bypass", "1")
time.sleep(4)
new = m.log_lines()[n:]
state = m.aa_state()
dlisp = [l for l in new if re.search(r"DLISP evaluate #\d+", l)]
released = [l for l in new if "back to the game's" in l]
print(f"Performance + Bypass=1: {state}; DLISP evaluate lines {len(dlisp)}; hold released {bool(released)}")
if "scene TAA 1 FXAA 0" not in state or not dlisp or released:
    print("FAIL")
    ok = False
m.set_key("Bypass", "0")
time.sleep(3)
m.set_key("Mode", "DLAA")
time.sleep(3)
m.set_key("LogLevel", "Info")
print("back to DLAA:", m.aa_state())
sys.exit(0 if ok else 1)
