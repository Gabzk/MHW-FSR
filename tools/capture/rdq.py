"""Client for rdserver.py (RenderDoc capture kept open in qrenderdoc's embedded Python).

  rdq.py --start CAPTURE.rdc     start qrenderdoc with the server and wait until the capture is loaded
  rdq.py SCRIPT.py               run a script against the open capture, print its output
  rdq.py -c "CODE"               same with inline code
  rdq.py --stop                  shut the server down

The command directory is %RDSERVER_DIR% or %TEMP%\\rdserver. Run with plain python (stdlib only).
"""
import argparse
import os
import subprocess
import sys
import time

QRENDERDOC = os.environ.get("QRENDERDOC", r"C:\Program Files\RenderDoc\qrenderdoc.exe")
CMD_DIR = os.environ.get("RDSERVER_DIR", os.path.join(os.environ.get("TEMP", "."), "rdserver"))


def start(capture, timeout):
    os.makedirs(CMD_DIR, exist_ok=True)
    for name in os.listdir(CMD_DIR):
        if name in ("ready", "quit") or name.endswith((".cmd.py", ".out", ".out.tmp")):
            os.remove(os.path.join(CMD_DIR, name))
    env = dict(os.environ, RDSERVER_DIR=CMD_DIR, RDSERVER_CAPTURE=os.path.abspath(capture))
    server = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rdserver.py")
    subprocess.Popen([QRENDERDOC, "--python", server], env=env, close_fds=True,
                     creationflags=subprocess.DETACHED_PROCESS)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(os.path.join(CMD_DIR, "ready")):
            print(open(os.path.join(CMD_DIR, "server.log")).read().strip().splitlines()[-1])
            return 0
        time.sleep(0.5)
    print("timed out; see", os.path.join(CMD_DIR, "server.log"))
    return 1


def run(code, timeout):
    if not os.path.exists(os.path.join(CMD_DIR, "ready")):
        print("server not running (rdq.py --start CAPTURE)")
        return 1
    name = "%d" % (time.time() * 1000)
    tmp = os.path.join(CMD_DIR, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(code)
    os.replace(tmp, os.path.join(CMD_DIR, name + ".cmd.py"))
    result = os.path.join(CMD_DIR, name + ".out")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(result):
            with open(result, encoding="utf-8") as f:
                sys.stdout.write(f.read())
            os.remove(result)
            return 0
        time.sleep(0.1)
    print("no answer within %d s (the command may still be running)" % timeout)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", nargs="?")
    ap.add_argument("-c", dest="code")
    ap.add_argument("--start", metavar="CAPTURE")
    ap.add_argument("--stop", action="store_true")
    ap.add_argument("--timeout", type=float, default=600)
    a = ap.parse_args()
    if a.start:
        return start(a.start, a.timeout)
    if a.stop:
        open(os.path.join(CMD_DIR, "quit"), "w").close()
        return 0
    if a.code is not None:
        return run(a.code, a.timeout)
    if a.script:
        with open(a.script, encoding="utf-8") as f:
            return run(f.read(), a.timeout)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
