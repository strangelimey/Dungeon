# tools/AITest.py - the monster AI's checks (code-review batch 33: where a
# monster can stand, and where a thing can come to rest - C58, C57, C74).
#
# Run:  python tools\AITest.py [--selftest]   (needs a debug build)
#
# Runs tools\EvalScripts\ai.eval headless and JUDGES what it printed. The eval
# runner's own verdict only says every line named a command; a monster that
# never moved reads PASS there. Each section of the script ends in a readout
# (`monsters`, `tally`, `castsvc floor`, the `drop` answers), and every check
# below is a claim about one section's readouts:
#
#   BRAZIER  crypt1 as shipped: a skeleton bolted awake behind the brazier
#            reaches a side of the party - not the brazier's square, which no
#            path reaches - and swings (C58).
#   DEADEND  a dead end has one side; a kiting mage nearer the party than a
#            brute takes none of it, so the brute reaches it and swings (C57).
#            The mage must be seen shooting (`mshots=`, a kiter's alone) before
#            the brute arrives, or the arrival proves nothing.
#   SIDEFILL a fleer held on the party's north side takes no side but FILLS
#            it, so the brute whose nearest side it is goes round to another
#            and swings, rather than stalling at the refused last step.
#   PIT      a drop onto a pit is refused while one beside it is laid; a throw
#            that ends against the wall beyond a pit comes down short of it,
#            while a flask BURSTS over the pit (nothing is left to hang there);
#            a rock thrown from the stairwell the party stands on comes down on
#            the party's square, never behind it; a skeleton goes round a pit to
#            the party's other side rather than standing over the shaft; and one
#            SHOVED toward the pit stops short of it - FreeSlotInCell's refusal on
#            its own, which the walk cannot isolate (C74).
#
# --selftest runs the same script with its `step` lines removed - no time
# passes, so no monster thinks, walks or swings and nothing in flight lands -
# and demands that EXACTLY the checks resting on time fail (STEP_FREE names the
# rest, which must still pass), so no check is satisfied by nothing happening.
# The one step it keeps is the party's walk up to the pit (WALK_SECTIONS).
import io
import os
import re
import sys
import tempfile

import harness_audio
import harness_game

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\ai.eval")

# crypt1's geometry the script relies on (assets/projects/dungeon-demo/levels):
# the party at 8,4 has open squares N, S and W and the brazier E.
BRAZIER_SIDES = {(8, 3), (8, 5), (7, 4)}
DEADEND_SIDE = (14, 19)   # arena room 9 4: the corridor's last square but one
# arena room 9 4 again, the party at the room's centre 14,12: its four sides,
# the north one held by the dazzled coward.
SIDEFILL_SIDES = {(14, 11), (14, 13), (13, 12), (15, 12)}
SIDEFILL_COWARD = (14, 11)
PIT = (3, 4)              # `stairadd pit 3 4` on crypt1
PIT_WALK_SIDE = (4, 5)    # the party at 3,5: its one side that is not the pit
STAIRWELL = (1, 1)        # crypt1's stairs_down, where the party arrives from crypt2
SHOVE_FROM, SHOVE_STOP = (5, 4), (4, 4)

results = []

# The checks that rest on no `step`: with every step cut (--selftest) these,
# and ONLY these, may still pass. A drop is an instant act, and so is its
# refusal; everything else needs the world to run.
STEP_FREE = {
	"a drop beside the pit is laid",
	"a drop onto the pit is refused, and nothing lies there",
	"a skeleton shoved toward the pit stops short of it",
	"the script ran to its end",
	"the game ran the script to its verdict",
}
# Sections whose steps --selftest keeps: the party's walk to the pit, which only
# sets the drops up (a drop needs its square seen, and only a step reveals one).
WALK_SECTIONS = {"pit-drop"}


def check(ok, label, detail=""):
	results.append((label, ok))
	print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
	if not ok and detail:
		print(f"         {detail}")


def run(script):
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script])
	log = text.splitlines()
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")
	# A run that died before its verdict is a failed run, not a log to judge as
	# if it were whole.
	check(harness_game.finished(code, text), "the game ran the script to its verdict",
		  f"exit code {code}, no verdict line")
	verdict = next((l for l in log if "eval RESULT=" in l), "")
	said = [l.split("console: ", 1)[1] for l in log if "console: " in l]
	return code, verdict, [l for l in said if not l.startswith("> ")]


def sections(lines):
	out, cur = {}, None
	for l in lines:
		m = re.match(r"=== (\S+) ===$", l)
		if m:
			cur = m.group(1)
			out[cur] = []
		elif cur:
			out[cur].append(l)
	return out


def monsters(sec):
	"""Every monster line, in order: (type, x, z, hp)."""
	out = []
	for l in sec:
		m = re.match(r"  (\w+) @ (\d+),(\d+)  hp ([\d.]+)", l)
		if m:
			out.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))))
	return out


def cells(sec, kind):
	"""Every square a monster of this kind was seen on, in order."""
	return [(x, z) for t, x, z, _ in monsters(sec) if t == kind]


def tally(sec):
	for l in reversed(sec):
		if l.startswith("TALLY "):
			return {k: v for k, v in re.findall(r"(\w+)=(\S+)", l)}
	return {}


def samples(sec):
	"""A sampled section split at each TALLY: [(its monster lines, its tally)]."""
	out, cur = [], []
	for l in sec:
		if l.startswith("TALLY "):
			out.append((monsters(cur), {k: v for k, v in re.findall(r"(\w+)=(\S+)", l)}))
			cur = []
		else:
			cur.append(l)
	return out


def cell_of(ms, kind):
	"""The square of the first monster of this kind in one `monsters` readout."""
	return next(((x, z) for t, x, z, _ in ms if t == kind), None)


def num(t, key):
	try:
		return float(t.get(key, "nan"))
	except ValueError:
		return float("nan")


def floor(sec, x, z):
	"""The ids `castsvc floor x z` reported lying on that square, or None."""
	for l in sec:
		m = re.match(rf"castsvc floor {x},{z}: (.*)$", l)
		if m:
			return [] if m.group(1) == "(none)" else m.group(1).split()
	return None


def dropped(sec, x, z):
	"""What the `drop` line for that square answered: 'laid', 'refused' or None."""
	for l in sec:
		m = re.match(rf"drop \w+ at {x},{z}: (\w+)$", l)
		if m:
			return m.group(1)
	return None


def last(seq, default=None):
	return seq[-1] if seq else default


def judge(lines):
	s = sections(lines)
	get = lambda name: s.get(name, [])

	print("BRAZIER - a brazier's square is no side (C58)")
	sec = get("brazier")
	t = tally(sec)
	check(num(t, "bolthits") >= 1, "the bolt struck the skeleton behind the brazier",
		  f"bolthits={t.get('bolthits')}")
	at = last(cells(sec, "skeleton"))
	check(at in BRAZIER_SIDES, "the woken skeleton reaches a side of the party, not the brazier's",
		  f"it ended at {at}; the sides are {sorted(BRAZIER_SIDES)}")
	check(num(t, "mswings") >= 1, "and swings within five seconds", f"mswings={t.get('mswings')}")

	print("DEADEND - a kiter takes no side (C57)")
	sec = get("deadend")
	t = tally(sec)
	ss = samples(sec)
	brute = [cell_of(ms, "skel_warrior") for ms, _ in ss]
	# The mage KITING is what the claim is about: a mage that never stirred (an
	# aggro below the five squares to the party, say) takes no side under any
	# rule, and the brute would reach the side with nothing proved. Its shots
	# are a kiter's alone, and they must come while the brute is on its way.
	first = next((i for i, (_, st) in enumerate(ss) if num(st, "mshots") >= 1), None)
	check(first is not None and all(c != DEADEND_SIDE for c in brute[:first + 1]),
		  "the mage kites (it shoots) while the brute is still on its way",
		  f"first shot by sample {first}; the brute per second: {brute}")
	at = last(brute)
	check(at == DEADEND_SIDE, "the brute reaches the dead end's one side past the kiting mage",
		  f"it ended at {at}, the side is {DEADEND_SIDE}; mage at {last(cells(sec, 'skel_mage'))}")
	check(num(t, "mswings") >= 1, "and swings", f"mswings={t.get('mswings')}")

	print("SIDEFILL - a body that takes no side still fills it (C57)")
	sec = get("sidefill")
	t = tally(sec)
	coward = last(cells(sec, "skel_coward"))
	at = last(cells(sec, "skel_warrior"))
	check(coward == SIDEFILL_COWARD and at in SIDEFILL_SIDES - {SIDEFILL_COWARD},
		  "the brute goes round the coward holding the north side, to another side",
		  f"the coward ended at {coward}, the brute at {at}; the sides are {sorted(SIDEFILL_SIDES)}")
	check(num(t, "mswings") >= 1, "and swings from the side it was sent to",
		  f"mswings={t.get('mswings')}")

	print("PIT - a pit is no floor to rest on (C74)")
	sec = get("pit-drop")
	beside = floor(sec, 5, 4)
	check(dropped(sec, 5, 4) == "laid" and beside is not None and "rock" in beside,
		  "a drop beside the pit is laid", f"drop said {dropped(sec, 5, 4)}, 5,4 holds {beside}")
	on = floor(sec, *PIT)
	check(dropped(sec, *PIT) == "refused" and on == [],
		  "a drop onto the pit is refused, and nothing lies there",
		  f"drop said {dropped(sec, *PIT)}, the pit holds {on}")
	sec = get("pit-throw")
	t = tally(sec)
	short = floor(sec, 4, 4)
	on = floor(sec, *PIT)
	check(num(t, "throwlandings") >= 1 and short is not None and "rock" in short and on == [],
		  "a throw ending against the wall beyond the pit comes down short of it",
		  f"landings={t.get('throwlandings')}, 4,4 holds {short}, the pit holds {on}")
	sec = get("pit-flask")
	t = tally(sec)
	check(num(t, "throwlandings") >= 1 and t.get("landat") == f"{PIT[0]},{PIT[1]}",
		  "a flask thrown at the wall beyond the pit bursts over it, not on the thrower's square",
		  f"landings={t.get('throwlandings')}, it came down at {t.get('landat')}")
	sec = get("stair-throw")
	t = tally(sec)
	on = floor(sec, *STAIRWELL)
	behind = floor(sec, STAIRWELL[0], STAIRWELL[1] + 1)
	check(num(t, "throwlandings") >= 1 and t.get("landat") == f"{STAIRWELL[0]},{STAIRWELL[1]}" and
		  on is not None and "rock" in on and behind == [],
		  "a rock thrown from the stairwell the party stands on lands there, not behind it",
		  f"landings={t.get('throwlandings')}, landat={t.get('landat')}, "
		  f"the stairwell holds {on}, the square behind {behind}")
	sec = get("pit-walk")
	t = tally(sec)
	walk = cells(sec, "skel_warrior")
	check(last(walk) == PIT_WALK_SIDE, "a skeleton reaches the party's side that is not the pit",
		  f"it ended at {last(walk)}")
	# Tied to the walk having HAPPENED: a skeleton that never moved never stood on
	# the pit either.
	moved = len(set(walk)) > 1
	check(moved and PIT not in walk, "and never stands on the pit on the way",
		  f"its squares: {walk}")
	check(num(t, "mswings") >= 1, "and swings from there", f"mswings={t.get('mswings')}")
	# FreeSlotInCell's hole refusal on its own: the shove steps a body square by
	# square with no plan, so the snapshot's blocked grid cannot stand in for it.
	sec = get("pit-shove")
	seen = cells(sec, "skel_bare")
	moved = any(l == "castsvc shove: moved=1" for l in sec)
	check(moved and seen[:1] == [SHOVE_FROM] and last(seen) == SHOVE_STOP,
		  "a skeleton shoved toward the pit stops short of it",
		  f"shove moved={1 if moved else 0}, its squares {seen}")
	check("end" in s, "the script ran to its end")


def main():
	if not os.path.exists(EXE):
		print(f"no debug build at {EXE}")
		return 2
	selftest = "--selftest" in sys.argv
	script = SCRIPT
	if selftest:
		text = io.open(SCRIPT, encoding="utf-8").read()
		kept, section = [], None
		for l in text.splitlines():
			m = re.match(r"echo === (\S+) ===$", l)
			if m:
				section = m.group(1)
			# Every step but the walk that brings the party beside the pit for
			# the drops (WALK_SECTIONS): that one is set-up, not a measured stretch
			# of time, and a party left where it was could not reach the squares.
			drop = l.startswith("step ") and section not in WALK_SECTIONS
			kept.append("echo skipped" if drop else l)
		cut = "\n".join(kept)
		fd, script = tempfile.mkstemp(suffix=".eval")
		with os.fdopen(fd, "w", encoding="utf-8") as fh:
			fh.write(cut + "\n")
	code, verdict, lines = run(script)
	print(f"eval: {verdict.split('] ', 1)[-1] if verdict else '(no verdict line)'}")
	judge(lines)
	if selftest:
		os.remove(script)
	failed = sum(1 for _, ok in results if not ok)
	if selftest:
		# Every check that rests on time passing must FAIL with the steps cut, and
		# the step-free ones must still pass - so the cut run is a real run, not a
		# broken one that fails everything.
		wrong = [lbl for lbl, ok in results if ok != (lbl in STEP_FREE)]
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed' if lbl not in STEP_FREE else 'failed'} with no steps")
		ok = not wrong
		print(f"aitest RESULT={'PASS' if ok else 'FAIL'} checks={len(results)} failures={failed} self_test=1")
		return 0 if ok else 1
	print(f"aitest RESULT={'PASS' if failed == 0 else 'FAIL'} checks={len(results)} failures={failed} self_test=0")
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	# Never a stale exe, and never beside this worktree's own game, which shares
	# the log the run reads. And muted for the run, like every other harness
	# (tools/harness_audio.py).
	harness_game.refuse_if_stale(EXE)
	harness_game.refuse_if_running(EXE)
	with harness_audio.muted(os.path.dirname(EXE)):
		code = main()
	sys.exit(code)
