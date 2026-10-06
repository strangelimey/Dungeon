# tools/AITest.py - the monster AI's checks (code-review batch 33: where a
# monster can stand, and where a thing can come to rest - C58, C57, C74; batch
# 34: the AI director's lifetime - C52, C69).
#
# Run:  python tools\AITest.py [--selftest]   (needs a debug build)
#
# Runs two scripts headless, each in its own process, and JUDGES what they
# printed. The eval runner's own verdict only says every line named a command;
# a monster that never moved reads PASS there.
#
# tools\EvalScripts\aiasync.eval runs on the WALL CLOCK - no lockstep, no
# `reset`, a real cold `newgame` - because that is where its defects live, and
# it must start its own process for the cold start to be one:
#
#   CONTINUE crypt1: a skeleton bolted awake hunts the party; the bucket workers
#            think from that (`aiwait`), go on thinking on the title, and a
#            Continue (a save from before the fight) must start every monster
#            unaware. Stale plans used to latch `aware` again (C52).
#   NEWGAME  the same fight, then Start New Game on the same level, reset in
#            place - the same claim (C52).
#   WORLDS   a world switch leaves exactly the new director's four `ai.bucketN`
#            workers - ids none of the old four had - none Dead: the old
#            director's used to linger, Dead, still holding jobs aimed at it
#            (C69). The run names its world (-project dungeon-demo) so the
#            switch does not save Test-World as the developer's last world,
#            and checks settings.ini's `project=` afterwards to show it did not.
#
# tools\EvalScripts\ai.eval runs in lockstep at timescale 0. Each section ends
# in a readout (`monsters`, `tally`, `castsvc floor`, the `drop` answers), and
# every check below is a claim about one section's readouts:
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
# --selftest runs the same scripts with their `step` lines removed - no time
# passes, so no monster thinks, walks or swings and nothing in flight lands -
# and demands that EXACTLY the checks resting on time fail (STEP_FREE names the
# rest, which must still pass), so no check is satisfied by nothing happening.
# The one step it keeps is the party's walk up to the pit (WALK_SECTIONS). The
# C52 claims are tied to the fight having happened, so they fail with it; the
# wall-clock waits, the in-place new game and the world switch rest on no step,
# and stay green.
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
ASYNC_SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\aiasync.eval")

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
# Where a new game on dungeon-demo begins (project.ini start_level/x/z), and the
# line StartNewGame logs when that level is the one already in memory - the
# world reset IN PLACE, monsters and all, rather than loaded afresh.
IN_PLACE = "New game started in crypt (crypt1 at 7,7)"
BUCKETS = [f"ai.bucket{b}" for b in range(4)]
# The async run's world, named so a world switch leaves settings.ini alone
# (Game::SwitchWorld saves the last world played unless -project chose it).
PROJECT = ("-project", "dungeon-demo")
SETTINGS = os.path.join(os.path.dirname(EXE), "settings.ini")
SWITCHED_TO = "Test-World"  # aiasync.eval's `worlds load`

results = []

# The checks that rest on no `step`: with every step cut (--selftest) these,
# and ONLY these, may still pass. A drop is an instant act, and so is its
# refusal; the wall-clock waits, the title, a new game reset in place and a
# world switch need no simulated time either. Everything else needs the world
# to run.
STEP_FREE = {
	"a drop beside the pit is laid",
	"a drop onto the pit is refused, and nothing lies there",
	"a skeleton shoved toward the pit stops short of it",
	"the script ran to its end",
	"the game ran ai.eval to its verdict",
	"the game ran aiasync.eval to its verdict",
	"the async script ran to its end",
	"lockstep stayed off for the whole async run",
	"continue: the workers thought on from that fight while the game sat on the title",
	"newgame: the workers thought on from that fight while the game sat on the title",
	"newgame: Start New Game reset crypt1 in place (no level load)",
	"a world switch leaves exactly the new director's four ai.bucket workers, none Dead",
	"the world switch left settings.ini's last world as it was",
}
# Sections whose steps --selftest keeps: the party's walk to the pit, which only
# sets the drops up (a drop needs its square seen, and only a step reveals one).
WALK_SECTIONS = {"pit-drop"}


def check(ok, label, detail=""):
	results.append((label, ok))
	print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
	if not ok and detail:
		print(f"         {detail}")


def last_world():
	"""settings.ini's `project=` - the world an ordinary launch opens - or None."""
	try:
		text = io.open(SETTINGS, encoding="utf-8").read()
	except OSError:
		return None
	return next((l[len("project="):] for l in text.splitlines() if l.startswith("project=")), None)


def run(script, name, extra=()):
	"""One script in its own process: (verdict line, the console lines, the
	whole log)."""
	code, text = harness_game.run_eval(EXE, ROOT, LOG, [script], extra)
	log = text.splitlines()
	for f in harness_game.fatal_lines(text):
		print(f"         game FATAL: {f}")
	# A run that died before its verdict is a failed run, not a log to judge as
	# if it were whole.
	check(harness_game.finished(code, text), f"the game ran {name} to its verdict",
		  f"exit code {code}, no verdict line")
	verdict = next((l for l in log if "eval RESULT=" in l), "")
	said = [l.split("console: ", 1)[1] for l in log if "console: " in l]
	return verdict, [l for l in said if not l.startswith("> ")], log


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


def awareness(sec):
	"""Every monster line's (type, aware) - `aware=` closes the line."""
	out = []
	for l in sec:
		m = re.match(r"  (\w+) @ \d+,\d+  hp [\d.]+.*  aware=(\d)$", l)
		if m:
			out.append((m.group(1), int(m.group(2))))
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


def split_at(sec, marker):
	"""The section's lines before and after the first line starting `marker`
	(the marker line itself in neither); everything before it, and [], when the
	marker never came."""
	for i, l in enumerate(sec):
		if l.startswith(marker):
			return sec[:i], sec[i + 1:]
	return sec, []


def judge_async(lines, log):
	"""aiasync.eval: the wall-clock half (C52, C69)."""
	s = sections(lines)
	get = lambda name: s.get(name, [])
	check("lockstep off" in lines and not any(l == "lockstep on" for l in lines),
		  "lockstep stayed off for the whole async run",
		  "the defects measured here live only where the workers think on the wall clock")

	def stale_plans(name, back, came_back):
		"""A fight, the workers thinking from it on the title, then `back` - and
		every monster unaware after it."""
		sec = get(name)
		before, after = split_at(sec, "back to the title")
		t = tally(before)
		fought = awareness(before)
		woke = num(t, "bolthits") >= 1 and ("skeleton", 1) in fought
		check(woke, f"{name}: the bolt woke the skeleton, which hunts the party before the title",
			  f"bolthits={t.get('bolthits')}, before the title {fought}")
		waits = [l for l in sec if l.startswith("aiwait:")]
		titled = "state menu" in after
		thought = (titled and len(waits) == 2 and
				   all(w.startswith("aiwait: every bucket published") for w in waits))
		check(thought, f"{name}: the workers thought on from that fight while the game sat on the title",
			  f"state menu seen: {titled}; aiwait said {waits}")
		_, returned = split_at(after, came_back)
		now = awareness(returned)
		hunting = [m for m in now if m[1] != 0]
		# Tied to the setup: an unaware monster proves nothing unless there was a
		# fight for stale plans to come from, and the workers thought from it.
		check(woke and thought and bool(now) and not hunting,
			  f"{name}: {back} after that fight starts every monster unaware",
			  f"after {back}: {now}")

	print("CONTINUE - a fight's stale plans do not survive Continue (C52)")
	stale_plans("continue", "Continue", "loaded: aitest_async")

	print("NEWGAME - nor a Start New Game on the same level (C52)")
	stale_plans("newgame", "Start New Game", "starting a new game")
	# The new game must have been the in-place reset: a level LOAD would hand
	# every monster a fresh id whatever ResetForNewGame did, and prove nothing.
	at = next((i for i, l in enumerate(log) if "console: === newgame ===" in l), None)
	in_place = at is not None and any(IN_PLACE in l for l in log[at:])
	check(in_place, "newgame: Start New Game reset crypt1 in place (no level load)",
		  f"no '{IN_PLACE}' line after the newgame section began")

	print("WORLDS - a world switch takes its AI workers with it (C69)")
	sec = get("worlds")
	before, after = split_at(sec, "switching to Test-World")
	rows = lambda part: [m.groups() for m in
						 (re.match(r"  #(\d+) (ai\.bucket\d) (\w+)", l) for l in part) if m]
	old, new = rows(before), rows(after)
	names = sorted(n for _, n, _ in new)
	dead = [r for r in new if r[2] in ("dead", "quarantd")]
	# The new world's game is up (Test-World starts on its world map)...
	landed = "state playing" in after or "state worldmap" in after
	# ...and the four are a NEW director's: ids are never reused, so a switch
	# that never landed - crypt1 still playing, the old director's four rows
	# listed again - shares every id and proves nothing.
	fresh = bool(new) and not ({i for i, _, _ in old} & {i for i, _, _ in new})
	check(landed and fresh and len(old) == 4 and names == BUCKETS and not dead,
		  "a world switch leaves exactly the new director's four ai.bucket workers, none Dead",
		  f"landed: {landed}; fresh ids: {fresh}; before {old}; after {new}")
	check("end" in s, "the async script ran to its end")


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


def cut_steps(path):
	"""A copy of the script with its `step` lines removed (but for the walk the
	pit drops need - WALK_SECTIONS), in a temp file the caller deletes."""
	text = io.open(path, encoding="utf-8").read()
	kept, section = [], None
	for l in text.splitlines():
		m = re.match(r"echo === (\S+) ===$", l)
		if m:
			section = m.group(1)
		# Every step but the walk that brings the party beside the pit for the
		# drops (WALK_SECTIONS): that one is set-up, not a measured stretch of
		# time, and a party left where it was could not reach the squares.
		drop = l.startswith("step ") and section not in WALK_SECTIONS
		kept.append("echo skipped" if drop else l)
	fd, cut = tempfile.mkstemp(suffix=".eval")
	with os.fdopen(fd, "w", encoding="utf-8") as fh:
		fh.write("\n".join(kept) + "\n")
	return cut


def main():
	if not os.path.exists(EXE):
		print(f"no debug build at {EXE}")
		return 2
	selftest = "--selftest" in sys.argv
	# The async script FIRST and alone: its opening `newgame` must be a cold
	# start, which only a fresh process gives it. It names its world with
	# -project, which is the world an -eval run opens anyway, because its
	# `worlds load Test-World` would otherwise remember Test-World in
	# settings.ini as the last world played - the developer's next launch.
	for path, name, extra, judge_it in (
			(ASYNC_SCRIPT, "aiasync.eval", PROJECT, judge_async),
			(SCRIPT, "ai.eval", (), None)):
		script = cut_steps(path) if selftest else path
		world_before = last_world()
		verdict, lines, log = run(script, name, extra)
		print(f"{name}: {verdict.split('] ', 1)[-1] if verdict else '(no verdict line)'}")
		if judge_it:
			judge_it(lines, log)
			world_after = last_world()
			check(world_after == world_before or world_after != SWITCHED_TO,
				  "the world switch left settings.ini's last world as it was",
				  f"project= was {world_before}, is {world_after} - the developer's next launch "
				  f"opens {world_after}")
		else:
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
