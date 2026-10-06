# tools/CombatTest.py - the combat rules' checks (docs/code-review-plan.md 0c).
#
# Run:  python tools\CombatTest.py [--selftest]   (needs a debug build)
#
# Runs tools\EvalScripts\combat.eval headless and JUDGES what it printed. The
# eval suites REPORT and never judge (docs/eval-harness.md), so a claim the code
# review's combat batches make about play lives here, one check per claim, each
# reading the readouts of one `=== <name> ===` section of the script:
#
#   WIND WARD  a bolt the ward turns is never rolled, so it teaches no `avoid`
#              and creeps no DEX (C11) - and the same volley unwarded does both,
#              so the warded section cannot pass by the archer teaching nothing
#              or by the readouts going unread.
#   HANDS      armour carried in a hand counts nowhere: the class, the
#              pipeline's soak and resists and the sheet's soak all read as if
#              the hand were empty, and worn they all move together (C12).
#   PIERCE     a critical whose piercing edge went under the plate teaches the
#              plate nothing (C11): through the SHIPPING call site, not the pure
#              rule RollTest checks - a swarm spawned `pierce` against a party in
#              plate, the lessons counted from the heavy-armour xp against the
#              tally's blows landed (struck) and criticals that went under
#              (pierced), one lesson's xp measured by the same fight unpierced.
#   FLIGHT END a flight that stopped against a wall or a shut door ends in the
#              last open square in front (FlightEnd): a burst bolt broken on a
#              shut door goes off on the caster's side and never reaches the
#              mummy beyond it (C43; the door opened, it does - the control), and
#              a firebolt flying past a lone member down the empty lane to break
#              on the rock behind him leaves its burn on him (C44). Both demand
#              the flight STOPPED in the door or the rock (the tally's
#              `wallstops=` / `stoppedin=`), since one whose reach ran out just
#              short ends in the same open square and passes under the old code.
#   BURST      a skel_magus's burst bolt (`bolt firebolt_burst`) that reaches the
#              party goes off there, on contact, on every member (C1) - and a
#              Wind Ward turns it first, so nothing goes off (Michael: a turned
#              bolt does not land).
#   GUST       a gust of half the bolt's strength leaves half its blast and half
#              its burn (C18), and one of its full strength leaves nothing: the
#              bolt falls without an expiry, so it never bursts in the party's
#              face at full force.
#   FLARE      a Firelight flare reaches 3 WALKING steps into open squares
#              (C17): it dazzles and scorches the mummy behind the party and
#              the one 3 open steps off, not the one 4 off, and kindles the
#              wall torch it reaches - and nothing through a shut door 3 steps
#              off (so the door, not a short reach, kept it out), no monster
#              still dazzled by an earlier flare elsewhere, no wall torch
#              through the rock.
#   WARDS      `effect <ward> ahead` lands each ward in its own school (C279):
#              a stone skin as earth, a fire shield as fire.
#   KILL       the blow that kills is the last (C5): a lit torch's fire burst,
#              swung or thrown, never lands on the corpse - `slain` moves by
#              exactly one, the corpse holds no grudge and no line says it
#              turned on anyone.
#   STATS      a stat point names its stat (C36): an INT and a WIL point earned
#              casting read "Intelligence" / "Willpower", and no `stat.` key
#              reaches the log anywhere in the run.
#   PUNCH      Punch, and the own swing of a held item with no damage, fight
#              unarmed (C39): with a key in the hand, every blow that lands
#              trains `unarmed` (xp gained = hits), and that hand parries
#              unarmed; a punch with the DAGGER hand trains unarmed too, and
#              blade not at all.
#   FUMBLE     a severe fumble drops what the hand held AS IT WAS (C10): Sera's
#              torch, burnt to 300 s and swung on a loaded die (`fumble severe`),
#              leaves her hand and lies in the party's square with those 300 s,
#              less the second it burned there - never relit full - and the line
#              names it.
#   FLASK      a fire flask that bursts on a monster trains `throwing`, one xp a
#              contact (C40), and one with nothing in its path, shattering where
#              its reach runs out, trains nothing (Michael: only contact trains).
#   SAVE       a save made with a fire flask in the air sets nothing off (C47):
#              nobody is hurt and nothing lands or bursts at the save, the flight
#              goes on to burst on its own, and loading the save finds the flask
#              whole on the floor of the square it was over - the square its
#              position (`flooritems` `at`) falls in, worked out HERE, not the
#              game's own `over` - both for a save in the party's square and for
#              one two squares out, which a save written at the party's feet or
#              a landing rule that never moved off the party would fail. Each
#              save must have been MADE in this run ("saved: <name>"), or a
#              stale file from an earlier run would be what loaded.
#   MELEE      a swing at a square several bone swarms share meets the FRONT
#              RANK, and of those the one in the swinger's own lane (C33):
#              Brand (front-left) and Sera (front-right) each punch, and only
#              the swarm in the quarter worked out HERE from `pos` and the
#              rule loses hp - side by side in front, Brand's is not the one
#              listed first, which both used to hit; four to a square, the
#              one in front of his lane, never the one behind it.
#   CLIPS      a cosmetic clip moves no dice (C73): a skeleton swinging at
#              the party (rungs/clips-skeleton.eval) swept over four seeds as
#              it is authored (one clip a state, so it draws nothing) and
#              again with a second attack clip (`monsterclips`), which draws a
#              variation every swing it starts (TALLY `clipdraws=`) - and
#              every other TALLY field matches, sample for sample.
#   NOTICED    an attack is noticed whatever came of it (C34): a firebolt that
#              MISSED a lurker lying dormant beyond its trigger wakes it (aware
#              0 -> 1, the tally's `boltmisses=1 bolthits=0`, since a bolt that
#              landed woke it under the old rule too) while its twin with no
#              shot sleeps through the same window (the control: `freeze on`
#              does not stop the brain latching `aware`), and a RESTING party is
#              roused as `attacked` (`rest until`'s stop and the `rest`
#              readout) at the first attack that reaches it: a skeleton's swing
#              that missed (one swing, none landed), one that landed but a water
#              veil drank whole (nobody hurt, the veil drawn on), a bolt the
#              Wind Ward turned, and a BURST bolt it turned - the one carrier
#              answered outside fx::Deal - with no blast.
#
# And the script as a whole must run CLEAN - its verdict a PASS, nothing in it
# refused or unknown - since a refused line (a save that failed, say) prints a
# notice the section checks might not read.
#
# Each later combat batch adds its sections to combat.eval and its checks here.
#
# --selftest runs the same script with every line that SETS UP a claim cut (the
# effects, spawns, equips, wears, casts, bolts, loaded fumbles, throws and clip
# tables - CUT below) and demands that
# EXACTLY the checks resting on one fail (SETUP_FREE names the rest, which must
# still pass), so no check is satisfied by nothing happening. The cut copy is
# written beside the script, since a `sweep` path resolves against the folder of
# the script that names it.
import io
import math
import os
import re
import sys
import tempfile

import harness_audio
import harness_game

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\combat.eval")
NAMES = ["Brand", "Sera", "Maren", "Tilo"]

# What --selftest cuts: the lines that put a claim's cause in place.
CUT = ("effect ", "spawn ", "equip ", "wear ", "cast ", "bolt ", "fumble ", "throw ",
	   "monsterclips ")

# The checks that rest on none of the cut lines: with them cut, these, and ONLY
# these, may still pass.
SETUP_FREE = {
	"char prints the defense line every time it is asked",
	"unarmoured, no class, no soak",
	"the script ran to its end",
	"the game ran the script to its verdict",
	# A cut line becomes an `echo`, and nothing after one is refused for it: the
	# cut run is a real run, so it too must run clean.
	"the script ran clean: nothing refused or unknown, and it ended in play",
}

results = []


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
	check(harness_game.finished(code, text), "the game ran the script to its verdict",
		  f"exit code {code}, no verdict line")
	verdict = next((l for l in log if "eval RESULT=" in l), "")
	# The verdict's own count: a line the game refused (a save that failed, a
	# command used wrong) or did not know prints a notice a section check need
	# not read, and the run still reaches its verdict - which then says FAIL.
	check("eval RESULT=PASS" in verdict,
		  "the script ran clean: nothing refused or unknown, and it ended in play",
		  verdict.split("] ", 1)[-1] if verdict else "no verdict line")
	said = [l.split("console: ", 1)[1] for l in log if "console: " in l]
	return code, verdict, [l for l in said if not l.startswith("> ")], text


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


def chars(sec):
	"""Every `char` block in the section, in order: a dict per block with the
	member's name, stats {str/dex/vit/wil/int: value}, skills {id: xp}, creep
	{stat: pool}, effects {id: magnitude}, hands [id, id], the skill each hand
	parries with [skill, skill] (None where the line names none) and the defense
	line's fields (or None if it printed none)."""
	out, cur = [], None
	for l in sec:
		m = re.match(r"  (\w+)  str (\d+) dex (\d+) vit (\d+) wil (\d+) int (\d+)", l)
		if m:
			cur = {"name": m.group(1), "skills": {}, "creep": {}, "effects": {},
				   "hands": [], "parries": [], "defense": None,
				   "stats": dict(zip(("str", "dex", "vit", "wil", "int"),
									 (int(g) for g in m.groups()[1:])))}
			out.append(cur)
			continue
		if cur is None:
			continue
		m = re.match(r"    skill (\S+)\s+level \d+ \(([\d.]+) xp\)", l)
		if m:
			cur["skills"][m.group(1)] = float(m.group(2))
			continue
		m = re.match(r"    creep (\S+)\s+([\d.]+) toward", l)
		if m:
			cur["creep"][m.group(1)] = float(m.group(2))
			continue
		m = re.match(r"    effect (\w+) ([\d.-]+) ", l)
		if m:
			cur["effects"][m.group(1)] = float(m.group(2))
			continue
		m = re.match(r"    hand\d (\S+)(?: parries (\S+))?", l)
		if m:
			cur["hands"].append(m.group(1))
			cur["parries"].append(m.group(2))
			continue
		m = re.match(r"    defense class (\w+) soak ([\d.-]+) sheet ([\d.-]+) resist (.*)$", l)
		if m:
			res = {} if m.group(4).strip() == "-" else {
				k: float(v) for k, v in re.findall(r"(\w+)=([\d.-]+)", m.group(4))}
			cur["defense"] = {"class": m.group(1), "soak": float(m.group(2)),
							  "sheet": float(m.group(3)), "resist": res}
	return out


def tally(sec):
	for l in sec:
		if l.startswith("TALLY "):
			return {k: v for k, v in re.findall(r"(\w+)=(\S+)", l)}
	return {}


def tallies(sec):
	"""Every TALLY line in the section, in order."""
	return [{k: v for k, v in re.findall(r"(\w+)=(\S+)", l)} for l in sec if l.startswith("TALLY ")]


def messages(sec):
	"""The HUD message log lines every `messages` readout in the section
	printed, in order (a line printed by two readouts appears twice)."""
	return [l[4:] for l in sec if l.startswith("  | ")]


def grudges(sec):
	"""The section's `grudges` readout: one row per monster holding threat
	(`<kind>#<id> [a b c d] lock=<name>`), [] for "no threat anywhere", None
	if the section printed neither."""
	rows = [l.strip() for l in sec if re.match(r"\s+\S+#\d+ \[", l)]
	if rows:
		return rows
	return [] if "no threat anywhere" in sec else None


def monster_rows(sec, kind):
	"""Every `monsters` row for monsters of `kind` in the section, in order: a
	dict per row with its cell, hp, its slot in the square (None for a size
	that has only one), whether it is dead, whether it is AWARE of the party
	(None if the row does not say), its effects {id: magnitude} and each
	effect's school {id: school}."""
	out = []
	for l in sec:
		m = re.match(r"\s+(\S+) @ (\d+),(\d+)\s+hp ([\d.-]+)(.*)$", l)
		if m and m.group(1) == kind:
			slot = re.match(r"\s+slot (\d+)", m.group(5))
			aware = re.search(r"\baware=([01])\b", m.group(5))
			out.append({"cell": (int(m.group(2)), int(m.group(3))), "hp": float(m.group(4)),
						"slot": int(slot.group(1)) if slot else None,
						"dead": "(dead)" in m.group(5),
						"aware": aware.group(1) == "1" if aware else None,
						"effects": {k: float(v) for k, v in
									re.findall(r"\[(\w+) ([\d.-]+) ", m.group(5))},
						"schools": dict(re.findall(r"\[(\w+) [\d.-]+ [\d.-]+s (\w+)", m.group(5)))})
	return out


def fires(sec):
	"""Every `castsvc fire` readout in the section, in order: True for a lit
	fire, False for one out, None for no fire there."""
	out = []
	for l in sec:
		m = re.match(r"castsvc fire: kind=(\w+) lit=(\d)", l)
		if m:
			out.append(None if m.group(1) == "none" else m.group(2) == "1")
	return out


def door_rows(sec):
	"""Every door `breakables` listed in the section, in order: a (cell, hp)
	pair per row."""
	return [((int(m.group(1)), int(m.group(2))), float(m.group(3))) for m in
			(re.match(r"\s+door \S+ @ (\d+),(\d+) hp=([\d.-]+)/", l) for l in sec) if m]


def stopped_in(t):
	"""The square the tally's last WALL stop stopped in, or None with none: the
	stone's or the shut door's own, not the open square in front where the
	flight ended (DungeonWorld::Tally::wallStops)."""
	m = re.match(r"(\d+),(\d+)$", t.get("stoppedin", ""))
	return (int(m.group(1)), int(m.group(2))) if m else None


def party_reads(sec):
	"""Every `party` readout in the section, in order: a dict per readout of
	member name -> hp. A readout starts at slot 0 (a member's effects line may
	sit between two members' rows)."""
	out = []
	for l in sec:
		m = re.match(r"  \[(\d)\] (\w+)\s+hp ([\d.-]+)/", l)
		if not m:
			continue
		if m.group(1) == "0" or not out:
			out.append({})
		out[-1][m.group(2)] = float(m.group(3))
	return out


def repel(sec):
	"""The section's `castsvc repel` readout as (weakened, turned), or None."""
	for l in sec:
		m = re.match(r"castsvc repel: weakened=(\d+) turned=(\d+)$", l)
		if m:
			return int(m.group(1)), int(m.group(2))
	return None


def losses(sec):
	"""What each member lost between the section's first and last `party`
	readouts, or None without two."""
	reads = party_reads(sec)
	if len(reads) < 2 or set(reads[0]) != set(reads[-1]):
		return None
	return {n: reads[0][n] - reads[-1][n] for n in reads[0]}


def burns(sec):
	"""{member: burn magnitude} over the section's `char` readouts (0 = none)."""
	return {c["name"]: c["effects"].get("burn", 0.0) for c in chars(sec)}


def held_torches(sec):
	"""Every `torch` readout in the section, in order: a list per readout of
	(member, hand, id, charge), one per filled hand. A readout ends with its
	`wall torch ahead` line."""
	out, cur = [], []
	for l in sec:
		m = re.match(r"  \[(\d)\] \w+ hand (\d): (\S+) charge ([\d.-]+)$", l)
		if m:
			cur.append((int(m.group(1)), int(m.group(2)), m.group(3), float(m.group(4))))
		elif l.startswith("  wall torch ahead:"):
			out.append(cur)
			cur = []
	return out


def floor_items(sec):
	"""Every item row the section's `flooritems` readouts printed, in order:
	(cell, id, charge)."""
	out = []
	for l in sec:
		m = re.match(r"flooritems (\d+),(\d+): (\S+) slot -?\d+ charge ([\d.-]+)$", l)
		if m:
			out.append(((int(m.group(1)), int(m.group(2))), m.group(3), float(m.group(4))))
	return out


def flying(sec):
	"""Every thrown item in the air the section's `flooritems` readouts listed:
	(id, charge, where it is in squares (x, z), the square it is over - the one
	a save made then writes it in, by the game's own rule)."""
	out = []
	for l in sec:
		m = re.match(r"flooritems flying: (\S+) charge ([\d.-]+) at ([\d.-]+),([\d.-]+) "
					 r"over (\d+),(\d+)$", l)
		if m:
			out.append((m.group(1), float(m.group(2)), (float(m.group(3)), float(m.group(4))),
						(int(m.group(5)), int(m.group(6)))))
	return out


def party_pos(sec):
	"""The party's square and facing from the section's `pos` line, or None."""
	for l in sec:
		m = re.match(r"(\d+),(\d+) facing (north|east|south|west)$", l)
		if m:
			return (int(m.group(1)), int(m.group(2))), m.group(3)
	return None


# A facing's step, +x east and +z south (Party's grid).
STEP = {"north": (0, -1), "east": (1, 0), "south": (0, 1), "west": (-1, 0)}


def front_in_lane(facing, member, slots, dim=2):
	"""Of the occupied `slots` of the square ahead (slot = row*dim + col, row
	along +z, col along +x), the one roster slot `member`'s swing should meet,
	worked out here from the rule rather than read off the game: the FRONT rank
	first (nearest the party), then the member's own lane - even members stand
	on the party's left, odd on its right, a quarter of a square off its middle."""
	dx, dz = STEP[facing]
	left = (dz, -dx)  # the facing turned a quarter to its left
	lane = 0.25 if member % 2 == 0 else -0.25

	def key(s):
		ox = ((s % dim) + 0.5) / dim - 0.5
		oz = ((s // dim) + 0.5) / dim - 0.5
		return (round(ox * dx + oz * dz, 6), round(abs(ox * left[0] + oz * left[1] - lane), 6))
	return min(slots, key=key) if slots else None


def rest_end(sec):
	"""How the section's rest ended, read twice: the reason `rest until` gave
	for stopping the clock (None if it ran to its cap, or printed no line) and
	the reason the bare `rest` readout names after it (None if it names none)."""
	stopped = next((m.group(1) for m in (re.match(r"rested [\d.]+s - rest ended: (\w+)$", l)
										 for l in sec) if m), None)
	named = next((m.group(1) for m in (re.match(r"rest off \(world x\d+\)\s+last ended: (\w+)$", l)
									   for l in sec) if m), None)
	return stopped, named


def clip_lists(sec, state):
	"""Every list of clips the section's `monsterclips` readouts printed for
	`state`, in order (a readout prints a line per state it lists)."""
	return [l.split(" = ", 1)[1].split() for l in sec if l.startswith(f"  {state} = ")]


def num(t, key):
	try:
		return float(t.get(key, "nan"))
	except ValueError:
		return float("nan")


def heavy_gain(sec):
	"""The heavy-armour xp each member gained across a section that printed all
	four `char`s before its fight and all four after; None if it did not."""
	cs = chars(sec)
	if len(cs) != 8:
		return None
	xp = lambda c: c["skills"].get("heavy_armor", 0.0)
	return [xp(cs[i + 4]) - xp(cs[i]) for i in range(4)]


def in_plate(cs):
	"""Every readout in the section says heavy armour (eight of them)."""
	return len(cs) == 8 and all(c["defense"] and c["defense"]["class"] == "heavy" for c in cs)


def lessons(gains, lesson):
	"""Each member's gain as a whole number of lessons, or None if one is not
	(`char` prints xp to 0.01, so a gain is a before/after pair each rounded):
	a lesson pays a fixed xp, so a gain between two counts means something else
	trained the skill."""
	if not lesson > 0.0:
		return None
	counts = [round(g / lesson) for g in gains]
	if any(abs(g - n * lesson) > 0.05 for g, n in zip(gains, counts)):
		return None
	return counts


def judge(lines, text):
	s = sections(lines)
	get = lambda name: s.get(name, [])

	print("WIND WARD - a turned bolt teaches nothing (C11)")
	warded = chars(get("windward"))
	spent = sum(99.0 - c["effects"]["windward"] for c in warded if "windward" in c["effects"])
	avoid = {c["name"]: c["skills"].get("avoid", 0.0) for c in warded}
	dex = {c["name"]: c["creep"].get("dexterity", 0.0) for c in warded}
	check(len(warded) == 4 and all("windward" in c["effects"] for c in warded) and spent >= 1.0,
		  "the ward turned the archer's bolts", f"charges spent {spent:.0f} over {len(warded)} readouts")
	check(spent >= 1.0 and len(warded) == 4 and not any(avoid.values()),
		  "and no member it turned one for gained avoid xp", f"avoid xp {avoid}")
	check(spent >= 1.0 and len(warded) == 4 and not any(dex.values()),
		  "nor any DEX creep", f"dexterity creep {dex}")
	t = tally(get("windward"))
	check(spent >= 1.0 and num(t, "taken") == 0.0, "and nothing reached anyone's health",
		  f"taken={t.get('taken')}")
	bare = chars(get("windward-control"))
	taught = {c["name"]: c["skills"].get("avoid", 0.0) for c in bare}
	crept = {c["name"]: c["creep"].get("dexterity", 0.0) for c in bare}
	check(len(bare) == 4 and any(taught.values()),
		  "the same volley unwarded teaches avoid (a miss is rolled)", f"avoid xp {taught}")
	# The DEX check above reads an absent creep line as zero, so without this a
	# changed `creep` line (or stat id) would pass it on nothing parsed.
	check(len(bare) == 4 and any(crept.values()),
		  "and creeps DEX, so the creep lines are being read", f"dexterity creep {crept}")

	print("HANDS - armour carried in a hand counts nowhere (C12)")
	reads = chars(get("hand-armor"))
	d = [c["defense"] for c in reads]
	check(len(d) == 3 and all(x is not None for x in d),
		  "char prints the defense line every time it is asked", f"{d}")
	if len(d) == 3 and all(x is not None for x in d):
		base, held, worn = d
		check(base["class"] == "none" and base["soak"] == 0.0 and base["sheet"] == 0.0,
			  "unarmoured, no class, no soak", f"{base}")
		check(reads[1]["hands"][:1] == ["plate_cuirass"] and held == base,
			  "a cuirass in a hand leaves class, soak and resists as they were",
			  f"hands {reads[1]['hands']} held {held} base {base}")
		check(worn["class"] == "heavy" and worn["soak"] > 0.0 and worn["resist"] != base["resist"],
			  "worn, it moves the class, the soak and the resists", f"{worn}")
		check(all(x["soak"] == x["sheet"] for x in d) and worn["soak"] > 0.0,
			  "the sheet's soak is the pipeline's, in every readout", f"{d}")
	else:
		for lbl in ("unarmoured, no class, no soak",
					"a cuirass in a hand leaves class, soak and resists as they were",
					"worn, it moves the class, the soak and the resists",
					"the sheet's soak is the pipeline's, in every readout"):
			check(False, lbl, "no defense line to read")

	print("PIERCE - a piercing critical teaches the plate nothing (C11)")
	ctrl, pier = get("pierce-control"), get("pierce-crit")
	cg, pg = heavy_gain(ctrl), heavy_gain(pier)
	ct, pt = tally(ctrl), tally(pier)
	c_struck, c_pierced = num(ct, "struck"), num(ct, "pierced")
	p_struck, p_pierced = num(pt, "struck"), num(pt, "pierced")
	check(all(in_plate(chars(x)) for x in (ctrl, pier)),
		  "the whole party wears plate in both fights",
		  f"{[[c['defense'] and c['defense']['class'] for c in chars(x)] for x in (ctrl, pier)]}")
	# The control: no piercing edge, so every blow that landed on a standing
	# member met the plate and taught it one lesson. That fixes what a lesson
	# pays (kXp x the heavy class's learn knob) without the judge knowing it.
	ok_ctrl = (cg is not None and c_struck >= 1 and c_pierced == 0
			   and num(ct, "downed") == 0 and sum(cg) > 0.0)
	check(ok_ctrl, "an ordinary blow the plate met teaches heavy armour (the control)",
		  f"struck={ct.get('struck')} pierced={ct.get('pierced')} downed={ct.get('downed')} "
		  f"heavy xp gained {cg}")
	lesson = sum(cg) / c_struck if ok_ctrl else float("nan")
	cn = lessons(cg, lesson) if ok_ctrl else None
	check(ok_ctrl and cn is not None and sum(cn) == c_struck,
		  "and every blow that landed taught exactly one lesson (the count is sound)",
		  f"lessons {cn} of {lesson:.3f} xp, struck {c_struck}")
	ok_p = (ok_ctrl and pg is not None and num(pt, "downed") == 0 and p_pierced >= 1)
	check(ok_p, "the piercing swarm drove criticals under the plate",
		  f"struck={pt.get('struck')} pierced={pt.get('pierced')} downed={pt.get('downed')}")
	pn = lessons(pg, lesson) if ok_p else None
	check(ok_p and pn is not None and sum(pn) == p_struck - p_pierced,
		  "a piercing critical teaches the plate nothing (lessons = struck - pierced)",
		  f"lessons {pn} (heavy xp gained {pg}), struck {p_struck}, pierced {p_pierced}"
		  f" - teaching every blow would be {p_struck}")
	check(ok_p and pn is not None and p_struck - p_pierced >= 1 and sum(pn) >= 1,
		  "and the piercing edge's ordinary blows, which the plate met, still teach",
		  f"lessons {pn}, struck {p_struck}, pierced {p_pierced}")

	print("FLIGHT END - a burst broken on a shut door stays on the caster's side (C43)")
	sec = get("burst-door")
	t = tally(sec)
	mummy = monster_rows(sec, "mummy")
	doors = door_rows(sec)
	# The one bolt STOPPED IN the door's square, against it. An expiry and a
	# blast alone would not say so: a flight whose reach ran out short of the
	# door ends in the same open square in front, and its burst, going off there
	# with the door shut, spares the mummy under the old code as well - so the
	# claim below would pass having tested nothing.
	stop = stopped_in(t)
	on_door = (num(t, "expired") == 1 and num(t, "wallstops") == 1 and len(doors) == 2
			   and stop == doors[0][0])
	check(on_door, "the burst bolt stopped against the shut door, not short of it",
		  f"expired={t.get('expired')} wallstops={t.get('wallstops')} "
		  f"stoppedin={t.get('stoppedin')} door at {doors[0][0] if doors else None}")
	went_off = on_door and num(t, "blasts") >= 1
	check(went_off, "and went off", f"blasts={t.get('blasts')}")
	check(went_off and doors[1][1] < doors[0][1],
		  "the door's face took the blast", f"door hp before/after {[hp for _, hp in doors]}")
	# The claim itself: with the burst centred INSIDE the door (the phantom) the
	# square beyond took three arrivals at distance 1; centred in front, none.
	check(went_off and len(mummy) == 2 and mummy[1]["hp"] == mummy[0]["hp"]
		  and not mummy[1]["dead"] and not mummy[1]["effects"],
		  "and nothing of it reached the mummy beyond the door", f"mummy before/after {mummy}")
	ctl = get("burst-door-open")
	ct = tally(ctl)
	cm = monster_rows(ctl, "mummy")
	check(num(ct, "blasts") >= 1 and len(cm) == 2
		  and (cm[1]["dead"] or cm[1]["hp"] < cm[0]["hp"]),
		  "with the door open the same burst reaches the mummy (the control)",
		  f"blasts={ct.get('blasts')} mummy before/after {cm}")

	print("FLIGHT END - a bolt broken on the wall behind you still catches you (C44)")
	sec = get("bolt-behind")
	t = tally(sec)
	cs = chars(sec)
	brand = cs[0] if len(cs) == 1 and cs[0]["name"] == "Brand" else None
	# One expiry and no strike: a bolt that met Brand would have been spent on
	# him (hit or miss) and never expired. And it STOPPED IN the rock directly
	# behind him: a flight whose reach ran out inside his own square would
	# deliver its burn there under the old code as well.
	at = party_pos(sec)
	behind = ((at[0][0] - STEP[at[1]][0], at[0][1] - STEP[at[1]][1]) if at else None)
	stop = stopped_in(t)
	flew_past = (num(t, "expired") == 1 and num(t, "wallstops") == 1 and brand is not None
				 and behind is not None and stop == behind)
	check(flew_past, "the bolt flew past Brand down the empty lane and broke on the rock behind him",
		  f"expired={t.get('expired')} wallstops={t.get('wallstops')} "
		  f"stoppedin={t.get('stoppedin')} party at {at} char readouts {[c['name'] for c in cs]}")
	check(flew_past and "burn" in brand["effects"],
		  "its burn caught him in the last open square, his own",
		  f"effects {brand['effects'] if brand else None}")

	print("BURST ON THE PARTY - a magus's burst bolt that reaches the party goes off (C1)")
	sec = get("burst-party")
	t = tally(sec)
	ctl_loss, ctl_burn = losses(sec), burns(sec)
	# Went off on CONTACT: a blast with no expiry. A bolt that flew past and burst
	# on the wall behind counts a blast too, with an expiry; one struck as a plain
	# bolt (the old way) counts neither.
	burst = num(t, "blasts") == 1 and num(t, "expired") == 0
	check(burst, "the burst bolt that reached the party went off, on contact",
		  f"blasts={t.get('blasts')} expired={t.get('expired')}")
	check(burst and ctl_loss is not None and len(ctl_loss) == 4
		  and all(v > 0.0 for v in ctl_loss.values())
		  and len(ctl_burn) == 4 and all(v > 0.0 for v in ctl_burn.values()),
		  "on every member in the square, each left burning",
		  f"lost {ctl_loss} burn {ctl_burn}")

	print("BURST ON THE PARTY - the Wind Ward turns it, and a turned bolt does not land")
	sec = get("burst-ward")
	t = tally(sec)
	cs = chars(sec)
	turned = sum(99.0 - c["effects"]["windward"] for c in cs if "windward" in c["effects"])
	ward_turned = len(cs) == 4 and turned == 1.0 and num(t, "expired") == 0
	check(ward_turned, "the ward on the member it would strike turned it (one charge)",
		  f"charges spent {turned} over {len(cs)} readouts, expired={t.get('expired')}")
	wl = losses(sec)
	check(ward_turned and num(t, "blasts") == 0 and num(t, "taken") == 0
		  and wl is not None and not any(wl.values()) and not any(burns(sec).values()),
		  "and nothing went off: no blast, nobody hurt, nobody burning",
		  f"blasts={t.get('blasts')} taken={t.get('taken')} lost {wl} burn {burns(sec)}")

	print("THE GUST - a weakened burst bolt keeps the share of its blast and burn it keeps of itself (C18)")
	sec = get("burst-gust")
	t = tally(sec)
	gl, gb = losses(sec), burns(sec)
	slowed = repel(sec) == (1, 0) and num(t, "blasts") == 1 and num(t, "expired") == 0
	check(slowed, "a gust of half its strength slowed it, and it still went off on the party",
		  f"repel {repel(sec)} blasts={t.get('blasts')} expired={t.get('expired')}")
	# Half its strength left, so half its blast: what each member lost at the
	# detonation square (and two ticks of the burn after it) is half the control's.
	ratio = ({n: gl[n] / ctl_loss[n] for n in gl}
			 if slowed and gl and ctl_loss and set(gl) == set(ctl_loss)
			 and all(v > 0.0 for v in ctl_loss.values()) else None)
	check(ratio is not None and len(ratio) == 4 and all(0.4 <= r <= 0.6 for r in ratio.values()),
		  "its blast landed at half: each member lost half what the control's took",
		  f"lost {gl} against {ctl_loss}")
	check(slowed and len(gb) == 4 and len(ctl_burn) == 4 and all(v > 0.0 for v in ctl_burn.values())
		  and all(abs(gb[n] - 0.5 * ctl_burn.get(n, 0.0)) < 0.05 for n in gb),
		  "and its burn at half the magnitude", f"burn {gb} against {ctl_burn}")

	print("THE GUST - a gust its equal leaves it nothing, and it falls (C18)")
	sec = get("burst-spent")
	t = tally(sec)
	sl = losses(sec)
	fell = repel(sec) == (0, 1) and num(t, "expired") == 0 and num(t, "blasts") == 0
	check(fell, "spent, it fell without expiring and without going off",
		  f"repel {repel(sec)} expired={t.get('expired')} blasts={t.get('blasts')}")
	check(fell and num(t, "taken") == 0 and sl is not None and not any(sl.values()),
		  "and nothing reached the party", f"taken={t.get('taken')} lost {sl}")

	print("THE FLARE - it reaches what it can walk to, and nothing else (C17)")
	sec = get("flare-door")
	rows = monster_rows(sec, "mummy")
	at = lambda cell: [r for r in rows if r["cell"] == cell]
	near, beyond, far = at((11, 12)), at((15, 12)), at((18, 12))
	edge, past = at((9, 12)), at((8, 12))  # 3 and 4 open steps west
	torches = fires(sec)  # the torch at 10,12, then 12,10; before the flare, then after
	whole = (len(near) == len(beyond) == len(far) == len(edge) == len(past) == 2
			 and len(torches) == 4)
	hit = lambda r: ("dazzle" in r[1]["effects"] and "dazzle" not in r[0]["effects"]
					 and r[1]["hp"] < r[0]["hp"])
	untouched = lambda r: "dazzle" not in r[1]["effects"] and r[1]["hp"] == r[0]["hp"]
	# What the flare DID, which every claim after it rests on: a flare that did
	# nothing would dazzle nothing beyond the door, strike nothing far and light
	# nothing through the rock as well.
	struck = whole and hit(near)
	check(struck, "the flare dazzled and scorched the mummy behind the party",
		  f"mummy at 11,12 before/after {near}")
	# The far end of its reach, which the door claim rests on: the mummy beyond
	# the door is 3 steps off too, so a reach of 2 would keep it out with no door.
	reached = whole and hit(edge)
	check(reached, "it reached the mummy 3 open steps away and scorched it",
		  f"mummy at 9,12 before/after {edge}")
	check(reached and untouched(past), "and not the one 4 open steps away",
		  f"mummy at 8,12 before/after {past}")
	lit = whole and torches[0] is False and torches[2] is True
	check(lit, "and kindled the wall torch it reached, two steps off",
		  f"torches (10,12 then 12,10, before then after) {torches}")
	check(struck and reached and untouched(beyond),
		  "nothing beyond the shut door, 3 steps off, was dazzled or struck",
		  f"mummy at 15,12 before/after {beyond}")
	check(struck and "dazzle" in far[0]["effects"] and far[1]["hp"] == far[0]["hp"],
		  "a monster still dazzled from before, far off, was not scorched",
		  f"mummy at 18,12 before/after {far}")
	check(lit and torches[1] is False and torches[3] is False,
		  "and no wall torch was kindled through the rock", f"torches {torches}")

	print("WARDS BY HAND - each lands in its own school (C279)")
	rows = monster_rows(get("wards-ahead"), "skeleton")
	last = rows[-1] if rows else {"effects": {}, "schools": {}}
	# The school is the claim. Both wards are held with or without the fix (a
	# ward refreshes only its own kind, whatever the school), so "both held" is
	# no check of C279 - only the printed schools are.
	check(len(rows) == 1 and last["schools"].get("stoneskin") == "earth"
		  and last["schools"].get("fireshield") == "fire",
		  "`effect stoneskin ahead` lands as earth, `effect fireshield ahead` as fire",
		  f"{rows}")

	print("THE KILLING BLOW - an enchanted torch's burst never lands on the corpse (C5)")
	for name, how, landed_key in (("torch-kill", "swung", "hits"),
								  ("torch-throw", "thrown", "throwstrikes")):
		sec = get(name)
		t = tally(sec)
		rows = monster_rows(sec, "skel_warrior")
		cs = chars(sec)
		# Sera's torch, for the swing: the claim is about an ENCHANTED blow.
		armed = how == "thrown" or (len(cs) == 1 and cs[0]["hands"][1:2] == ["torch_lit"])
		killed = (armed and len(rows) == 2 and not rows[0]["dead"] and rows[1]["dead"]
				  and num(t, landed_key) >= 1)
		check(killed, f"a {how} lit torch killed the weakened skeleton warrior",
			  f"hands {[c['hands'] for c in cs]} warrior before/after {rows} "
			  f"{landed_key}={t.get(landed_key)}")
		check(killed and num(t, "slain") == 1, f"and `slain` moved by exactly one ({how})",
			  f"slain={t.get('slain')} - two is the burst wounding the corpse again")
		g = grudges(sec)
		check(killed and g == [], f"the corpse holds no grudge ({how})", f"grudges {g}")
		said = messages(sec)
		# The readout is read: the kill's own line is in it.
		check(killed and any(m == "The skeleton warrior is slain!" for m in said)
			  and not any(" turns on " in m for m in said),
			  f"and no line said it turned on anyone ({how})", f"messages {said}")

	print("STAT POINTS - a point names its stat (C36)")
	sec = get("stat-names")
	said = messages(sec)
	cs = chars(sec)
	gained = (lambda s: len(cs) == 2 and cs[1]["stats"][s] > cs[0]["stats"][s])
	int_line = [m for m in said if re.fullmatch(r"Maren's Intelligence rises to \d+\.", m)]
	wil_line = [m for m in said if re.fullmatch(r"Maren's Willpower rises to \d+\.", m)]
	ups = [m for m in said if re.fullmatch(r"Maren's (?!.* skill ).+ rises to \d+\.", m)]
	check(gained("int") and bool(int_line),
		  "an INT point earned casting reads \"Intelligence\"",
		  f"stats {[c['stats'] for c in cs]} stat lines {ups}")
	check(gained("wil") and bool(wil_line),
		  "a WIL point earned casting reads \"Willpower\"",
		  f"stats {[c['stats'] for c in cs]} stat lines {ups}")
	strays = sorted(set(re.findall(r"\bstat\.[a-z]\w*", text)))
	check(bool(int_line) and bool(wil_line) and not strays,
		  "and no `stat.` key reached the log anywhere in the run", f"keys {strays}")

	print("PUNCH - a punch, and a key's own swing, fight unarmed (C39)")
	sec = get("punch-key")
	cs = chars(sec)
	ts = tallies(sec)
	# Five `char` blocks: before, after the punches, after the key's swings, with
	# the dagger in hand 1, after the dagger hand's punches; three tallies.
	keyed = (len(cs) == 5 and all(c["hands"][:1] == ["iron_key"] for c in cs)
			 and len(ts) == 3)
	xp = [c["skills"].get("unarmed", 0.0) for c in cs] if keyed else []
	blade = [c["skills"].get("blade", 0.0) for c in cs] if keyed else []
	hits = [num(x, "hits") for x in ts] if keyed else []
	check(keyed and hits[0] >= 1 and abs((xp[1] - xp[0]) - hits[0]) < 0.05,
		  "every punch that landed with a key in the hand trained unarmed (xp gained = hits)",
		  f"hands {[c['hands'] for c in cs]} unarmed xp {xp} hits {hits}")
	check(keyed and hits[1] >= 1 and abs((xp[2] - xp[1]) - hits[1]) < 0.05,
		  "and so did every landed swing of the key itself, which has no damage of its own",
		  f"unarmed xp {xp} hits {hits}")
	check(keyed and cs[0]["parries"][:1] == ["unarmed"],
		  "the hand holding the key parries unarmed",
		  f"parries {[c['parries'] for c in cs]}")
	# The DAGGER hand is what tells "a punch is the bare hand's whatever it holds"
	# from "a key is no weapon": a key's hand is bare under either rule. Its hand
	# parries blade (so the dagger IS wielded there), yet its punches train
	# unarmed by exactly the hits and blade not at all.
	armed = keyed and cs[3]["hands"][1:2] == ["dagger"] and cs[3]["parries"][1:2] == ["blade"]
	check(armed and hits[2] >= 1 and abs((xp[4] - xp[3]) - hits[2]) < 0.05
		  and abs(blade[4] - blade[3]) < 0.005,
		  "a punch with the dagger hand trains unarmed (xp gained = hits), never blade",
		  f"hands {[c['hands'] for c in cs]} parries {[c['parries'] for c in cs]} "
		  f"unarmed xp {xp} blade xp {blade} hits {hits}")

	print("FUMBLE - a severe fumble drops the torch with what it had left (C10)")
	sec = get("fumble-drop")
	t = tally(sec)
	reads = held_torches(sec)
	sera = lambda r: [h for h in r if h[0] == 1 and h[1] == 1]
	# Sera's torch, burnt to 300 s before the swing; the swing a SEVERE fumble
	# (the loaded die) that put a held item down.
	before = sera(reads[0]) if len(reads) == 2 else []
	fumbled = (before == [(1, 1, "torch_lit", 300.0)] and num(t, "fumbles") == 1
			   and num(t, "severefumbles") == 1 and num(t, "fumbledrops") == 1)
	check(fumbled, "Sera's part-burnt torch, swung on the loaded die, fumbled severely",
		  f"torch readouts {reads} fumbles={t.get('fumbles')} "
		  f"severefumbles={t.get('severefumbles')} fumbledrops={t.get('fumbledrops')}")
	check(fumbled and sera(reads[1]) == [], "and left her hand", f"after {reads[1:]}")
	down = [r for r in floor_items(sec) if r[1] == "torch_lit"]
	# 300 s when it fell, less the second it has burned on the floor since. A
	# torch dropped with no charge reads as untouched and is relit full (~899).
	check(fumbled and len(down) == 1 and down[0][0] == (14, 14) and 297.0 <= down[0][2] <= 300.0,
		  "it lies in the party's square with the 300 s it had (less the second it burned there)",
		  f"floor {floor_items(sec)} - a torch relit full would read about 899")
	said = messages(sec)
	check(fumbled and "Sera loses their grip on the Lit torch!" in said,
		  "and the line names what fell", f"messages {said}")

	print("FLASK - a bomb that bursts on a monster trains the throw; one that meets nothing does not (C40)")
	sec = get("flask-contact")
	t = tally(sec)
	cs = chars(sec)
	thrown = lambda c: c["skills"].get("throwing", 0.0)
	struck = (len(cs) == 2 and cs[0]["name"] == "Brand" and num(t, "throws") == 1
			  and num(t, "throwstrikes") == 1 and num(t, "blasts") >= 1)
	check(struck, "Brand's fire flask burst on the skeleton",
		  f"throws={t.get('throws')} throwstrikes={t.get('throwstrikes')} blasts={t.get('blasts')} "
		  f"char readouts {[c['name'] for c in cs]}")
	gain = thrown(cs[1]) - thrown(cs[0]) if len(cs) == 2 else float("nan")
	check(struck and abs(gain - num(t, "throwstrikes")) < 0.005,
		  "and trained `throwing`, one xp for the contact",
		  f"throwing xp {[thrown(c) for c in cs]}, throwstrikes={t.get('throwstrikes')}")
	sec = get("flask-nothing")
	t = tally(sec)
	cs = chars(sec)
	missed = (len(cs) == 2 and num(t, "throws") == 1 and num(t, "throwstrikes") == 0
			  and num(t, "throwlandings") == 1 and num(t, "blasts") >= 1)
	check(missed, "a flask with nothing in its path shattered where its reach ran out",
		  f"throws={t.get('throws')} throwstrikes={t.get('throwstrikes')} "
		  f"throwlandings={t.get('throwlandings')} blasts={t.get('blasts')}")
	check(missed and abs(thrown(cs[1]) - thrown(cs[0])) < 0.005,
		  "and trained nothing: it touched no monster",
		  f"throwing xp {[thrown(c) for c in cs]}")

	print("SAVE - a save made with a flask in the air sets nothing off (C47)")
	sec = get("flask-save")
	ts = tallies(sec)
	air = flying(sec)
	pr = party_reads(sec)
	at = party_pos(sec)
	# Both saves were MADE in this run: a save that failed prints a refusal, and
	# the load after it would find an earlier run's file under the same name.
	made = "saved: combat_flask" in sec and "saved: combat_flask_far" in sec
	# The flask was IN THE AIR at both saves: one flight, read at each.
	aloft = (made and at is not None and len(air) == 2 and all(a[0] == "fire_flask" for a in air)
			 and len(ts) == 3 and len(pr) == 2)
	check(aloft, "the fire flask was in the air at both saves, each made in this run",
		  f"saved lines {[l for l in sec if l.startswith(('saved', 'not saved'))]} flying {air} "
		  f"tallies {len(ts)} party readouts {len(pr)} pos {at}")
	check(aloft and pr[0] == pr[1] and num(ts[0], "taken") == 0,
		  "and the save hurt nobody", f"hp before/after {pr} taken={ts[0].get('taken') if ts else None}")
	check(aloft and all(num(t, "throwlandings") == 0 and num(t, "blasts") == 0 for t in ts[:2]),
		  "nor landed it, nor set it off", f"after each save {ts[:2]}")
	# Its landing and burst came AFTER both saves: none counted at them, one since.
	check(aloft and num(ts[1], "throwlandings") == 0 and num(ts[2], "throwlandings") == 1
		  and num(ts[1], "blasts") == 0 and num(ts[2], "blasts") >= 1,
		  "the flight went on, and ended in a burst of its own",
		  f"at the later save {ts[1] if len(ts) > 1 else None} after {ts[2] if len(ts) > 2 else None}")
	# WHERE the flask was at each save, worked out here from its position - not
	# taken from the game's `over`, which is the same rule the save itself used.
	cell = lambda p: (math.floor(p[0]), math.floor(p[1]))
	near = cell(air[0][2]) if aloft else None
	far = cell(air[1][2]) if aloft else None
	party = at[0] if at else None
	step = STEP.get(at[1]) if at else None
	ahead = (aloft and step is not None and far != party
			 and any(far == (party[0] + k * step[0], party[1] + k * step[1]) for k in range(1, 5)))
	check(aloft and near == party and ahead,
		  "at the first save it was over the party's square, at the second a square or more ahead",
		  f"party {at} flask at {[a[2] for a in air]} -> squares {near} / {far}")
	for name, want in (("near", near), ("far", far)):
		loaded_sec = get(f"flask-save-{name}")
		got = [r for r in floor_items(loaded_sec) if r[1] == "fire_flask"]
		loaded = f"loaded: combat_flask{'_far' if name == 'far' else ''}" in loaded_sec
		check(aloft and loaded and len(got) == 1 and got[0][0] == want
			  and (name == "near" or got[0][0] != party),
			  f"the {name} save holds the flask, whole, on the floor of the square it was over",
			  f"loaded={loaded} floor {floor_items(loaded_sec)} it was over {want}, party {party}")

	print("MELEE - a swing meets the front rank, in the swinger's lane (C33)")
	for name, n, setup, labels in (
			("melee-lane", 2, "two bone swarms stand side by side in the front quarters of the square ahead",
			 ("Brand's punches met only the front swarm in his lane, not the one listed first",
			  "Sera's met only the front swarm in hers")),
			("melee-front", 4, "four bone swarms fill the square ahead",
			 ("Brand's punches met the front swarm in his lane, never the one behind it",
			  "Sera's met the front swarm in hers"))):
		sec = get(name)
		at = party_pos(sec)
		rows = monster_rows(sec, "skel_swarm")
		ts = tallies(sec)
		reads = [rows[i * n:(i + 1) * n] for i in range(3)] if len(rows) == 3 * n else []
		ahead = (at[0][0] + STEP[at[1]][0], at[0][1] + STEP[at[1]][1]) if at else None
		slots = [r["slot"] for r in reads[0]] if reads else []
		# Each member's quarter, and every claim below, is worked out from what the
		# square holds - so the setup must say the square holds what the section
		# means: all n in the square ahead, each its own quarter, and (side by side)
		# both in the front rank, Brand's not the one listed first.
		wants = [front_in_lane(at[1], m, slots) for m in (0, 1)] if at and reads else [None, None]
		placed = (bool(reads) and len(ts) == 2 and all(r["cell"] == ahead for r in reads[0])
				  and None not in slots and len(set(slots)) == n and wants[0] != slots[0]
				  and None not in wants and wants[0] != wants[1])
		check(placed, setup, f"party {at} square ahead {ahead} swarms {rows[:n]} tallies {len(ts)} "
							 f"quarters to meet {wants}")
		for phase, label in ((1, labels[0]), (2, labels[1])):
			before, after = (reads[phase - 1], reads[phase]) if placed else ([], [])
			lost = [b["slot"] for b, a in zip(before, after) if a["hp"] < b["hp"]]
			still = placed and [r["slot"] for r in after] == slots and not any(r["dead"] for r in after)
			check(still and num(ts[phase - 1], "hits") >= 1 and lost == [wants[phase - 1]],
				  f"{label} (quarter {wants[phase - 1]})",
				  f"quarters that lost hp {lost}, hits={ts[phase - 1].get('hits') if placed else None}, "
				  f"before {before} after {after}")

	print("CLIPS - a cosmetic clip moves no dice (C73)")
	plain_sec, extra_sec = get("clip-plain"), get("clip-extra")
	plain, extra = tallies(plain_sec), tallies(extra_sec)
	authored, extra_lists = clip_lists(plain_sec, "attack"), clip_lists(extra_sec, "attack")
	drew = (authored == [["attack"]] and extra_lists[:1] == [["attack", "walk"]]
			and len(plain) == 4 and len(extra) == 4
			and all(num(t, "clipdraws") == 0 for t in plain)
			and sum(num(t, "clipdraws") for t in extra) > 0)
	check(drew, "with a second attack clip the skeleton drew clip variations; as authored it drew none",
		  f"attack clips {authored} then {extra_lists} clipdraws {[t.get('clipdraws') for t in plain]} "
		  f"then {[t.get('clipdraws') for t in extra]}")
	strip = lambda t: {k: v for k, v in t.items() if k != "clipdraws"}
	differ = [i + 1 for i, (a, b) in enumerate(zip(plain, extra)) if strip(a) != strip(b)]
	# ...over real fights: in every sample the skeleton's blows landed.
	check(drew and not differ and all(num(t, "struck") >= 1 for t in plain),
		  "and every combat number of the sweep matched, sample for sample",
		  f"seeds that differ {differ}: " +
		  "; ".join(f"{strip(plain[i - 1])} vs {strip(extra[i - 1])}" for i in differ[:1]))

	print("NOTICED - a missed shot wakes a sleeper (C34)")
	sec = get("wake-miss")
	t = tally(sec)
	rows = monster_rows(sec, "skel_lurker")
	# The shot MISSED: one that landed woke it under the old rule as well, so a
	# hit would make the claim below pass having tested nothing.
	missed = (len(rows) == 2 and rows[0]["aware"] is False and num(t, "boltmisses") == 1
			  and num(t, "bolthits") == 0)
	check(missed, "Maren's firebolt missed the lurker lying dormant beyond its trigger",
		  f"lurker before/after {rows} bolthits={t.get('bolthits')} boltmisses={t.get('boltmisses')}")
	# The CONTROL: `freeze on` does not stop the brain latching `aware`, so what
	# says the MISS woke it is the twin with no shot sleeping through the same
	# window - else a think or a timer would pass for the miss.
	still = monster_rows(get("wake-still"), "skel_lurker")
	quiet = len(still) == 2 and all(r["aware"] is False for r in still)
	check(quiet, "the control: with no shot, the same lurker lay dormant through the same four seconds",
		  f"lurker before/after {still}")
	check(missed and quiet and rows[1]["aware"] is True and rows[1]["hp"] == rows[0]["hp"],
		  "and the miss woke it, unharmed", f"lurker before/after {rows}")

	print("NOTICED - an attack that reached the resting party ends the rest, landed or not (C34)")
	# Each rest ends at its FIRST attack, as `attacked`: `rest until` stops the
	# clock the moment it ends, so the TALLY counts only what came before.
	ended = lambda sec: rest_end(sec) == ("attacked", "attacked")
	sec = get("rest-miss")
	t = tally(sec)
	one_miss = num(t, "mswings") == 1 and num(t, "struck") == 0 and num(t, "taken") == 0
	check(one_miss and ended(sec),
		  "a skeleton's swing that MISSED ended the rest as `attacked` (one swing, none landed)",
		  f"mswings={t.get('mswings')} struck={t.get('struck')} taken={t.get('taken')} "
		  f"rest ended {rest_end(sec)} - under the old rule it slept on until a blow landed")
	sec = get("rest-veil")
	t = tally(sec)
	cs = chars(sec)
	drank = sum(999.0 - c["effects"]["waterveil"] for c in cs if "waterveil" in c["effects"])
	drunk = (num(t, "mswings") == 1 and num(t, "struck") == 1 and num(t, "taken") == 0
			 and len(cs) == 2 and drank > 0.0)
	check(drunk and ended(sec),
		  "a swing that LANDED but the water veil drank whole ended it too, with nobody hurt",
		  f"mswings={t.get('mswings')} struck={t.get('struck')} taken={t.get('taken')} "
		  f"veil drank {drank:.1f} over {len(cs)} readouts, rest ended {rest_end(sec)}")
	for name, what, extra in (("rest-ward", "a bolt the Wind Ward turned", ()),
							  ("rest-burst", "a BURST bolt the Wind Ward turned, so nothing went off,",
							   ("blasts",))):
		sec = get(name)
		t = tally(sec)
		turned = (num(t, "wardturns") == 1 and num(t, "taken") == 0
				  and all(num(t, k) == 0 for k in extra))
		check(turned and ended(sec), f"{what} ended it as `attacked`",
			  f"wardturns={t.get('wardturns')} taken={t.get('taken')} blasts={t.get('blasts')} "
			  f"rest ended {rest_end(sec)}")
	check("end" in s, "the script ran to its end")


def main():
	if not os.path.exists(EXE):
		print(f"no debug build at {EXE}")
		return 2
	selftest = "--selftest" in sys.argv
	script = SCRIPT
	if selftest:
		text = io.open(SCRIPT, encoding="utf-8").read()
		cut = "\n".join(("echo skipped" if l.startswith(CUT) else l) for l in text.splitlines())
		# Beside the script: its `sweep` names a rung relative to its own folder.
		fd, script = tempfile.mkstemp(suffix=".eval", dir=os.path.dirname(SCRIPT))
		with os.fdopen(fd, "w", encoding="utf-8") as fh:
			fh.write(cut + "\n")
	try:
		code, verdict, lines, text = run(script)
		print(f"eval: {verdict.split('] ', 1)[-1] if verdict else '(no verdict line)'}")
		judge(lines, text)
	finally:
		# It sits in the source tree, so it goes however the run ends.
		if selftest:
			os.remove(script)
	failed = sum(1 for _, ok in results if not ok)
	if selftest:
		# Every check that rests on a cut line must FAIL, and the setup-free ones
		# must still pass - so the cut run is a real run, not a broken one that
		# fails everything.
		wrong = [lbl for lbl, ok in results if ok != (lbl in SETUP_FREE)]
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed' if lbl not in SETUP_FREE else 'failed'} with its setup cut")
		ok = not wrong
		print(f"combattest RESULT={'PASS' if ok else 'FAIL'} checks={len(results)} failures={failed} self_test=1")
		return 0 if ok else 1
	print(f"combattest RESULT={'PASS' if failed == 0 else 'FAIL'} checks={len(results)} failures={failed} self_test=0")
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
