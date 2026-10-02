# tools/SpellTest.py - the spell tiers' checks (docs/spell-updates-plan.md Phase 8).
#
# Run:  python tools\SpellTest.py [--selftest]   (needs a debug build)
#
# Runs tools\EvalScripts\spells.eval headless and JUDGES what it printed. The
# eval runner's own verdict only says every line named a command; a hand spell
# that did nothing reads PASS there. Each section of the script ends in a
# readout (`castsvc fire`, `torch`, `castsvc floor`, `monsters`, `tally`,
# `char`), and every check below is a claim about one section's readouts:
#
#   TIER 1  Kenaz lights the held torch, the wall torch ahead, and the brazier
#           ahead only past its power; Ansuz flares a fire and shoves a monster
#           only past its power; Laguz fills a waterskin a step at a time, puts
#           out the wall torch, the brazier only past its power, and smokes the
#           square it put out; Berkano lands in an empty hand, else at the feet.
#   STATE   a douse survives a save and load; a new game finds a fire doused in
#           the last one lit again (it once did not - the level's stashed map
#           kept the flag).
#   GRAMMAR school, form, then one modifier: four malformed recipes refused,
#           a well-formed one cast.
#   TIER 2+ Fire Bolt strikes once and bursts nothing; Ingwaz flies more than one
#           bolt; Hagalaz bursts, reaching the square beside the target; Ingwaz
#           on a ward wards all four; Hagalaz on a ward kills what stands round
#           the caster, harms no one in the party, and leaves no ward.
#   MONSTERS the adept's spell hurts the party, and the ladder is authored.
#   REPEL   a strong gust turns arrows back, and they kill the archer.
#
# --selftest runs the same script with every `cast` line removed and demands
# that EXACTLY the checks resting on a spell fail (SPELL_FREE names the rest,
# which must still pass), so no check is satisfied by nothing happening.
import io
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\spells.eval")
MONSTERS = os.path.join(ROOT, r"assets\projects\dungeon-demo\catalog\monsters.cat")
NAMES = ["Brand", "Sera", "Maren", "Tilo"]

results = []

# The checks that rest on no spell: with every cast cut (--selftest) these, and
# ONLY these, may still pass.
SPELL_FREE = {
	"a torch doused before a save is out again after the load",
	"a new game finds the torch doused in the last one lit",
	"the adept's volleys hurt the party",
	"mage, adept and magus cast bolt, volley and burst",
	"a strong gust turns an arrow back",
	"and the turned arrows kill the archer",
	"the script ran to its end",
}


def check(ok, label, detail=""):
	results.append((label, ok))
	print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
	if not ok and detail:
		print(f"         {detail}")


def run(script):
	args = [EXE, "-headless", "-eval", script]
	try:
		code = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=600).returncode
	except subprocess.TimeoutExpired:
		code = -1
	log = io.open(LOG, encoding="utf-8", errors="replace").read().splitlines()
	for line in log:
		if "FATAL" in line:
			print(f"         game FATAL: {line.split('FATAL', 1)[1][:160]}")
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


def fires(sec):
	out = []
	for l in sec:
		m = re.match(r"castsvc fire: kind=(\w+) lit=(\d) canburn=\d haze=([\d.]+) flare=([\d.]+)", l)
		if m:
			out.append({"kind": m.group(1), "lit": m.group(2) == "1",
						"haze": float(m.group(3)), "flare": float(m.group(4))})
	return out


def hands(sec):
	"""Every `torch` readout in the section, as a list of {(member, hand): id}."""
	out, cur = [], None
	for l in sec:
		m = re.match(r"  \[(\d)\] \w+ hand (\d): (\S+) charge", l)
		if m:
			if cur is None:
				cur = {}
				out.append(cur)
			cur[(int(m.group(1)), int(m.group(2)))] = m.group(3)
		elif l.startswith("  wall torch ahead:"):
			cur = None
	return out


def monsters(sec):
	"""Every monster line, in order: (type, x, z, hp)."""
	out = []
	for l in sec:
		m = re.match(r"  (\w+) @ (\d+),(\d+)  hp ([\d.]+)", l)
		if m:
			out.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))))
	return out


def tally(sec):
	for l in sec:
		if l.startswith("TALLY "):
			return {k: v for k, v in re.findall(r"(\w+)=(\S+)", l)}
	return {}


def num(t, key):
	try:
		return float(t.get(key, "nan"))
	except ValueError:
		return float("nan")


def effects(sec):
	"""`char` blocks: member name -> [effect id, ...]."""
	out, cur = {}, None
	for l in sec:
		m = re.match(r"  (\w+)  str \d+", l)
		if m:
			cur = m.group(1)
			out[cur] = []
			continue
		m = re.match(r"    effect (\w+) ", l)
		if m and cur:
			out[cur].append(m.group(1))
	return out


def party(sec):
	out = {}
	for l in sec:
		m = re.match(r"  \[\d\] (\w+)\s+hp ([\d.]+)/([\d.]+)", l)
		if m:
			out[m.group(1)] = (float(m.group(2)), float(m.group(3)))
	return out


def cast(sec):
	"""The section's spell went off. A NEGATIVE claim ("leaves it cold",
	"harms no one") holds just as well when nothing was cast, so each one is
	tied to this."""
	return "cast away" in sec


def first(seq, default=None):
	return seq[0] if seq else default


def last(seq, default=None):
	return seq[-1] if seq else default


def judge(lines):
	s = sections(lines)
	get = lambda name: s.get(name, [])
	lit = lambda f: f is not None and f["lit"]
	out = lambda f: f is not None and not f["lit"]

	print("TIER 1 - the hand spells")
	h = last(hands(get("flame-held")), {})
	check(h.get((2, 1)) == "torch_lit", "Kenaz lights the torch in the other hand",
		  f"Maren's right hand holds {h.get((2, 1))}")
	f = fires(get("flame-walltorch"))
	check(out(first(f)) and lit(last(f)), "Kenaz lights the wall torch ahead", f"{f}")
	check(cast(get("flame-brazier-weak")) and out(last(fires(get("flame-brazier-weak")))),
		  "a weak Kenaz leaves the brazier cold", f"{fires(get('flame-brazier-weak'))}")
	check(lit(last(fires(get("flame-brazier-strong")))),
		  "a strong Kenaz lights the brazier", f"{fires(get('flame-brazier-strong'))}")
	f = fires(get("gust-flare"))
	check(len(f) == 2 and f[0]["flare"] == 0.0 and f[1]["flare"] > 0.0,
		  "Ansuz flares the fire ahead", f"{f}")
	weak = last(fires(get("splash-brazier-weak")))
	check(cast(get("splash-brazier-weak")) and lit(weak),
		  "a weak Laguz leaves the brazier burning", f"{weak}")
	f = fires(get("splash-brazier-strong"))
	strong = last(f)
	check(len(f) == 2 and lit(f[0]) and out(f[1]), "a strong Laguz puts the brazier out", f"{f}")
	check(weak is not None and strong is not None and strong["haze"] > weak["haze"],
		  "the doused brazier smokes its square", f"haze {weak and weak['haze']} -> {strong and strong['haze']}")
	f = fires(get("splash-walltorch"))
	check(lit(first(f)) and out(last(f)), "Laguz puts the wall torch out", f"{f}")
	check(len(f) == 2 and f[1]["haze"] > f[0]["haze"], "the doused torch smokes its square", f"{f}")
	steps = [r.get((2, 1)) for r in hands(get("splash-fill"))]
	check(steps == ["waterskin_half", "waterskin", "waterskin"],
		  "Laguz fills a waterskin a step at a time, and stops when full", f"{steps}")
	h = last(hands(get("pebble-hand")), {})
	check(h.get((0, 0)) == "pebble", "Berkano lands in the casting hand when it is empty",
		  f"Brand's left hand holds {h.get((0, 0))}")
	floor = [l for l in get("pebble-feet") if l.startswith("castsvc floor:")]
	check(bool(floor) and "pebble" in floor[-1].split(":", 1)[1].split(),
		  "with both hands full, the pebble lands at the feet", f"{floor}")
	m = monsters(get("gust-shove"))
	check(len(m) == 2 and m[0][1:3] == (14, 11) and m[1][1:3] == (14, 10),
		  "a weak Ansuz moves nothing, a strong one shoves the monster back a square", f"{m}")

	print("STATE - fires outlive a save, not a new game")
	f = fires(get("douse-saveload"))
	check(len(f) == 2 and lit(f[0]) and out(f[1]),
		  "a torch doused before a save is out again after the load", f"{f}")
	f = fires(get("newgame-relights"))
	check(len(f) == 2 and out(f[0]) and lit(f[1]),
		  "a new game finds the torch doused in the last one lit", f"{f}")

	print("GRAMMAR - school, form, then one modifier")
	casts = [l for l in get("grammar") if l.startswith(("cast away", "no cast"))]
	check(casts[:4] == ["no cast (fizzle / no mana / unknown)"] * 4,
		  "no modifier on tier 1, none out of order, one at most, none on Sight", f"{casts}")
	check(len(casts) == 5 and casts[4] == "cast away", "the well-formed recipe beside them casts",
		  f"{casts}")

	print("TIER 2 and 3 - bolts, volleys, bursts, wards")
	t = tally(get("firebolt"))
	bolts = num(t, "bolthits") + num(t, "boltmisses") + num(t, "expired")
	check(bolts == 1 and num(t, "blasts") == 0, "Fire Bolt flies one bolt and bursts nothing",
		  f"bolts={bolts} blasts={t.get('blasts')}")
	m = monsters(get("firebolt"))
	beside = [x for x in m if x[1:3] == (13, 11)]
	check(cast(get("firebolt")) and beside and beside[0][3] == 88.0,
		  "the monster beside the target is untouched", f"{m}")
	t = tally(get("volley"))
	bolts = num(t, "bolthits") + num(t, "boltmisses") + num(t, "expired")
	check(bolts >= 2 and num(t, "blasts") == 0, "Ingwaz on a bolt flies a volley",
		  f"bolts={bolts} blasts={t.get('blasts')}")
	t = tally(get("burst"))
	m = monsters(get("burst"))
	check(num(t, "blasts") == 1, "Hagalaz on a bolt bursts", f"blasts={t.get('blasts')}")
	check(len(m) == 2 and all(x[3] < 88.0 for x in m),
		  "the burst reaches the square beside the target", f"{m}")
	e = effects(get("ward-self"))
	check("stoneskin" in e.get("Maren", []) and "stoneskin" not in e.get("Brand", []),
		  "a plain ward settles on the caster alone", f"{e}")
	e = effects(get("ward-party"))
	check(all("stoneskin" in e.get(n, []) for n in NAMES), "Ingwaz on a ward wards all four", f"{e}")
	sec = get("ward-burst")
	t = tally(sec)
	p = party(sec)
	check(num(t, "slain") == 4, "Hagalaz on a ward kills the four standing round the caster",
		  f"slain={t.get('slain')}")
	check(cast(sec) and num(t, "taken") == 0 and len(p) == 4
		  and all(hp == mx for hp, mx in p.values()),
		  "and harms no one in the party", f"taken={t.get('taken')} {p}")
	check(cast(sec) and "fireshield" not in effects(sec).get("Maren", ["?"]),
		  "and leaves no ward behind (a burst instead)", f"{effects(sec)}")

	print("MONSTERS - the mage ladder")
	t = tally(get("adept"))
	check(num(t, "taken") > 0, "the adept's volleys hurt the party", f"taken={t.get('taken')}")
	cat = io.open(MONSTERS, encoding="utf-8").read()
	spell_of = {}
	for block in re.split(r"\n(?=\[)", cat):
		m = re.match(r"\[(\w+)\]", block)
		sm = re.search(r"^spell\s*=\s*(\S+)", block, re.M)
		if m and sm:
			spell_of[m.group(1)] = sm.group(1)
	ladder = (spell_of.get("skel_mage"), spell_of.get("skel_mage_adept"), spell_of.get("skel_magus"))
	check(ladder == ("firebolt", "firebolt_volley", "firebolt_burst"),
		  "mage, adept and magus cast bolt, volley and burst", f"{ladder}")

	print("REPEL - a strong gust against arrows")
	sec = get("repel")
	turned = sum(1 for l in sec if re.match(r"castsvc repel: weakened=\d+ turned=[1-9]", l))
	check(turned >= 1, "a strong gust turns an arrow back", f"turned {turned} times")
	check(num(tally(sec), "slain") >= 1, "and the turned arrows kill the archer",
		  f"slain={tally(sec).get('slain')}")
	check("end" in s, "the script ran to its end")


def main():
	if not os.path.exists(EXE):
		print(f"no debug build at {EXE}")
		return 2
	selftest = "--selftest" in sys.argv
	script = SCRIPT
	if selftest:
		text = io.open(SCRIPT, encoding="utf-8").read()
		cut = "\n".join(("echo skipped" if l.startswith("cast ") else l) for l in text.splitlines())
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
		# Every check that rests on a cast must FAIL with the casts cut, and the
		# spell-free ones must still pass - so the cut run is a real run, not a
		# broken one that fails everything.
		wrong = [lbl for lbl, ok in results if ok != (lbl in SPELL_FREE)]
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed' if lbl not in SPELL_FREE else 'failed'} with no casts")
		ok = not wrong
		print(f"spelltest RESULT={'PASS' if ok else 'FAIL'} checks={len(results)} failures={failed} self_test=1")
		return 0 if ok else 1
	print(f"spelltest RESULT={'PASS' if failed == 0 else 'FAIL'} checks={len(results)} failures={failed} self_test=0")
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
