# ============================================================================
# tools/harness_audio.py - every harness runs the game MUTED.
#
# The Python half of tools/HarnessAudio.ps1 (read that header for the why): a
# harness mutes the `volume=` line of settings.ini beside the exe for the run,
# and puts back exactly that line afterwards, however the run ends.
#
#   with harness_audio.muted(BIN):
#       ...
#
# or, where the run is already one long try/finally, mute() before it and
# restore() in its finally. DN_HARNESS_MUTED makes it nest with the PowerShell
# side: whoever set it first owns the restore.
# ============================================================================
import contextlib
import io
import os
import re

_FLAG = "DN_HARNESS_MUTED"


def _read(path):
    if not os.path.isfile(path):
        return None
    return io.open(path, encoding="utf-8", newline="").read()


def _with_volume(text, line):
    """text with its volume line replaced in place (or appended); line=None removes it."""
    out = []
    placed = line is None
    for l in (l for l in re.split(r"\r?\n", text or "") if l):
        if not l.startswith("volume="):
            out.append(l)
        elif not placed:
            out.append(line)
            placed = True
    if not placed:
        out.append(line)
    return "\n".join(out) + "\n" if out else ""


def mute(bin_dir):
    """Mute the game; returns the state restore() needs (None = not ours)."""
    if os.environ.get(_FLAG):
        return None
    ini = os.path.join(bin_dir, "settings.ini")
    before = _read(ini)
    original = next((l for l in re.split(r"\r?\n", before or "")
                     if l.startswith("volume=")), None)
    shown = original[len("volume="):] if original else "default"
    print(f"audio: master volume muted for the run (was {shown})")
    if os.path.isdir(bin_dir):
        io.open(ini, "w", encoding="utf-8", newline="").write(
            _with_volume(before, "volume=0"))
    os.environ[_FLAG] = "1"
    return {"ini": ini, "existed": before is not None, "line": original, "shown": shown}


def restore(state):
    """Put back the volume line mute() found - and only that line."""
    if state is None:
        return
    os.environ.pop(_FLAG, None)
    now = _read(state["ini"])
    if now is not None:
        restored = _with_volume(now, state["line"])
        # A file the run created only to hold the mute goes again.
        if not state["existed"] and not restored:
            os.remove(state["ini"])
        else:
            io.open(state["ini"], "w", encoding="utf-8", newline="").write(restored)
    print(f"audio: master volume restored ({state['shown']})")


@contextlib.contextmanager
def muted(bin_dir):
    state = mute(bin_dir)
    try:
        yield
    finally:
        restore(state)
