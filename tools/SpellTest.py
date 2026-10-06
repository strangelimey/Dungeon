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
#           bolt, each leaving the VOLLEY's own on-hit (burn 1 4), not its
#           form's (code-review C19); Hagalaz bursts, reaching the square beside
#           the target; Ingwaz on a ward wards all four; Hagalaz on a ward kills
#           what stands round the caster, harms no one in the party, and leaves
#           no ward.
#   MONSTERS the adept's spell hurts the party, the ladder is authored, and the
#           magus's burst bolt goes off on the party it reaches (code-review C1).
#   REPEL   a strong gust turns arrows back, and they kill the archer.
#   LIGHT   a Firelight running out says its own line, naming the light, and
#           never the Sight spell's (code-review C9) - read off the HUD's
#           message log (`messages`) in the game's own language.
#   WORDS   a description is shown WHOLE in every language (code-review C371):
#           German Sowilo's in the details dialog, the Russian tablets' and the
#           amulet's, and the Russian lights' Known Spells rows - each past the
#           255 bytes a loc::Line holds, each held to the byte count of the
#           .lang file this judge reads itself, never to the game's own.
#
# --selftest runs the same script with every `cast` line removed - and every
# `lang` line made `lang en`, which the WORDS checks rest on - and demands that
# EXACTLY the checks resting on either fail (SPELL_FREE names the rest, which
# must still pass), so no check is satisfied by nothing happening.
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
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\spells.eval")
MONSTERS = os.path.join(ROOT, r"assets\projects\dungeon-demo\catalog\monsters.cat")
NAMES = ["Brand", "Sera", "Maren", "Tilo"]

results = []

# The checks that rest on no spell and no language: with every cast cut and
# every `lang` made English (--selftest) these, and ONLY these, may still pass.
SPELL_FREE = {
	"the details dialog offers Memorize for a rune its holder does not know",
	"and pressing it learns the rune and spends the tablet",
	"a rune its holder already knows offers no Memorize, and is not spent",
	"the dialog opened on no one's item offers no Memorize",
	# the book's own Cast is not a `cast` line, so it still casts with them cut
	"with the mana back it casts, and the slate empties",
	"a torch doused before a save is out again after the load",
	"a new game finds the torch doused in the last one lit",
	"the adept's volleys hurt the party",
	"mage, adept and magus cast bolt, volley and burst",
	"the magus's burst bolt goes off on the party it reaches",
	"a strong gust turns an arrow back",
	"and the turned arrows kill the archer",
	"the script ran to its end",
	"the game ran the script to its verdict",
}


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
	# if it were whole (the code used to be captured and never read).
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


def monster_effects(sec):
	"""Every monster line, in order: (type, {effect id: magnitude})."""
	out = []
	for l in sec:
		m = re.match(r"  (\w+) @ \d+,\d+  hp [\d.]+(.*)$", l)
		if m:
			out.append((m.group(1), {k: float(v) for k, v in
									 re.findall(r"\[(\w+) ([\d.-]+) ", m.group(2))}))
	return out


def said(sec):
	"""The HUD message log lines a `messages` readout printed in the section."""
	return [l[4:] for l in sec if l.startswith("  | ")]


def lang():
	"""The game's own strings, in the language it runs in (settings.ini beside
	the exe, en when unset): a world line is localized, so the judge formats the
	expected line from the same table rather than assuming English."""
	code = "en"
	ini = os.path.join(os.path.dirname(EXE), "settings.ini")
	if os.path.isfile(ini):
		for l in io.open(ini, encoding="utf-8", errors="replace"):
			if l.startswith("language="):
				code = l.split("=", 1)[1].strip() or "en"
	path = os.path.join(ROOT, "assets", "lang", f"{code}.lang")
	if not os.path.isfile(path):
		path = os.path.join(ROOT, "assets", "lang", "en.lang")
	table = {}
	for l in io.open(path, encoding="utf-8-sig"):
		if "=" in l and not l.lstrip().startswith(";"):
			k, v = l.rstrip("\n").split("=", 1)
			table[k.strip()] = v
	return table


def lang_bytes(code, key):
	"""(UTF-8 byte length, has a {} hole) of `key`'s text in
	assets\\lang\\<code>.lang, read the way Core/Loc's parser reads it (key and
	text trimmed of spaces and tabs) - or None when the file lacks it. Each WORDS
	check holds the game to this."""
	path = os.path.join(ROOT, "assets", "lang", f"{code}.lang")
	found = None
	for l in io.open(path, encoding="utf-8-sig"):
		s = l.rstrip("\r\n").strip(" \t")
		if not s or s.startswith(";") or "=" not in s:
			continue
		k, v = s.split("=", 1)
		if k.strip(" \t") == key:
			found = v.strip(" \t\r")  # the LAST definition wins, as in Loc
	return None if found is None else (len(found.encode("utf-8")), "{" in found)


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

	print("MEMORIZE - offered only for a rune its holder does not know")
	sec = get("memorize-details")
	shown = [l.endswith("memorize=1") for l in sec if l.startswith("item details: open")]
	slot0 = []  # Maren's pack slot 0, at each `inventory status`
	for l in sec:
		m = re.search(r"\| 2: (\S+)", l)
		if l.startswith("inventory:") and m:
			slot0.append(m.group(1))
	pressed = [l for l in sec if l.startswith("item details: memorized")
			   or l.startswith("item details: no Memorize")]
	check(len(shown) == 3 and shown[0],
		  "the details dialog offers Memorize for a rune its holder does not know", f"{shown}")
	check(pressed[:1] == ["item details: memorized"] and slot0[:1] == ["-"],
		  "and pressing it learns the rune and spends the tablet", f"{pressed} {slot0}")
	check(len(shown) == 3 and not shown[1] and pressed[1:2] == ["item details: no Memorize button up"]
		  and slot0[1:2] == ["rune_multiple"],
		  "a rune its holder already knows offers no Memorize, and is not spent",
		  f"{shown} {pressed} {slot0}")
	check(len(shown) == 3 and not shown[2],
		  "the dialog opened on no one's item offers no Memorize", f"{shown}")

	print("GRAMMAR - school, form, then one modifier")
	casts = [l for l in get("grammar") if l.startswith(("cast away", "no cast"))]
	check(casts[:4] == ["no cast (fizzle / no mana / unknown)"] * 4,
		  "no modifier on tier 1, none out of order, one at most, none on Sight", f"{casts}")
	check(len(casts) == 5 and casts[4] == "cast away", "the well-formed recipe beside them casts",
		  f"{casts}")

	print("THE BOOK - a Cast refused for want of mana keeps the spell")
	sec = get("book-nomana")
	slates = [int(m.group(1)) for l in sec if (m := re.match(r"book slate: (\d+)", l))]
	mp = next((float(m.group(1)) for l in sec
			   if (m := re.match(r"  \[2\] Maren\s+hp \S+\s+st \S+\s+mp ([\d.]+)/", l))), None)
	check(mp is not None and mp < 8.0 and slates[:2] == [2, 2],
		  "out of mana, the book's Cast is refused and the spell stays built",
		  f"mp={mp} slates={slates}")
	check(slates[2:3] == [0], "with the mana back it casts, and the slate empties", f"{slates}")

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
	# Its bolts leave the volley's OWN on-hit, spells.cat [firebolt_volley] burn
	# 1 4 - not firebolt's burn 2 4, which every volley bolt carried while the
	# entry was read by nothing (code-review C19).
	burned = [fx["burn"] for kind, fx in monster_effects(get("volley"))
			  if kind == "skel_warrior" and "burn" in fx]
	check(num(t, "bolthits") >= 1 and burned == [1.0],
		  "a volley bolt leaves the volley's own burn (1 a second), not its form's (2)",
		  f"bolthits={t.get('bolthits')} warrior burns {burned}")
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
	# Every monster bolt that ENDS without reaching anyone is an expiry, and a burst
	# one bursts there; one that reaches the party bursts too, with no expiry. So
	# more blasts than expiries is a burst that went off on the party - which the
	# magus's never did, its bolt striking one member as a plain firebolt (C1).
	t = tally(get("magus"))
	check(num(t, "blasts") > num(t, "expired") and num(t, "taken") > 0,
		  "the magus's burst bolt goes off on the party it reaches",
		  f"blasts={t.get('blasts')} expired={t.get('expired')} taken={t.get('taken')}")

	print("REPEL - a strong gust against arrows")
	sec = get("repel")
	turned = sum(1 for l in sec if re.match(r"castsvc repel: weakened=\d+ turned=[1-9]", l))
	check(turned >= 1, "a strong gust turns an arrow back", f"turned {turned} times")
	check(num(tally(sec), "slain") >= 1, "and the turned arrows kill the archer",
		  f"slain={tally(sec).get('slain')}")

	print("LIGHT - a light running out says its own line (code-review C9)")
	sec = get("light-fades")
	words = lang()
	lines = said(sec)
	own = words.get("log.light_fades", "?").format("Maren", words.get("spell.firelight", "?"))
	sight = {words.get("log.sight_fades", "?").format(n) for n in NAMES}
	check(cast(sec) and own in lines, "a Firelight running out says its own line, naming the light",
		  f"want {own!r} among {lines}")
	check(cast(sec) and own in lines and not sight.intersection(lines),
		  "and never the Sight spell's", f"{lines}")

	print("WORDS - a description is shown whole, in every language (code-review C371)")
	sec = get("descriptions")
	# Each `itemdetails status` while open: (shown bytes, entry bytes), in the
	# script's order. Each expectation is the FILE's count, and past 255 - the
	# bytes a loc::Line holds - or the check could pass on a cut nobody needed.
	shown = [int(m.group(1)) for l in sec
			 if l.startswith("item details: open") and (m := re.search(r" desc=(\d+)/\d+ ", l))]
	want = [("de", "item.rune_light.desc"), ("ru", "item.rune_light.desc"),
			("ru", "item.rune_explode.desc"), ("ru", "item.rune_multiple.desc"),
			("ru", "item.moonstone_amulet.desc")]
	expect = [(lang_bytes(code, key) or (0, False))[0] for code, key in want]
	premise = all(n > 255 for n in expect)
	check(premise and shown[:1] == expect[:1],
		  "German Sowilo's description is shown whole, past 255 bytes",
		  f"shown {shown[:1]}, de.lang {expect[:1]}")
	check(premise and shown[1:] == expect[1:],
		  "the Russian tablets' and the amulet's are shown whole",
		  f"shown {shown[1:]}, ru.lang {expect[1:]}")
	# The Known Spells rows (`sheet spells`): every row whose Russian text has no
	# {} hole (a hole's length depends on the power formatted in) against the
	# file - the lights' three long ones among them.
	rows = {m.group(1): int(m.group(2)) for l in sec if (m := re.match(r"  (\w+) desc=(\d+)$", l))}
	summary = next((l for l in sec if l.startswith("sheet spells: ")), "")
	cuts = re.search(r"cuts=(\d+)", summary)
	wrong, compared = [], []
	for sid, n in sorted(rows.items()):
		entry = lang_bytes("ru", f"spell.{sid}.desc")
		if entry is None or entry[1]:
			continue
		compared.append(sid)
		if n != entry[0]:
			wrong.append(f"{sid} {n}/{entry[0]}")
	long_lights = [sid for sid in ("firelight", "skylight", "stonelight")
				   if (lang_bytes("ru", f"spell.{sid}.desc") or (0, False))[0] > 255]
	check(len(long_lights) == 3 and set(long_lights) <= set(compared) and not wrong
		  and cuts is not None and cuts.group(1) == "0",
		  "the Russian Known Spells rows are whole, the lights' long ones too",
		  f"rows {len(rows)}, compared {len(compared)}, wrong {wrong}, {summary!r}")
	check("end" in s, "the script ran to its end")


def main():
	if not os.path.exists(EXE):
		print(f"no debug build at {EXE}")
		return 2
	selftest = "--selftest" in sys.argv
	script = SCRIPT
	if selftest:
		# Every cast cut, and every language switch made English: the WORDS
		# checks then see English descriptions, which the files' German and
		# Russian counts must tell apart.
		text = io.open(SCRIPT, encoding="utf-8").read()
		cut = "\n".join(("echo skipped" if l.startswith("cast ") else
						 "lang en" if l.startswith("lang ") else l) for l in text.splitlines())
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
		# Every check that rests on a cast (or a language) must FAIL with them
		# cut, and the spell-free ones must still pass - so the cut run is a real
		# run, not a broken one that fails everything.
		wrong = [lbl for lbl, ok in results if ok != (lbl in SPELL_FREE)]
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed' if lbl not in SPELL_FREE else 'failed'} with no casts")
		ok = not wrong
		print(f"spelltest RESULT={'PASS' if ok else 'FAIL'} checks={len(results)} failures={failed} self_test=1")
		return 0 if ok else 1
	print(f"spelltest RESULT={'PASS' if failed == 0 else 'FAIL'} checks={len(results)} failures={failed} self_test=0")
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
