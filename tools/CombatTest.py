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
#
# Each later combat batch adds its sections to combat.eval and its checks here.
#
# --selftest runs the same script with every line that SETS UP a claim cut (the
# effects, spawns, equips, wears, casts and bolts) and demands that EXACTLY the
# checks resting on one fail (SETUP_FREE names the rest, which must still pass),
# so no check is satisfied by nothing happening.
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
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\combat.eval")
NAMES = ["Brand", "Sera", "Maren", "Tilo"]

# What --selftest cuts: the lines that put a claim's cause in place.
CUT = ("effect ", "spawn ", "equip ", "wear ", "cast ", "bolt ")

# The checks that rest on none of the cut lines: with them cut, these, and ONLY
# these, may still pass.
SETUP_FREE = {
	"char prints the defense line every time it is asked",
	"unarmoured, no class, no soak",
	"the script ran to its end",
	"the game ran the script to its verdict",
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


def chars(sec):
	"""Every `char` block in the section, in order: a dict per block with the
	member's name, skills {id: xp}, creep {stat: pool}, effects {id: magnitude},
	hands [id, id] and the defense line's fields (or None if it printed none)."""
	out, cur = [], None
	for l in sec:
		m = re.match(r"  (\w+)  str \d+", l)
		if m:
			cur = {"name": m.group(1), "skills": {}, "creep": {}, "effects": {},
				   "hands": [], "defense": None}
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
		m = re.match(r"    hand\d (\S+)", l)
		if m:
			cur["hands"].append(m.group(1))
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


def monster_rows(sec, kind):
	"""Every `monsters` row for monsters of `kind` in the section, in order: a
	dict per row with its cell, hp, whether it is dead, its effects
	{id: magnitude} and each effect's school {id: school}."""
	out = []
	for l in sec:
		m = re.match(r"\s+(\S+) @ (\d+),(\d+)\s+hp ([\d.-]+)(.*)$", l)
		if m and m.group(1) == kind:
			out.append({"cell": (int(m.group(2)), int(m.group(3))), "hp": float(m.group(4)),
						"dead": "(dead)" in m.group(5),
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


def party_pos(sec):
	"""The party's square and facing from the section's `pos` line, or None."""
	for l in sec:
		m = re.match(r"(\d+),(\d+) facing (north|east|south|west)$", l)
		if m:
			return (int(m.group(1)), int(m.group(2))), m.group(3)
	return None


# A facing's step, +x east and +z south (Party's grid).
STEP = {"north": (0, -1), "east": (1, 0), "south": (0, 1), "west": (-1, 0)}


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


def judge(lines):
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
