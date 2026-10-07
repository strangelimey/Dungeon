# tools/DisplayTest.py - the display lifecycle's checks (code-review batch 68).
#
# Run:  python tools\DisplayTest.py [--selftest] [--exclusive]   (needs a debug build)
#
# The Video tab's Apply and the GPU switch's relaunch were both a click on a page
# no harness opened. The dev console's `video` reaches them now - `video status`
# reads what is STAGED, RUNNING and SAVED, `video apply` stages and presses the
# tab's own Apply, `video restart` is the relaunch - and this judge reads what
# they print and what dungeon.log says happened:
#
#   PLACEMENT  tools\EvalScripts\display.eval in a SHOWN window (C196). An
#              untouched staging is the window as it is, and Apply on it keeps
#              that size; a native-size client is shrunk until its frame fills
#              the chosen monitor's WORK AREA; re-staging as opening Settings does
#              (`video restage`) then stages the shrunk window, not the size asked
#              for; a size that fits is centred in it; the same on the display
#              list's last monitor - only where there is more than one, so a one-monitor
#              machine SKIPS that check and the verdict line's `monitors=` says
#              how many were there; and nothing a script applies is saved -
#              settings.ini's display keys end as they began. It needs a Windowed
#              start: a settings.ini saving Borderless or Exclusive is REFUSED
#              (exit 2) before any game starts, since the shown window would take
#              that mode at boot (Exclusive switches the display).
#   RELAUNCH   tools\EvalScripts\relaunch.eval, headless, with `-project` naming
#              a scratch copy of dungeon-demo (C398). The child is this exe by its
#              own path with the parent's arguments less the script, plus
#              `-relaunched <parent pid>`; it waits for the parent to exit, so
#              the shared dungeon.log holds the parent's run WHOLE and then the
#              child's; the child opens the world the parent was given; and,
#              being headless with no script, it QUITS ON ITS OWN once its boot
#              load lands (it used to idle unseen until something killed it). A
#              child still running at the end is killed by its id, and fails.
#   WARP       tools\EvalScripts\displaywarp.eval, headless on `-warp` (code-review
#              batch 69). WARP has no outputs of its own - a hybrid laptop's
#              discrete GPU has none either - so this is where the monitors must
#              come from the DESKTOP, not the rendering adapter (C198): the
#              running adapter is listed and staged (C197), the monitor and
#              resolution lists are not empty, and Borderless through `video
#              apply` covers the staged monitor. A monitor that is not there
#              (`video ghost`) is dropped, and a real one staged, by the re-read a
#              posted WM_DISPLAYCHANGE sets off (C199); and the staged monitor's
#              device name and the GPU's identity survive settings.ini's text both
#              ways (`video ini` - the text, never the player's file).
#   DROP       in the PLACEMENT run (code-review batch 70, C194): `video drop`
#              does what Alt+Tab does to an Exclusive swapchain - its state goes
#              behind the device's back - and the next frame must find it, rebuild
#              the back buffers once (said in the log) and the frames after it
#              present. The window is Windowed there, so the drop is SIMULATED
#              (the device is made to remember a state the swapchain does not
#              hold, the same mismatch); no harness run takes a monitor.
#   EXCLUSIVE  only with --exclusive, never from CheckAll: it SWITCHES A MONITOR
#              to exclusive full-screen for a few seconds, so run it when nobody is
#              using that screen. tools\EvalScripts\displayexclusive.eval enters
#              Exclusive, drops it for real, checks the drop as above, posts the
#              activation that coming back sends (`video activate`) and checks
#              the Exclusive is re-entered, then goes back to Windowed.
#
# --selftest runs the scripts with every `video apply`, `video restart`, `video
# displaychange`, `video ini`, `video drop` and `video activate` line cut, and
# demands EXACTLY the checks
# resting on them fail (NOT_CUT names the rest, which must still pass), so no
# check is met by nothing happening.
#
# Exit: 0 PASS (or, under --selftest, exactly the expected checks failed);
# 1 FAIL; 2 nothing ran (no build); 3 refused - this worktree's game is running;
# 4 refused - the exe is behind its sources.
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import harness_audio
import harness_game

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
INI = os.path.join(ROOT, r"build\debug\bin\settings.ini")
PLACEMENT = os.path.join(ROOT, r"tools\EvalScripts\display.eval")
RELAUNCH = os.path.join(ROOT, r"tools\EvalScripts\relaunch.eval")
WARP = os.path.join(ROOT, r"tools\EvalScripts\displaywarp.eval")
WORLD = "dt_demo"
TOOL = "displaytest"
CHILD_WAIT_S = 90
DISPLAY_KEYS = ("adapter_id", "monitor", "reswidth", "resheight", "fullscreen")
GHOST = r"\\.\DN-GHOST"
EXCLUSIVE = os.path.join(ROOT, r"tools\EvalScripts\displayexclusive.eval")
CUT = ("video apply", "video restart", "video displaychange", "video ini", "video drop",
	   "video activate")

# The checks that rest on none of the CUT lines: with those lines cut
# (--selftest) these, and ONLY these, may still pass.
NOT_CUT = {
	"an untouched staging is the window as it is: Windowed, its own client size",
	"a script's apply is never saved: settings.ini's display keys are as they began",
	"and every status read the same saved choice",
	"the placement script ran to its verdict, its refusals refused",
	"a WARP boot lists and stages the running adapter, and its monitor and resolution "
	"lists are not empty",
	"the WARP script ran to its verdict",
	"the exclusive script ran to its verdict",
	"the real worlds and the style library are byte for byte as the run found them "
	"(or as a killed run's backup held them)",
	"and git status under assets/projects and assets/library shows nothing new since "
	"the run found it",
	"and names none of this judge's own scratch worlds (a killed run's included)",
}

results = []
monitors = 0  # the staged adapter's monitors, from the placement run's first status


def check(ok, label, detail=""):
	results.append((label, bool(ok)))
	print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
	if not ok and detail:
		print(f"         {detail}")
	return ok


# ---------------------------------------------------------------------------
# Reading the readouts
# ---------------------------------------------------------------------------

def rect(text):
	"""'x,y,w,h' -> (x, y, w, h); None for '-' or anything else."""
	m = re.fullmatch(r"(-?\d+),(-?\d+),(\d+),(\d+)", text or "")
	return tuple(int(g) for g in m.groups()) if m else None


def size(text):
	m = re.fullmatch(r"(\d+)x(\d+)", text or "")
	return (int(m.group(1)), int(m.group(2))) if m else None


def fields(line):
	"""The key=value words of a `video ...:` line, up to its free-text name
	(adaptername= runs to the end of the line)."""
	head = line.split(" adaptername=", 1)[0]
	return dict(re.findall(r"(\w+)=(\S+)", head))


def statuses(log_text):
	"""Each `video status` the script ran, in order, with the command it came
	after: [(previous command, {"staged": {...}, "running": {...}, "saved": line,
	"displays": {...}, "monitors": [index...], "mon": {index: {...}}})] - a
	monitor line's fields end at its free-text name=."""
	out, last, cur = [], None, None
	for l in log_text.splitlines():
		if "console: " not in l:
			continue
		said = l.split("console: ", 1)[1]
		if said.startswith("> "):
			cmd = said[2:].strip()
			if cmd == "video status":
				cur = {"after": last}
				out.append(cur)
			else:
				cur = None
				last = cmd
			continue
		if cur is None:
			continue
		for kind in ("staged", "running"):
			if said.startswith(f"video {kind}: "):
				cur[kind] = fields(said)
				cur[kind + "_line"] = said
		if said.startswith("video saved: "):
			cur["saved"] = said
		if said.startswith("video displays: "):
			cur["displays"] = fields(said)
		if said.startswith("video swapchain: "):
			cur["swapchain"] = fields(said)
		m = re.match(r"video monitor (\d+): ", said)
		if m:
			cur.setdefault("monitors", []).append(int(m.group(1)))
			cur.setdefault("mon", {})[int(m.group(1))] = dict(
				re.findall(r"(\w+)=(\S+)", said.split(" name=", 1)[0]))
	return out


def after(sts, cmd):
	"""The status read right after `cmd` (its first run), or {}."""
	return next((s for s in sts if s.get("after") == cmd), {})


def inside(win, work):
	return (win and work and win[0] >= work[0] and win[1] >= work[1]
			and win[0] + win[2] <= work[0] + work[2] and win[1] + win[3] <= work[1] + work[3])


def centred(win, work):
	return (win and work and abs((2 * win[0] + win[2]) - (2 * work[0] + work[2])) <= 2
			and abs((2 * win[1] + win[3]) - (2 * work[1] + work[3])) <= 2)


def ini_display():
	if not os.path.isfile(INI):
		return {}
	out = {}
	for line in io.open(INI, encoding="utf-8", errors="replace").read().splitlines():
		key, _, value = line.partition("=")
		if key.strip() in DISPLAY_KEYS:
			out[key.strip()] = value.strip()
	return out


def cut_copy(src, out_dir, starts):
	"""A copy of eval script `src` under the same name with every line starting
	with one of `starts` made an `echo` (the line count stays)."""
	text = io.open(src, encoding="utf-8").read()
	lines = [("echo cut by the self-test" if l.startswith(starts) else l) for l in text.splitlines()]
	dst = os.path.join(out_dir, os.path.basename(src))
	io.open(dst, "w", encoding="utf-8", newline="").write("\n".join(lines) + "\n")
	return dst


# ---------------------------------------------------------------------------
# PLACEMENT
# ---------------------------------------------------------------------------

def placement(script):
	print("PLACEMENT - where a Windowed Apply puts the window (code-review C196)")
	before = ini_display()
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script], headless=False, timeout=300)
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")
	sts = statuses(text)
	first = sts[0] if sts else {}

	st, run = first.get("staged", {}), first.get("running", {})
	check(st.get("mode") == "windowed" and run.get("mode") == "windowed"
		  and size(st.get("size")) is not None and st.get("size") == run.get("size"),
		  "an untouched staging is the window as it is: Windowed, its own client size",
		  f"staged {st.get('mode')} {st.get('size')}, running {run.get('mode')} {run.get('size')}")

	s = after(sts, "video apply")
	st, run = s.get("staged", {}), s.get("running", {})
	check(run.get("size") is not None and run.get("size") == first.get("running", {}).get("size")
		  and inside(rect(run.get("window")), rect(st.get("work"))),
		  "an untouched Apply keeps that size, inside the monitor's work area",
		  f"size {first.get('running', {}).get('size')} -> {run.get('size')}, "
		  f"window {run.get('window')} work {st.get('work')}")

	s = after(sts, "video apply windowed native")
	st, run = s.get("staged", {}), s.get("running", {})
	win, work, got, asked = rect(run.get("window")), rect(st.get("work")), size(run.get("size")), size(st.get("size"))
	fills = win and work and abs(win[2] - work[2]) <= 2 and abs(win[3] - work[3]) <= 2
	check(asked and got and got[0] < asked[0] and got[1] < asked[1] and inside(win, work) and fills,
		  "a native-size client is shrunk until its frame fills the work area, and no further",
		  f"asked {st.get('size')} got {run.get('size')}, window {run.get('window')} work {st.get('work')}")

	# The staging after that apply is the size ASKED for, and the window the one
	# the work area allowed. Opening Settings re-seeds it from the window as it
	# is now (RestageVideo, through `video restage`, the entries' own function).
	native = after(sts, "video apply windowed native")
	n_st, n_run = native.get("staged", {}), native.get("running", {})
	s = after(sts, "video restage")
	st, run = s.get("staged", {}), s.get("running", {})
	check(size(n_st.get("size")) is not None and n_st.get("size") != n_run.get("size")
		  and st.get("mode") == "windowed" and size(st.get("size")) is not None
		  and st.get("size") == run.get("size") == n_run.get("size"),
		  "re-staged as opening Settings does, the staging is the shrunk window, not the size asked for",
		  f"after the apply: staged {n_st.get('size')} running {n_run.get('size')}; "
		  f"re-staged {st.get('mode')} {st.get('size')} running {run.get('size')}")

	s = after(sts, "video apply windowed 1280x720")
	st, run = s.get("staged", {}), s.get("running", {})
	win, work = rect(run.get("window")), rect(st.get("work"))
	check(run.get("size") == "1280x720" and inside(win, work) and centred(win, work),
		  "a size that fits is that size, centred in the work area",
		  f"size {run.get('size')}, window {run.get('window')} work {st.get('work')}")

	s = after(sts, "video apply windowed 1280x720 monitor last")
	st, run = s.get("staged", {}), s.get("running", {})
	win, work = rect(run.get("window")), rect(st.get("work"))
	# The monitor count comes from the FIRST status, which lists them whatever
	# the script did after it, so a cut apply cannot make a one-monitor run.
	global monitors
	monitors = len(first.get("monitors", []))
	label = "on the last monitor the window is in ITS work area, centred, and on it"
	if monitors == 1:
		# The last monitor is monitor 0 again, which the 1280x720 check already
		# covered: counting it would claim the half of C196 that matters - a
		# window placed on a NON-primary monitor - on a machine that has none.
		print(f"  [skip] {label}")
		print("         (one monitor here - see monitors= on the verdict line)")
	else:
		check(run.get("size") == "1280x720" and st.get("monitor") is not None
			  and run.get("monitor") == st.get("monitor") and str(monitors - 1) == st.get("monitor")
			  and inside(win, work) and centred(win, work), label,
			  f"monitor staged {st.get('monitor')} running {run.get('monitor')} of {monitors}, "
			  f"window {run.get('window')} work {st.get('work')}")

	drop(text, sts, after(sts, "video apply windowed 1280x720 monitor last"), "simulated")

	after_ini = ini_display()
	check(before == after_ini, "a script's apply is never saved: settings.ini's display keys are as they began",
		  f"before {before} after {after_ini}")
	saved = {s.get("saved") for s in sts}
	check(len(sts) >= 1 and len(saved) == 1 and None not in saved,
		  "and every status read the same saved choice", f"{sorted(map(str, saved))}")

	verdict = next((l for l in text.splitlines() if "eval RESULT=" in l), "")
	check(harness_game.finished(code, text) and "eval RESULT=PASS" in verdict,
		  "the placement script ran to its verdict, its refusals refused",
		  f"exit {code}: {verdict.split('eval ', 1)[-1] if verdict else 'no verdict line'}")
	if "eval RESULT=FAIL" in verdict:
		harness_game.report_failed_script(text, "display.eval")


# ---------------------------------------------------------------------------
# DROP and EXCLUSIVE
# ---------------------------------------------------------------------------

def drop(text, sts, before_status, how):
	"""`video drop` (C194): the frame after it finds the swapchain's state is not
	the one the device last saw and rebuilds the back buffers - said in the log
	after the drop - and every frame after that presents. Read from the
	`video swapchain:` line of `before_status` and of the status after the
	frames that follow the drop (`echo dropped-frame-3`)."""
	before = before_status.get("swapchain", {})
	later = after(sts, "echo dropped-frame-3").get("swapchain", {})
	lines = text.splitlines()
	at = next((i for i, l in enumerate(lines) if "console: > video drop" in l), None)
	tail = lines[at + 1:] if at is not None else []
	said = any(f"video: exclusive state dropped ({how})" in l for l in tail)
	changed = any("swapchain: the exclusive state changed under the device" in l for l in tail)
	rebuilt = any("swapchain: back buffers recreated" in l for l in tail)
	b_rec, l_rec = num(before.get("recreates")), num(later.get("recreates"))
	b_sc, l_sc = num(before.get("statechanges")), num(later.get("statechanges"))
	b_pr, l_pr = num(before.get("presents")), num(later.get("presents"))
	# Five frames run from the drop to that status (the drop, three echoes, the
	# status itself), each presenting; at least four of them after the rebuild.
	check(said and changed and rebuilt and b_rec >= 0 and l_rec == b_rec + 1 and b_sc >= 0
		  and l_sc == b_sc + 1 and b_pr >= 0 and l_pr >= b_pr + 4 and later.get("exclusive") == "0",
		  f"a dropped exclusive state ({how}) is found by the next frame, the back buffers are "
		  "rebuilt once, and the frames after it present",
		  f"drop said: {said}; log: changed={changed} rebuilt={rebuilt}; recreates {b_rec} -> {l_rec}, "
		  f"state changes {b_sc} -> {l_sc}, presents {b_pr} -> {l_pr}, exclusive after "
		  f"{later.get('exclusive')}")


def exclusive(script):
	print("EXCLUSIVE - a real drop of exclusive full-screen, and its re-entry (C194; --exclusive)")
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script], headless=False, timeout=300)
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")
	sts = statuses(text)
	entered = after(sts, "video apply exclusive native")
	check(entered.get("swapchain", {}).get("exclusive") == "1"
		  and entered.get("swapchain", {}).get("wanted") == "1",
		  "Exclusive is entered", f"{entered.get('swapchain')}")
	drop(text, sts, entered, "real")
	lost = after(sts, "echo dropped-frame-3").get("swapchain", {})
	back = after(sts, "echo reentered").get("swapchain", {})
	check(lost.get("lost") == "1" and back.get("exclusive") == "1" and back.get("lost") == "0"
		  and num(back.get("reentries")) == num(lost.get("reentries")) + 1
		  and num(back.get("activations")) > num(lost.get("activations")),
		  "the activation that coming back sends re-enters the Exclusive the drop took",
		  f"after the drop {lost}; after the activation {back}")
	verdict = next((l for l in text.splitlines() if "eval RESULT=" in l), "")
	check(harness_game.finished(code, text) and "eval RESULT=PASS" in verdict,
		  "the exclusive script ran to its verdict",
		  f"exit {code}: {verdict.split('eval ', 1)[-1] if verdict else 'no verdict line'}")


# ---------------------------------------------------------------------------
# WARP
# ---------------------------------------------------------------------------

def num(text, default=-1):
	return int(text) if text is not None and re.fullmatch(r"-?\d+", text) else default


def warp(script):
	print("WARP - the monitors are the desktop's, not the rendering GPU's (C198, C199, C197)")
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script], ["-warp"], timeout=300)
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")
	sts = statuses(text)
	first = sts[0] if sts else {}

	# The boot: WARP is listed (it is not hardware, so it is added as the one
	# RUNNING) and staged, and the monitors are every one on the desktop.
	st, run, disp, mon = (first.get("staged", {}), first.get("running", {}),
						  first.get("displays", {}), first.get("mon", {}))
	staged_mon = mon.get(num(st.get("monitor")), {})
	real = {m.get("device") for m in mon.values()}
	check(num(run.get("adapter")) >= 0 and st.get("adapter") == run.get("adapter")
		  and "adaptername=WARP" in first.get("running_line", "")
		  and num(disp.get("monitors"), 0) >= 1 and len(mon) == num(disp.get("monitors"), 0)
		  and num(staged_mon.get("modes"), 0) >= 1 and staged_mon.get("device") == st.get("device"),
		  "a WARP boot lists and stages the running adapter, and its monitor and resolution "
		  "lists are not empty",
		  f"adapter staged {st.get('adapter')} running {run.get('adapter')}; "
		  f"{disp.get('monitors')} monitor(s); staged monitor {st.get('monitor')} with "
		  f"{staged_mon.get('modes')} mode(s); running: {first.get('running_line', '-')[:120]}")

	s = after(sts, "video apply borderless")
	st, run, mon = s.get("staged", {}), s.get("running", {}), s.get("mon", {})
	desk = rect(mon.get(num(st.get("monitor")), {}).get("desktop"))
	check(st.get("mode") == "borderless" and run.get("mode") == "borderless" and desk is not None
		  and rect(run.get("window")) == desk and run.get("device") == st.get("device"),
		  "Borderless applies through `video apply` on WARP: the window covers the staged monitor",
		  f"running {run.get('mode')} window {run.get('window')} on {run.get('device')}, "
		  f"staged {st.get('device')} desktop {desk}")

	# The ghost, then the display change that must drop it.
	g = after(sts, "video ghost")
	c = after(sts, "video displaychange")
	gst, cst, gd, cd = (g.get("staged", {}), c.get("staged", {}), g.get("displays", {}),
						c.get("displays", {}))
	n0 = num(disp.get("monitors"), 0)
	check(gst.get("device") == GHOST and num(gd.get("monitors")) == n0 + 1
		  and num(cd.get("monitors")) == n0
		  and num(cd.get("displaychanges"), 0) > num(gd.get("displaychanges"), 0)
		  and num(cd.get("refreshes"), 0) > num(gd.get("refreshes"), 0)
		  and cst.get("device") in real and GHOST not in {m.get("device") for m in c.get("mon", {}).values()},
		  "a display change re-reads the list: the monitor that is not there is gone and a real "
		  "one is staged",
		  f"ghost staged {gst.get('device')} with {gd.get('monitors')} monitor(s); after the "
		  f"change {cd.get('monitors')} monitor(s), changes {gd.get('displaychanges')} -> "
		  f"{cd.get('displaychanges')}, refreshes {gd.get('refreshes')} -> {cd.get('refreshes')}, "
		  f"staged {cst.get('device')}")

	ini = next((l.split("console: ", 1)[1] for l in text.splitlines()
				if "console: video ini: " in l), "")
	f = dict(re.findall(r"(\w+)=(\S+)", ini.split(" adapter=", 1)[0]))
	adapter = ini.split(" adapter=", 1)[1] if " adapter=" in ini else ""
	check(bool(ini) and f.get("monitor") == cst.get("device") and f.get("monitorread") == f.get("monitor")
		  and f.get("monitorsame") == "1" and f.get("adaptersame") == "1"
		  and re.match(r"[0-9a-f]+:[0-9a-f]+:[0-9a-f]+:[0-9a-f]+ \S", adapter) is not None,
		  "the staged monitor's device name and the GPU's identity survive settings.ini's text "
		  "both ways", f"{ini or 'no video ini line'} (staged {cst.get('device')})")

	# The script ran to its verdict, and the debug layer raised nothing at what the
	# run did call: making the WARP device and resizing the hidden swapchain
	# (Borderless, then Windowed). NOT frames in flight under that resize - the run
	# is headless, so no frame is ever drawn or submitted, and a resize drain cut
	# out of SetFullscreen would pass here. A drawn run that goes Borderless would
	# cover a monitor of whoever is at the machine, so that is left unchecked.
	verdict = next((l for l in text.splitlines() if "eval RESULT=" in l), "")
	d3d = [l for l in text.splitlines() if "d3d12 error" in l]
	check(harness_game.finished(code, text) and "eval RESULT=PASS" in verdict and not d3d,
		  "the WARP script ran to its verdict",
		  f"exit {code}: {verdict.split('eval ', 1)[-1] if verdict else 'no verdict line'}; "
		  f"d3d12 errors: {len(d3d)} {d3d[:1]}")
	if "eval RESULT=FAIL" in verdict:
		harness_game.report_failed_script(text, "displaywarp.eval")


# ---------------------------------------------------------------------------
# RELAUNCH
# ---------------------------------------------------------------------------

def child_pid(text):
	m = re.search(r"relaunching: pid (\d+) started as (.*) - it waits for this run to exit", text)
	return (int(m.group(1)), m.group(2)) if m else (None, "")


def wait_for_child(pid):
	"""Waits for the child to EXIT - headless with no script, it quits once its
	boot load lands - or for the wait to run out. Returns (log text, whether it
	was still running when the wait ran out)."""
	deadline = time.time() + CHILD_WAIT_S
	while time.time() < deadline:
		if pid not in harness_game.running_copies(EXE):
			return harness_game.read_log(LOG), False
		time.sleep(0.5)
	return harness_game.read_log(LOG), pid in harness_game.running_copies(EXE)


def end_child(pid):
	"""Kills the child by its id - only if that id is running THIS exe. Returns
	whether it was (and so was ended)."""
	if pid is None or pid not in harness_game.running_copies(EXE):
		return False
	subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
	for _ in range(40):
		if pid not in harness_game.running_copies(EXE):
			break
		time.sleep(0.25)
	return True


def relaunch(script):
	print("RELAUNCH - the GPU switch's relaunch keeps its world and the log (code-review C398)")
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script], ["-project", WORLD], timeout=180)
	pid, cmd = child_pid(text)
	stuck = False
	if pid:
		text, stuck = wait_for_child(pid)
	if stuck:
		end_child(pid)  # it did not end itself: the failure below says so
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")

	# Headless with no script, it has nothing to drive it: it must END ITSELF
	# once its boot load lands (Game::QuitOnceLoaded), with a clean exit line,
	# rather than idle unseen until a judge - or nobody - kills it.
	lines = text.splitlines()
	at = next((i for i, l in enumerate(lines) if "relaunched by pid" in l), None)
	tail = lines[at + 1:] if at is not None else []
	quit_line = any("headless with no script: nothing can drive this run" in l for l in tail)
	exited = bool(tail) and "Dungeon exited (code 0)" in tail[-1]
	check(pid is not None and not stuck and quit_line and exited,
		  "the parent started a child, which - headless with no script - quit on its own once loaded",
		  f"relaunching line: {'pid %s' % pid if pid else 'none'}; still running after "
		  f"{CHILD_WAIT_S} s: {stuck}; its quit line: {quit_line}; "
		  f"last line: {tail[-1] if tail else '-'!r}")

	# The command line, read back the way the child reads it.
	words = re.findall(r'"((?:[^"\\]|\\.)*)"|(\S+)', cmd)
	argv = [a or b for a, b in words]
	exe_ok = bool(argv) and os.path.normcase(os.path.abspath(argv[0])) == os.path.normcase(os.path.abspath(EXE))
	check(exe_ok, "the child is this exe, by its own path", f"command: {cmd or '(none)'}")
	rest = argv[1:]
	project = rest[rest.index("-project") + 1] if "-project" in rest[:-1] else None
	parent = rest[rest.index("-relaunched") + 1] if "-relaunched" in rest[:-1] else None
	check(project == WORLD and "-headless" in rest and "-unattended" in rest and "-eval" not in rest
		  and not any(a.endswith(".eval") for a in rest) and parent is not None and parent.isdigit(),
		  "with the parent's arguments - -project, -headless, -unattended - less its script, "
		  "and -relaunched <parent>", f"arguments: {rest}")

	marker = re.search(r"relaunched by pid (\d+): it exited (\d+) ms after", text)
	check(marker is not None and parent is not None and marker.group(1) == parent and at is not None,
		  "the child waited for that parent to exit before it wrote a line",
		  f"marker: {lines[at] if at is not None else 'none'}")

	# The parent's run, whole, before the child's first line: its start, the
	# relaunch, its shutdown, its heap report and its LAST line ("Dungeon
	# exited", written once the game and the device are torn down - which, with
	# a level loaded, takes longer than a child that did not wait takes to
	# write). And none of the parent's lines after it. The child shuts down too
	# now (it quits once loaded), so a shutdown, heap report or exit line is the
	# PARENT's only before the child's own quit line; past that they are its own.
	head = lines[:at] if at is not None else lines
	starts_head = sum(1 for l in head if "Dungeon starting..." in l)
	whole = (starts_head == 1 and any("relaunching: pid" in l for l in head)
			 and any("Dungeon shutting down." in l for l in head)
			 and any(l.startswith("[info ] heap[") for l in head)
			 and bool(head) and "Dungeon exited (code " in head[-1])
	quit_at = next((i for i, l in enumerate(tail) if "headless with no script: nothing can drive" in l),
				   len(tail))
	stray = [l for i, l in enumerate(tail)
			 if "relaunching: pid" in l or "eval: " in l
			 or (i < quit_at and ("Dungeon shutting down." in l or l.startswith("[info ] heap[")
								  or "Dungeon exited" in l))]
	check(at is not None and whole and not stray,
		  "the parent's log is whole and first: its start, the relaunch, its shutdown, its heap "
		  "report and its exit line last, none of it after the child's first line",
		  f"before the child: starts={starts_head} whole={whole} "
		  f"last={head[-1] if head else '-'!r}; parent lines after it: {stray[:3]}")

	starts_tail = sum(1 for l in tail if "Dungeon starting..." in l)
	world = re.search(r"Default world '([^']*)'", "\n".join(tail))
	check(starts_tail == 1 and world is not None and world.group(1) == WORLD,
		  "the child opened the world the parent was given (-project)",
		  f"child starts={starts_tail}, world {world.group(1) if world else 'none'}")
	return code


# ---------------------------------------------------------------------------

def main():
	selftest = "--selftest" in sys.argv
	if not os.path.exists(EXE):
		return 2, (f"{TOOL} RESULT=FAIL checks=0 failures=0 self_test={int(selftest)} - "
				   f"no debug build at {EXE}")
	guard = harness_game.RealTree(ROOT, own=[WORLD])
	harness_game.scratch_world(ROOT, WORLD)
	tmp = tempfile.mkdtemp(prefix="displaytest-")
	try:
		place, again, soft = PLACEMENT, RELAUNCH, WARP
		if selftest:
			place = cut_copy(PLACEMENT, tmp, CUT)
			again = cut_copy(RELAUNCH, tmp, CUT)
			soft = cut_copy(WARP, tmp, CUT)
		placement(place)
		warp(soft)
		relaunch(again)
		if "--exclusive" in sys.argv:
			exclusive(cut_copy(EXCLUSIVE, tmp, CUT) if selftest else EXCLUSIVE)
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
		# A child the run left (one that did not end itself, and a crash between
		# its start and the wait) is not left running: only ids running THIS
		# exe, which none was when the run began.
		for pid in harness_game.running_copies(EXE):
			print(f"  ending a game this run left running (pid {pid})")
			end_child(pid)
		harness_game.remove_world(ROOT, WORLD)
	guard.check(check)

	failed = {lbl for lbl, ok in results if not ok}
	if selftest:
		expected = {lbl for lbl, _ in results if lbl not in NOT_CUT}
		wrong = sorted((expected - failed) | (failed - expected))
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed with its line cut' if lbl in expected else 'failed with nothing cut'}")
		caught = not wrong
		return (0 if caught else 1,
				f"{TOOL} RESULT={'FAIL' if failed else 'PASS'} checks={len(results)} "
				f"failures={len(failed)} monitors={monitors} self_test=1 caught={int(caught)}")
	return (1 if failed else 0,
			f"{TOOL} RESULT={'FAIL' if failed else 'PASS'} checks={len(results)} "
			f"failures={len(failed)} monitors={monitors} self_test=0")


def refuse_unwindowed_start():
	"""PLACEMENT needs a Windowed start: the window is SHOWN, so a saved
	Borderless covers a monitor and a saved Exclusive switches it at boot, and
	the untouched staging then is not Windowed. Such a settings.ini is refused
	before any game starts (exit 2, nothing judged), never edited - it is the
	one this build's player uses."""
	saved = ini_display().get("fullscreen", "0")
	if saved != "0":
		mode = {"1": "Borderless", "2": "Exclusive"}.get(saved, f"fullscreen={saved}")
		print(f"{TOOL}: refused - {INI} saves the display mode {mode}; this judge shows its window "
			  "and needs a Windowed start (Settings -> Video -> Display Mode: Windowed, Apply)")
		print(f"{TOOL} RESULT=FAIL checks=0 failures=0 monitors=0 "
			  f"self_test={int('--selftest' in sys.argv)} - refused: not a Windowed start")
		sys.exit(2)


if __name__ == "__main__":
	# Never a stale exe, and never beside this worktree's own game, which shares
	# the log the run reads. Muted for the run, like every other harness. The
	# verdict is printed AFTER the volume is put back, so it is the last line
	# (CheckAll reads it there, tools\Verdict.ps1).
	harness_game.refuse_if_stale(EXE)
	harness_game.refuse_if_running(EXE)
	refuse_unwindowed_start()
	with harness_audio.muted(os.path.dirname(EXE)):
		code, verdict = main()
	print(verdict)
	sys.exit(code)
