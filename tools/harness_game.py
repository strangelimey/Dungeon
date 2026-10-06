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
#   harness_game.refuse_if_stale(EXE)          # once, at the start (exit 4)
#   harness_game.refuse_if_running(EXE)        # once, at the start (exit 3)
#   code, log = harness_game.run_eval(EXE, ROOT, LOG, [script], ["-project", p])
#   if not harness_game.finished(code, log): ...   # crashed / killed / hung
#   harness_game.report_failed_script(log, script) # finished, but came back FAIL
#
# A run COUNTS only if it finished: exit 0, or a verdict line in its log
# (`eval RESULT=` / `eval BATCH RESULT=` - an eval that FAILS exits non-zero
# but still says so). A run killed before its verdict line is a failed run,
# never a log to be parsed as if it were whole.
#
# A JUDGE NEVER WORKS ON THE REAL WORLD (code-review C431). EditorTest and
# WorldTest used to edit dungeon-demo in place and put it back with a delete
# followed by a copy: a run killed between the two left the folder empty, and a
# run killed mid-phase left it changed - whereupon the next run's first act was
# to delete its backup, the only clean copy. Now:
#
#   guard = harness_game.RealTree(ROOT, own=WORLDS)       # BEFORE clearing up
#   ... clear what a killed run left (remove_world, recover) ...
#   proj = harness_game.scratch_world(ROOT, "et_demo")    # run with -project et_demo
#   name = harness_game.save_name(ROOT, "flagtest")       # this worktree's save
#   path = harness_game.eval_script(src, out_dir, saves={"flagtest": name})
#   guard.check(check)                                    # ... phases ... nothing real moved
#
# What a killed run leaves is a scratch folder the next run replaces, never a
# changed real one. Where the real tree MUST change (EditorTest's style library,
# which has one fixed home), back_up() / restore() keep a backup that is only
# ever deleted by the restore that used it, and a folder under the backup's
# name is always a WHOLE one: it arrives by a rename and leaves by a rename.
# ============================================================================
import ctypes
import filecmp
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
from ctypes import wintypes

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# Exit codes the harnesses share (tools/HarnessGame.ps1): 0 PASS, 1 FAIL,
# 2 nothing ran (no build, or an argument the judge does not know), 3 refused -
# this worktree's game is running, 4 refused - the exe is behind its sources. A
# refusal is not a verdict.
EXIT_USAGE = 2
EXIT_RUNNING = 3
EXIT_STALE = 4


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
        sys.exit(EXIT_RUNNING)


def stale_steps(exe):
    """How many build steps stand between exe and its sources, asked of ninja
    with a dry run (-1 = cannot tell). Through a COPY of the manifest: the
    CONFIGURE_DEPENDS globs make a plain `ninja -n` stop at "Re-running CMake"
    every time (tools/HarnessGame.ps1 Get-StaleSteps says why)."""
    build = os.path.dirname(os.path.dirname(os.path.abspath(exe)))
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(cache) or not os.path.isfile(os.path.join(build, "build.ninja")):
        return -1
    ninja = None
    for line in io.open(cache, encoding="utf-8", errors="replace"):
        if line.startswith("CMAKE_MAKE_PROGRAM:FILEPATH="):
            ninja = line.split("=", 1)[1].strip()
            break
    if not ninja:
        return -1
    target = os.path.splitext(os.path.basename(exe))[0]
    copy = os.path.join(build, f"harness-dryrun-{os.getpid()}.ninja")
    shutil.copyfile(os.path.join(build, "build.ninja"), copy)
    try:
        r = subprocess.run([ninja, "-C", build, "-f", os.path.basename(copy), "-n", target],
                           capture_output=True, text=True, errors="replace")
    finally:
        try:
            os.remove(copy)
        except OSError:
            pass
    if r.returncode != 0:
        return -1
    out = r.stdout + r.stderr
    if "no work to do" in out:
        return 0
    return sum(1 for l in out.splitlines() if re.match(r"\[\d+/\d+\]", l))


def refuse_if_stale(exe):
    """Exit (code 4) when exe is behind its sources: a PASS on yesterday's
    binary reads as a PASS on today's change (code-review C426). CheckAll builds
    first, so this only stops a harness run on its own. DN_HARNESS_ALLOW_STALE=1
    runs it anyway and says so. A missing exe is left to the caller's own "no
    build" message."""
    if not os.path.isfile(exe):
        return
    steps = stale_steps(exe)
    if steps == 0:
        return
    cfg = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(exe))))
    why = (f"the build system could not say whether {exe} is current" if steps < 0
           else f"{exe} is STALE ({steps} build step(s) pending)")
    if os.environ.get("DN_HARNESS_ALLOW_STALE"):
        print(f"WARNING: {why} - running it anyway (DN_HARNESS_ALLOW_STALE)")
        return
    print(f"refused: {why} - run .\\build.cmd {cfg} first (exit {EXIT_STALE})")
    sys.exit(EXIT_STALE)


def read_log(log):
    if not os.path.isfile(log):
        return ""
    return io.open(log, encoding="utf-8", errors="replace").read()


def run_eval(exe, cwd, log, scripts, extra=(), timeout=600, headless=True):
    """Runs the game on eval scripts, headless unless told otherwise. Returns
    (exit code, log text); the code is -1 when the run timed out (subprocess.run
    kills it). headless=False is for a check whose subject is DRAWING - a
    headless run skips the render half of every frame, so a fault there cannot
    happen in it (EditorTest's sprite-arena and type-editor swatch phases).

    EVERY run is -unattended, drawn or not: nobody watches a harness, so a fatal
    error records, dumps and EXITS rather than parking the game on the CRT's
    abort box until the timeout (-headless implies it; a windowed run used to
    lose it, and a drawing fault that ended in an assert would have waited out
    the whole timeout on someone's desktop)."""
    refuse_if_running(exe)
    args = [exe, "-unattended", *(["-headless"] if headless else []), *extra,
            "-eval", *scripts]
    try:
        code = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        # A run that HANGS (an assert no longer can - see above) is reported as
        # a failed run with the log's FATAL line, not a traceback.
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


def report_failed_script(log_text, what):
    """Prints why a script that FINISHED came back FAIL: the runner's own lines
    for a refused, unknown or unrefused line, or the end state. Returns True if
    any script verdict in the log is FAIL (the caller counts a failure). A
    harness that reads only the lines it checks never sees a setup line the
    world declined - that is code-review C442's whole finding."""
    lines = log_text.splitlines()
    failed = [l for l in lines if "eval RESULT=FAIL" in l]
    if not failed:
        return False
    for v in failed:
        print(f"  [FAIL] {what}: {v.split('eval ', 1)[1][:160]}")
    for l in lines:
        if any(k in l for k in ("REFUSED:", "matched no command", "expected to REFUSE",
                                 "refused by the console's gate", "ended in state")):
            print(f"         {l.split('eval: ', 1)[-1][:160]}")
    return True


# ---------------------------------------------------------------------------
# Scratch worlds: a judge's -project, never the real one
# ---------------------------------------------------------------------------

def projects_dir(root):
    return os.path.join(root, "assets", "projects")


def _tracked(root, path):
    """Whether git tracks anything under path (a real world, not a scratch one).
    FAILS CLOSED: a git that cannot answer (not a repository, a safe.directory
    refusal when run as another user, no git at all) raises. Read as "nothing
    tracked", that error would let remove_world delete dungeon-demo."""
    rel = os.path.relpath(path, root)
    r = subprocess.run(["git", "-C", root, "ls-files", "--", rel],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"git ls-files failed in {root} (exit {r.returncode}: "
                           f"{r.stderr.strip()[:200]}) - cannot tell whether {path} is a real "
                           "world, so it is not deleted")
    return r.stdout.strip() != ""


def _own_folders(name):
    """The folders scratch world `name` can leave: itself, and the hidden
    `.building-<name>` a create killed half-way leaves (Game::CreateWorld builds
    a world there and renames it into place)."""
    return (name, ".building-" + name)


def remove_world(root, name):
    """Deletes scratch world `name` (assets/projects/<name>) and its half-built
    `.building-<name>`, whichever are there. REFUSES a folder git tracks: a typo
    here must not delete dungeon-demo."""
    for folder in _own_folders(name):
        path = os.path.join(projects_dir(root), folder)
        if not os.path.isdir(path):
            continue
        if _tracked(root, path):
            raise RuntimeError(f"refusing to delete {path}: git tracks it, so it is not a scratch world")
        shutil.rmtree(path, ignore_errors=True)


def scratch_world(root, name, source="dungeon-demo"):
    """A fresh copy of world `source` as assets/projects/<name>, for a run with
    `-project <name>`. Whatever a killed run left under that name is replaced."""
    remove_world(root, name)
    path = os.path.join(projects_dir(root), name)
    shutil.copytree(os.path.join(projects_dir(root), source), path)
    return path


# ---------------------------------------------------------------------------
# Saves named per worktree
# ---------------------------------------------------------------------------

def save_dir():
    """Where the game saves: the Documents KNOWN FOLDER's DungeonSaves (Core/
    Paths.cpp SaveDir), asked the way the game asks. Hardcoding %USERPROFILE%\\
    OneDrive\\Documents is right only where OneDrive redirected Documents.
    Raises rather than guessing, since a wrong guess reads as "no save"."""
    import uuid

    class GUID(ctypes.Structure):
        _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                    ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]

    folder = GUID.from_buffer_copy(
        uuid.UUID("{FDD39AD0-238F-46AF-ADB4-6C85480369C7}").bytes_le)  # FOLDERID_Documents
    path = ctypes.c_wchar_p()
    hr = ctypes.windll.shell32.SHGetKnownFolderPath(
        ctypes.byref(folder), 0, None, ctypes.byref(path))
    try:
        if hr != 0 or not path.value:
            raise OSError("SHGetKnownFolderPath(Documents) failed: 0x%08X" % (hr & 0xFFFFFFFF))
        return os.path.join(path.value, "DungeonSaves")
    finally:
        ctypes.windll.ole32.CoTaskMemFree(path)


def worktree_tag(root):
    """A short name for this checkout: its folder name plus a hash of its path
    (two checkouts may share a folder name under different parents)."""
    base = re.sub(r"[^a-z0-9]+", "_", os.path.basename(os.path.normpath(root)).lower()).strip("_")
    digest = hashlib.sha1(os.path.normcase(os.path.abspath(root)).encode("utf-8")).hexdigest()[:6]
    return f"{base}_{digest}"


def save_name(root, base):
    """`base` as THIS worktree's save. Documents\\DungeonSaves is one folder that
    every worktree, every session and Michael's own play share, so a fixed name
    is overwritten by - and deleted by - somebody else's run. Spelled the way
    SaveSlotPath slugs a name (lowercase letters, digits, '_'), so the file is
    exactly <name>.dsav."""
    return f"{base}_{worktree_tag(root)}"


def save_path(name):
    return os.path.join(save_dir(), name + ".dsav")


def remove_saves(names):
    """Deletes this run's saves (and a .bak beside one). Only ever pass names
    from save_name(): they are this worktree's, so no one else's play is lost."""
    for n in names:
        for p in (save_path(n), save_path(n) + ".bak"):
            try:
                os.remove(p)
            except OSError:
                pass


# A `save` / `load` line, bare or as an `expect-refuse` probe. The probe form was
# missed when the probes arrived (code-review C442): WorldTest phase 3's
# `expect-refuse load worldtrip` kept the shared name, loaded a slot that did not
# exist and was refused for THAT - its checks were met by other lines.
_COMMAND_SAVE = re.compile(r"^(\s*(?:expect-refuse\s+)?)(save|load)(\s+)(\S+)(\s*)$")


def eval_script(src, out_dir, saves=None, words=None):
    """A copy of eval script `src` for this run, written to out_dir under the
    SAME file name (the verdict line names a script by its file name):
    - `saves`: {name: new} renames the slot of a `save` / `load` line (an
      `expect-refuse` one too);
    - `words`: {word: new} replaces that whole word (a world's id, say) in
      every command line.
    Comment lines are copied as they are. Returns src itself when nothing
    changed. A script with `include` is refused: an include resolves against
    the script's own folder, which a copy would move."""
    saves, words = saves or {}, words or {}
    text = io.open(src, encoding="utf-8", newline="").read()
    out = []
    for line in text.splitlines(True):
        body = line.rstrip("\r\n")
        eol = line[len(body):]
        if body.lstrip().startswith(";") or not body.strip():
            out.append(line)
            continue
        if re.match(r"\s*include\b", body):
            raise RuntimeError(f"{src}: a script with `include` cannot be copied for a run")
        m = _COMMAND_SAVE.match(body)
        if m and m.group(4) in saves:
            body = m.group(1) + m.group(2) + m.group(3) + saves[m.group(4)] + m.group(5)
        for old, new in words.items():
            body = re.sub(r"(?<![\w-])" + re.escape(old) + r"(?![\w-])", new, body)
        out.append(body + eol)
    copied = "".join(out)
    if copied == text:
        return src
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, os.path.basename(src))
    io.open(dst, "w", encoding="utf-8", newline="").write(copied)
    return dst


# ---------------------------------------------------------------------------
# The real tree: changed only behind a backup, and checked untouched
# ---------------------------------------------------------------------------

def _files(path):
    out = {}
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            out[os.path.relpath(p, path)] = p
    return out


def tree_digest(path):
    """A fingerprint of every file under path, names and bytes."""
    h = hashlib.sha1()
    for rel, p in _files(path).items():
        h.update(rel.replace("\\", "/").encode("utf-8") + b"\0")
        with open(p, "rb") as f:
            h.update(f.read())
        h.update(b"\0")
    return h.hexdigest()


# A folder under a backup's own name is ALWAYS a whole copy, because it only
# ever arrives and leaves by an (atomic) rename: back_up() copies under
# <backup>.partial and renames it in, restore() renames it out to
# <backup>.discard and only then deletes it. A half-made or half-deleted backup
# therefore never sits under the name that means "the clean copy" - which is
# what made a plain rmtree(backup) a hazard: rmtree removes a file at a time,
# a run killed during it left a partial folder under that name, and the next
# run restored from it, deleting every real file the partial copy had lost.
_PARTIAL = ".partial"
_DISCARD = ".discard"


def _clear_leftovers(backup):
    """A half-made (.partial) or half-deleted (.discard) backup is never read,
    only removed."""
    for leftover in (backup + _PARTIAL, backup + _DISCARD):
        shutil.rmtree(leftover, ignore_errors=True)


def restore(backup, dst):
    """Puts dst back to EXACTLY what backup holds, overwriting in place (a file
    that differs is rewritten, one the backup lacks is removed), and only then
    discards the backup - by renaming it out of its name before deleting it.
    Killed part-way through the writing, the backup is still there, whole, and
    the next run's recover() / back_up() finishes the job - unlike a
    delete-then-copy, which a kill in the middle leaves with nothing at all."""
    want, have = _files(backup), _files(dst)
    for rel, p in want.items():
        target = os.path.join(dst, rel)
        if rel in have and filecmp.cmp(have[rel], p, shallow=False):
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(p, target)
    for rel, p in have.items():
        if rel not in want:
            os.remove(p)
    for dirpath, dirnames, filenames in os.walk(dst, topdown=False):
        if dirpath != dst and not os.listdir(dirpath):
            os.rmdir(dirpath)
    gone = backup + _DISCARD
    shutil.rmtree(gone, ignore_errors=True)
    os.rename(backup, gone)
    shutil.rmtree(gone)


def back_up(src, backup):
    """Copies src to backup before a phase changes the real src. A backup that
    is ALREADY there is a killed run's and the only clean copy, so src is
    restored from it first - never deleted to make room for a copy of the
    damage. The copy is made under a temporary name and renamed into place, so
    a half-made backup is never mistaken for a whole one."""
    if os.path.isdir(backup):
        print(f"  a killed run's backup is at {backup} - putting {src} back from it")
        restore(backup, src)
    _clear_leftovers(backup)
    shutil.copytree(src, backup + _PARTIAL)
    os.rename(backup + _PARTIAL, backup)


def recover(src, backup):
    """At a run's start: if a killed run left `backup`, put src back from it.
    Its leftovers (.partial, .discard) are removed and never restored from."""
    if os.path.isdir(backup):
        print(f"a killed run's backup is at {backup} - putting {src} back from it")
        restore(backup, src)
    _clear_leftovers(backup)


def _key(path):
    return os.path.normcase(os.path.abspath(path))


class RealTree:
    """What a judge must leave alone. Taken at its start BEFORE it clears up
    after a killed run, so the clean-up is judged too rather than adopted as
    the starting point (taken after it, a recovery that DAMAGED the library
    became the baseline and the run still passed). check() at the end makes it
    a CHECK rather than a promise:

    - every world under assets/projects and the style library ends byte for
      byte as the run found it - except a tree in `backups` ({tree: backup})
      whose backup a killed run left standing: that tree must end as the
      BACKUP holds it (the clean copy), not as the killed run left it;
    - git status under assets/projects and assets/library shows nothing that
      was not there when the run found it;
    - and NO status line names one of the judge's `own` scratch worlds (or the
      .building- folder a killed create leaves), whatever the start had: what
      a killed run left of a judge's own is that judge's to clear."""

    PATHS = ("assets/projects", "assets/library")

    def __init__(self, root, own=(), backups=None):
        self.root = root
        folders = {f for n in own for f in _own_folders(n)}
        self.own = sorted(f"assets/projects/{f}/" for f in folders)
        projects = projects_dir(root)
        self.trees = [os.path.join(projects, d) for d in sorted(os.listdir(projects))
                      if os.path.isdir(os.path.join(projects, d)) and d not in folders]
        self.trees.append(os.path.join(root, "assets", "library"))
        standing = {_key(t): b for t, b in (backups or {}).items() if os.path.isdir(b)}
        self.digests = {t: tree_digest(standing.get(_key(t), t)) for t in self.trees}
        self.status = self._status()

    def _status(self):
        r = subprocess.run(["git", "-C", self.root, "status", "--porcelain", "--untracked-files=all",
                            "--", *self.PATHS],
                           capture_output=True, text=True, errors="replace")
        return r.stdout.splitlines() if r.returncode == 0 else None

    def _names_own(self, line):
        path = line[3:].strip().strip('"').replace("\\", "/")
        return any(path.startswith(p) for p in self.own)

    def check(self, check):
        moved = [os.path.relpath(t, self.root) for t in self.trees
                 if not os.path.isdir(t) or tree_digest(t) != self.digests[t]]
        check(not moved, "the real worlds and the style library are byte for byte as the run "
              "found them (or as a killed run's backup held them)", f"changed: {moved}")
        now = self._status()
        if now is None or self.status is None:
            check(False, "git status under assets/projects and assets/library", "git status failed")
            return
        new = sorted(set(now) - set(self.status))
        check(not new, "and git status under assets/projects and assets/library shows nothing "
              "new since the run found it", "; ".join(new))
        left = [l for l in now if self._names_own(l)]
        check(not left, "and names none of this judge's own scratch worlds (a killed run's "
              "included)", "; ".join(left[:8]) + (f" ... ({len(left)} lines)" if len(left) > 8 else ""))
