# Developer tool: keeps a RenderDoc capture open in qrenderdoc's embedded Python and runs command
# scripts against it, so a 1 GB capture is loaded once instead of per query. Start it with
# tools/capture/rdq.py --start CAPTURE.rdc (which runs qrenderdoc.exe --python rdserver.py) and send
# commands with rdq.py. Each command runs with these names in scope, plus whatever earlier commands
# stored in the namespace:
#   rd          the renderdoc module
#   ctrl        the ReplayController of the open capture
#   sdf         its structured file (API call names and parameters)
#   actions     every action (draw, dispatch, copy, clear, marker) in event order, flattened
#   out(path)   absolute path for an output file in the command directory
import io
import os
import sys
import time
import traceback
import contextlib

import renderdoc as rd

CMD_DIR = os.environ["RDSERVER_DIR"]
CAPTURE = os.environ["RDSERVER_CAPTURE"]


def log(msg):
    with open(os.path.join(CMD_DIR, "server.log"), "a") as f:
        f.write(time.strftime("%H:%M:%S ") + msg + "\n")


def flatten(action_list, result):
    for a in action_list:
        result.append(a)
        flatten(a.children, result)
    return result


def main():
    log("opening " + CAPTURE)
    cap = rd.OpenCaptureFile()
    status = cap.OpenFile(CAPTURE, "", None)
    if status != rd.ResultCode.Succeeded:
        log("OpenFile failed: %s" % status)
        return
    if not cap.LocalReplaySupport():
        log("capture cannot be replayed locally")
        return
    status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        log("OpenCapture failed: %s" % status)
        return
    ns = {
        "rd": rd,
        "ctrl": ctrl,
        "sdf": ctrl.GetStructuredFile(),
        "actions": flatten(ctrl.GetRootActions(), []),
        "out": lambda name: os.path.join(CMD_DIR, name),
    }
    log("ready: %d actions" % len(ns["actions"]))
    with open(os.path.join(CMD_DIR, "ready"), "w") as f:
        f.write(CAPTURE)

    while True:
        names = sorted(n for n in os.listdir(CMD_DIR) if n.endswith(".cmd.py"))
        if not names:
            if os.path.exists(os.path.join(CMD_DIR, "quit")):
                break
            time.sleep(0.1)
            continue
        for name in names:
            path = os.path.join(CMD_DIR, name)
            with open(path, "r", encoding="utf-8") as f:
                code = f.read()
            os.remove(path)
            buf = io.StringIO()
            start = time.time()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
                try:
                    exec(compile(code, name, "exec"), ns)
                except Exception:
                    traceback.print_exc()
            buf.write("\n[%.1f s]\n" % (time.time() - start))
            tmp = os.path.join(CMD_DIR, name[:-7] + ".out.tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(buf.getvalue())
            os.replace(tmp, os.path.join(CMD_DIR, name[:-7] + ".out"))

    log("quit")
    os.remove(os.path.join(CMD_DIR, "quit"))
    ctrl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
for name in ("ready",):
    try:
        os.remove(os.path.join(CMD_DIR, name))
    except OSError:
        pass
os._exit(0)
