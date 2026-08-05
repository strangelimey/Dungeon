"""
Blender bridge — a localhost socket that executes Python inside a RUNNING,
interactive Blender, on Blender's main thread.

WHY THIS EXISTS. Michael does not drive Blender's UI; Claude does, live, while
he watches the viewport and says "wider" / "more weathered". That needs a way to
push Python into an already-open Blender session and read the result back, which
`blender --background --python` cannot do — it exits when the script ends.

WHY A MAIN-THREAD PUMP. `bpy` is NOT thread-safe. Touching the scene from the
socket thread corrupts Blender's state and crashes it in ways that look random
and land nowhere near the cause. So the socket thread ONLY enqueues, and a
`bpy.app.timers` callback — which Blender runs on the main thread — is the one
and only place anything executes. This is the whole design; don't shortcut it.

WHY EVERY SNIPPET IS LOGGED. The project's rule is that the SCRIPT is the asset:
diffable, re-runnable, with the .glb as a build artifact (see docs/authoring-
scale.md and the tools/Build*.py family). Interactive modelling would normally
destroy that. It doesn't here, because the session is driven BY PYTHON — so the
log of what was executed distils straight into a committed tools/Build*.py.
Drive with parameterised operations, not hand-nudged vertices, and the log stays
worth keeping.

USAGE
    tools\\blender-bridge.cmd            (finds the newest Blender for you)
    blender --python tools/blender_bridge.py

PROTOCOL — newline-delimited JSON, one request per line. json.dumps never emits
a bare newline, so a line IS a message.
    ->  {"code": "...", "log": true, "timeout": 120}
    <-  {"ok": true, "out": "...", "err": ""}

SECURITY. This binds 127.0.0.1 ONLY and executes whatever it is sent. That is
the point of it, and also why it must never be exposed off the loopback
interface. It is a local developer tool, not a service.
"""

import json
import os
import queue
import socketserver
import threading
import traceback
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from io import StringIO
from pathlib import Path

import bpy

HOST = "127.0.0.1"
PORT = int(os.environ.get("DUNGEON_BRIDGE_PORT", "4242"))

# The running transcript. Gitignored — it is working material, not the asset;
# the committed tools/Build*.py distilled out of it is.
LOG_PATH = Path(__file__).resolve().parent / ".bridge-log.py"

# How often the main thread drains the queue. 20 Hz is imperceptible when
# iterating by hand and costs nothing when idle.
PUMP_INTERVAL = 0.05

_requests: "queue.Queue" = queue.Queue()
_server = None

# One namespace shared by every snippet, so state accumulates across calls the
# way it would in a REPL — a mesh gets built over many small steps, and step 7
# needs the name that step 3 bound.
_ns = {"__name__": "__bridge__", "bpy": bpy}


def _append_log(code, ok):
    stamp = datetime.now().strftime("%H:%M:%S")
    try:
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write("\n# --- {}{}\n".format(stamp, "" if ok else "   [FAILED]"))
            f.write(code.rstrip() + "\n")
    except OSError as exc:  # a missing log must never kill the session
        print("[bridge] could not write log: {}".format(exc))


def _execute(code, do_log):
    """Run one snippet. Always returns a reply — never raises at the caller."""
    out, err = StringIO(), StringIO()
    ok = True
    try:
        with redirect_stdout(out), redirect_stderr(err):
            exec(compile(code, "<bridge>", "exec"), _ns)
    except BaseException:
        # BaseException, not Exception: a snippet that raises SystemExit or
        # KeyboardInterrupt should come back as a failed request, not tear
        # down Blender's main loop.
        ok = False
        err.write(traceback.format_exc())
    if do_log:
        _append_log(code, ok)
    return {"ok": ok, "out": out.getvalue(), "err": err.getvalue()}


def _pump():
    """Main-thread callback: drain everything queued since the last tick."""
    while True:
        try:
            code, do_log, reply = _requests.get_nowait()
        except queue.Empty:
            break
        reply.put(_execute(code, do_log))
    return PUMP_INTERVAL  # returning a float reschedules the timer


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        # Iterating the file object gives us one line per message and ends
        # cleanly when the client hangs up.
        for raw in self.rfile:
            line = raw.decode("utf-8").strip()
            if not line:
                continue
            try:
                req = json.loads(line)
            except json.JSONDecodeError as exc:
                self._send({"ok": False, "out": "", "err": "bad JSON: {}".format(exc)})
                continue

            timeout = float(req.get("timeout", 120))
            reply = queue.Queue()
            _requests.put((req.get("code", ""), bool(req.get("log", True)), reply))
            try:
                self._send(reply.get(timeout=timeout))
            except queue.Empty:
                # Blender's main thread is busy or wedged (a modal operator, a
                # long bake, a dialog waiting on a click). Say so rather than
                # hanging the client forever.
                self._send({
                    "ok": False,
                    "out": "",
                    "err": "timed out after {}s waiting for Blender's main thread "
                           "(modal operator or open dialog?)".format(timeout),
                })

    def _send(self, obj):
        self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.wfile.flush()

    def log_message(self, *args):  # silence socketserver's stderr chatter
        pass


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True  # so re-running the script doesn't hit TIME_WAIT


def stop():
    global _server
    if _server is not None:
        _server.shutdown()
        _server.server_close()
        _server = None
    if bpy.app.timers.is_registered(_pump):
        bpy.app.timers.unregister(_pump)
    print("[bridge] stopped")


def start():
    global _server
    if _server is not None:
        print("[bridge] already running on {}:{}".format(HOST, PORT))
        return
    try:
        _server = _Server((HOST, PORT), _Handler)
    except OSError as exc:
        print("[bridge] could NOT bind {}:{} — {}".format(HOST, PORT, exc))
        print("[bridge] another Blender may already have the bridge open; "
              "set DUNGEON_BRIDGE_PORT to use a different port.")
        return
    threading.Thread(
        target=_server.serve_forever, name="blender-bridge", daemon=True
    ).start()
    if not bpy.app.timers.is_registered(_pump):
        bpy.app.timers.register(_pump, first_interval=0.1, persistent=True)
    print("[bridge] listening on {}:{}  (log -> {})".format(HOST, PORT, LOG_PATH))


if __name__ == "__main__":
    start()
