"""
Client for tools/blender_bridge.py — sends Python to a running Blender and
prints what came back.

Runs on the HOST python (3.14 here), not Blender's; stdlib only, no install.

    python tools\\bsend.py -c "print(len(bpy.data.objects))"
    python tools\\bsend.py -f tools\\BuildArch.py
    echo print(bpy.app.version_string) | python tools\\bsend.py

Exit status is 0 only when the snippet ran without raising, so it composes with
`&&` and is honest to a script that checks.

--no-log suppresses the bridge's transcript entry: use it for INSPECTION
(measuring, listing, screenshotting) so the log stays a buildable recipe rather
than a mix of construction and questions.
"""

import argparse
import json
import socket
import sys

HOST = "127.0.0.1"
DEFAULT_PORT = 4242


def send(code, port=DEFAULT_PORT, timeout=120.0, log=True):
    request = json.dumps({"code": code, "log": log, "timeout": timeout}) + "\n"
    with socket.create_connection((HOST, port), timeout=10) as sock:
        sock.sendall(request.encode("utf-8"))
        # Give the read a little more rope than the bridge's own timeout, so a
        # bridge-side timeout comes back as its proper error message rather
        # than as a socket timeout here.
        sock.settimeout(timeout + 15)
        chunks = []
        while not (chunks and chunks[-1].endswith(b"\n")):
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks).decode("utf-8").strip()
    if not raw:
        raise ConnectionError("bridge closed the connection without replying")
    return json.loads(raw)


def main():
    ap = argparse.ArgumentParser(description="Send Python to a running Blender.")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("-c", "--code", help="code to run")
    src.add_argument("-f", "--file", help="file of code to run")
    ap.add_argument("-p", "--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("-t", "--timeout", type=float, default=120.0)
    ap.add_argument("--no-log", action="store_true",
                    help="don't record this in the bridge transcript")
    args = ap.parse_args()

    if args.code is not None:
        code = args.code
    elif args.file:
        with open(args.file, "r", encoding="utf-8") as f:
            code = f.read()
    else:
        code = sys.stdin.read()

    if not code.strip():
        ap.error("nothing to run")

    try:
        reply = send(code, args.port, args.timeout, log=not args.no_log)
    except (ConnectionError, OSError) as exc:
        print("could not reach the bridge on {}:{} — {}".format(HOST, args.port, exc),
              file=sys.stderr)
        print("is Blender running with tools\\blender-bridge.cmd?", file=sys.stderr)
        return 2

    if reply.get("out"):
        sys.stdout.write(reply["out"])
    if reply.get("err"):
        sys.stderr.write(reply["err"])
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
