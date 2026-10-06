# ============================================================================
# tools/harness_game.py - the Python half of tools/HarnessGame.ps1.
#
# EditorTest, WorldTest, SpellTest and LevelBuildTest each ran the game with
# their own subprocess.run(), and the copies drifted (code-review C401, C430):
# two dropped the exit code and let a TimeoutExpired escape as a traceback, one
# captured the code and never read it, and none refused a second run from the
# same worktree - whose game writes the SAME dungeon.log, truncated on open, so
# two runs interleave each other's verdict source.
#
#   import harness_game
#   harness_game.refuse_if_running(EXE)        # once, at the start
#   code, log = harness_game.run_eval(EXE, ROOT, LOG, [script], ["-project", p])
#   if not harness_game.finished(code, log): ...   # crashed / killed / hung
#
# A run COUNTS only if it finished: exit 0, or a verdict line in its log
# (`eval RESULT=` / `eval BATCH RESULT=` - an eval that FAILS exits non-zero
# but still says so). A run killed before its verdict line is a failed run,
# never a log to be parsed as if it were whole.
# ============================================================================
import ctypes
import io
import os
import subprocess
import sys
from ctypes import wintypes

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def running_copies(exe):
    """PIDs of processes running exactly this exe (this worktree's build)."""
    want = os.path.normcase(os.path.abspath(exe))
    psapi = ctypes.WinDLL("psapi")
    k32 = ctypes.WinDLL("kernel32")
    k32.OpenProcess.restype = wintypes.HANDLE
    pids = (wintypes.DWORD * 4096)()
    got = wintypes.DWORD()
    if not psapi.EnumProcesses(ctypes.byref(pids), ctypes.sizeof(pids), ctypes.byref(got)):
        return []
    found = []
    for pid in pids[: got.value // ctypes.sizeof(wintypes.DWORD)]:
        if not pid:
            continue
        h = k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(len(buf))
            if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                if os.path.normcase(buf.value) == want:
                    found.append(int(pid))
        finally:
            k32.CloseHandle(h)
    return found


def refuse_if_running(exe):
    """Exit (code 3) if this worktree's game is already running: it shares the
    log this run would read. Another worktree's game is a different exe with its
    own log, and is fine."""
    pids = running_copies(exe)
    if pids:
        print(f"refused: {exe} is already running (pid {', '.join(map(str, pids))}) - "
              "it writes the same dungeon.log; close it or wait for it")
        sys.exit(3)


def read_log(log):
    if not os.path.isfile(log):
        return ""
    return io.open(log, encoding="utf-8", errors="replace").read()


def run_eval(exe, cwd, log, scripts, extra=(), timeout=600):
    """Runs the game headless on eval scripts. Returns (exit code, log text);
    the code is -1 when the run timed out (subprocess.run kills it)."""
    refuse_if_running(exe)
    args = [exe, "-headless", *extra, "-eval", *scripts]
    try:
        code = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        # A debug assert parks the game on a CRT dialog until the timeout. It is
        # reported as a failed run with the log's FATAL line, not a traceback.
        code = -1
    return code, read_log(log)


def finished(code, log_text):
    """Whether a run got as far as its verdict: exit 0, or a verdict line."""
    return code == 0 or "eval RESULT=" in log_text or "eval BATCH RESULT=" in log_text


def fatal_lines(log_text):
    return [l.split("FATAL", 1)[1][:160] for l in log_text.splitlines() if "FATAL" in l]


def report_unfinished(code, log_text, what):
    """Prints why a run that did not finish did not count. Returns True if it
    did not finish (the caller counts a failure)."""
    if finished(code, log_text):
        return False
    why = "timed out" if code == -1 else f"exited {code}"
    print(f"  [FAIL] {what}: the game {why} before writing its verdict - the run does not count")
    for f in fatal_lines(log_text):
        print(f"         game FATAL: {f}")
    return True
