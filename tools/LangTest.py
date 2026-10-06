# tools/LangTest.py - every language key the code names is in every .lang file.
#
# Run:  python tools\LangTest.py [--selftest]   (no build needed: it reads src\
#       and assets\lang, and runs nothing)
#
# A key missing from a language does not fail anywhere at run time: Loc hands
# the KEY back as the text, so the screen shows `map.select.wall` and nothing
# else notices (code-review C107). LogMissingKeys warns only for keys en.lang
# has and a translation lacks - a key the code names and en.lang never got is
# invisible to it, and so is a key no screen the warning's reader opened. This
# reads the code instead, and demands of EVERY .lang file in assets\lang:
#
#   KEYS     every literal key the source names is in the file. A literal key
#            is (a) any key-shaped string literal handed straight to loc::Tr /
#            View / Format / FormatLine / VFormat / VFormatLine - a ternary of
#            two literals counts both - and (b) any other dotted key-shaped
#            literal whose first word is a family en.lang already uses (`map.`,
#            `log.`, ...), which is how a key reaches Loc through a table or a
#            helper (MapEditor's kCategoryInfo, a `counted` lambda); a file
#            name is not one there ("items.cat": a last word that is a file
#            extension). Comments are not code. A key BUILT at run time
#            (ViewKey("monster.", id), "skill." + id) is not literal and is not
#            checked here.
#   HOLES    every translation of a key has en.lang's count of {} holes. One
#            short drops a value without a word; one long makes std::vformat
#            throw, and Loc then shows the raw pattern.
#   ONCE     no key is defined twice in one file: the later line silently
#            wins, so the first one is dead text that reads as live.
#   ENOUGH   the run read a real tree: at least MIN_SOURCE_FILES source files
#            and MIN_KEYS literal keys. os.walk of a missing or misrouted src\
#            yields nothing and raises nothing, so without this a run that read
#            no code would find no missing key and PASS on zero work.
#
# --selftest hands the check a fault of every kind and passes (exit 0) only
# when it reports exactly those: a key the code names dropped from one
# translation, a loc::Tr of a key no file has, and a table literal of a key in
# a known family that no file has; a translation with a hole too few; a key
# that translation defines twice (planted in the file's TEXT and read back
# through parse_lang, so the reading that notices is the plain run's); and a
# run over no source at all, which must be refused rather than passed. A check
# that cannot see a missing key is the defect this exists to stop.
#
# Exit 0 = every file holds every key (or, under --selftest, every fault was
# caught); 2 = the run read too little to judge. The verdict line is
# `langtest RESULT=...`.
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
LANG = os.path.join(ROOT, r"assets\lang")
REFERENCE = "en"

# The loc:: calls whose FIRST argument is a key.
LOC_CALL = re.compile(r"\bloc::(Tr|View|Format|FormatLine|VFormat|VFormatLine)\s*\(")
# A key: lower-case words joined by dots. `title` is the one key with no dot,
# which is why a dotless literal counts only when it IS a call's whole argument.
KEY = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)*$")
DOTTED = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$")
HOLE = re.compile(r"\{[^{}]*\}")
# A last word that names a file, not a key ("items.cat", "dungeon.log").
FILE_EXT = {"cat", "lang", "ini", "map", "ent", "gltf", "glb", "png", "dds", "log", "eval",
            "hlsl", "txt", "dmp", "py", "ps1", "cpp", "h", "md", "fbx", "obj", "json", "tsv",
            "wav", "ogg", "exe", "dll", "cso", "bin", "sav"}
# The least a real tree gives (2026-10: 440 source files, 1100 literal keys) -
# far enough below today's to leave room to grow and shrink, far enough above
# zero that a scan which read the wrong folder cannot pass.
MIN_SOURCE_FILES = 100
MIN_KEYS = 500


# --- the .lang files --------------------------------------------------------

def parse_lang(text):
    """{key: text} the way Core/Loc's ParseFile reads it, plus the keys a file
    defines more than once."""
    table, dupes = {}, []
    for line in text.splitlines():
        s = line.strip(" \t\r")
        if not s or s.startswith(";") or "=" not in s:
            continue
        key, value = s.split("=", 1)
        key = key.strip(" \t")
        if not key:
            continue
        if key in table:
            dupes.append(key)
        table[key] = value.strip(" \t")
    return table, dupes


def lang_text(code):
    with io.open(os.path.join(LANG, code + ".lang"), encoding="utf-8-sig") as f:
        return f.read()


def load_langs():
    langs = {}
    for name in sorted(os.listdir(LANG)):
        if name.endswith(".lang"):
            langs[name[:-5]] = parse_lang(lang_text(name[:-5]))
    return langs


def holes(text):
    return len(HOLE.findall(text.replace("{{", "").replace("}}", "")))


# --- the source ---------------------------------------------------------------

def number_before(text, i):
    """Whether the ' at `i` sits inside a number (1'000'000, 0xFF'FF) - the
    token it follows starts with a digit - rather than opening a character
    literal, which follows a space, a bracket or a prefix (L'x', u8'x')."""
    j = i
    while j > 0 and (text[j - 1].isalnum() or text[j - 1] in "_'"):
        j -= 1
    return j < i and text[j].isdigit()


def lex(text):
    """The text with comments blanked and every literal's insides blanked (so
    brackets and commas can be counted without a string's own), and the string
    literals as (start offset in the text, value). Newlines are kept, so an
    offset still names its line."""
    code = list(text)
    lits = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                code[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if code[k] != "\n":
                    code[k] = " "
            i = j
        elif c == 'R' and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            # A raw string: R"delim( ... )delim".
            open_paren = text.find("(", i + 2)
            delim = text[i + 2:open_paren]
            close = text.find(")" + delim + '"', open_paren)
            close = n if close < 0 else close
            lits.append((i + 1, text[open_paren + 1:close]))
            for k in range(open_paren + 1, close):
                if code[k] != "\n":
                    code[k] = "x"
            i = close + len(delim) + 2
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            lits.append((i, text[i + 1:j]))
            for k in range(i + 1, min(j, n)):
                code[k] = "x"
            i = j + 1
        elif c == "'" and number_before(text, i):
            i += 1  # a digit separator (1'000'000), not a character literal
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                code[k] = "x"
            i = j + 1
        else:
            i += 1
    return "".join(code), lits


def first_argument(code, start):
    """[start, end) of a call's first argument, `start` just past its '('."""
    depth = 0
    i = start
    while i < len(code):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return start, i
            depth -= 1
        elif c == "," and depth == 0:
            return start, i
        i += 1
    return start, i


def used_keys(files, families):
    """{key: [file:line, ...]} - every literal key the files name. `files` is
    [(relative path, text)]."""
    used = {}

    def note(key, rel, text, at):
        used.setdefault(key, []).append(f"{rel}:{text.count(chr(10), 0, at) + 1}")

    for rel, text in files:
        code, lits = lex(text)
        in_call = set()
        for m in LOC_CALL.finditer(code):
            a, b = first_argument(code, m.end())
            whole = code[a:b].strip()
            for s, v in lits:
                if not a <= s < b:
                    continue
                in_call.add(s)
                # A dotted literal is a key wherever it sits in the argument (a
                # ternary's arms); a dotless one only when it IS the argument
                # (`title`), not a word handed to a helper inside it. A prefix
                # ("monster." + id) ends in a dot and matches neither.
                lone = whole == '"' + "x" * len(v) + '"'
                if DOTTED.match(v) or (lone and KEY.match(v)):
                    note(v, rel, text, s)
        for s, v in lits:
            if s in in_call or not DOTTED.match(v):
                continue
            if v.split(".", 1)[0] in families and v.rsplit(".", 1)[-1] not in FILE_EXT:
                note(v, rel, text, s)
    return used


def source_files():
    out = []
    for dp, _, names in os.walk(SRC):
        for name in sorted(names):
            if name.endswith((".cpp", ".h")):
                p = os.path.join(dp, name)
                with io.open(p, encoding="utf-8", errors="replace") as f:
                    out.append((os.path.relpath(p, ROOT), f.read()))
    return out


# --- the check ----------------------------------------------------------------

def judge(langs, files):
    """Every finding, as (kind, text)."""
    findings = []
    ref = langs[REFERENCE][0]
    families = {k.split(".", 1)[0] for k in ref if "." in k}
    used = used_keys(files, families)
    for key in sorted(used):
        lacking = [code for code, (table, _) in langs.items() if key not in table]
        if lacking:
            where = ", ".join(used[key][:2]) + (f" (+{len(used[key]) - 2})" if len(used[key]) > 2 else "")
            findings.append(("KEYS", f"'{key}' ({where}) is missing from {', '.join(c + '.lang' for c in lacking)}"))
    for code, (table, dupes) in langs.items():
        for key in sorted(set(dupes)):
            findings.append(("ONCE", f"{code}.lang defines '{key}' more than once"))
        if code == REFERENCE:
            continue
        for key, text in sorted(table.items()):
            if key in ref and holes(text) != holes(ref[key]):
                findings.append(("HOLES", f"{code}.lang '{key}' has {holes(text)} {{}} hole(s), "
                                          f"{REFERENCE}.lang {holes(ref[key])}"))
    return findings, used


def starved(files, used):
    """Why a run read too little to be a verdict (ENOUGH), or None."""
    if len(files) < MIN_SOURCE_FILES:
        return (f"read {len(files)} source file(s) under {SRC}, fewer than {MIN_SOURCE_FILES} - "
                "the scan did not read the tree, so no missing key could be found")
    if len(used) < MIN_KEYS:
        return (f"found {len(used)} literal key(s) in {len(files)} source file(s), fewer than "
                f"{MIN_KEYS} - the scan read the files but saw almost no Loc call")
    return None


def report(findings):
    for kind, text in findings:
        print(f"  [FAIL] {kind}: {text}")


def self_test(langs, files):
    """The faults the check must catch, and only those."""
    ref = langs[REFERENCE][0]
    victim = next(c for c in langs if c != REFERENCE)
    families = {k.split(".", 1)[0] for k in ref if "." in k}
    used = used_keys(files, families)
    # A key the code really names, dropped from one translation.
    dropped = next(k for k in sorted(used) if "." in k and all(k in t for t, _ in langs.values()))
    # A hole short in another key of the same translation.
    holed = next(k for k in sorted(ref) if holes(ref[k]) >= 1 and k != dropped and k in langs[victim][0])
    # A third key that translation defines twice - planted in the file's TEXT
    # and read back through parse_lang, so the reading that has to notice is the
    # very one the plain run uses (the same words twice: no HOLES finding rides
    # along).
    victim_table = langs[victim][0]
    twice = next(k for k in sorted(victim_table) if k not in (dropped, holed))
    table, dupes = parse_lang(lang_text(victim) + f"\n{twice}={victim_table[twice]}\n")
    del table[dropped]
    table[holed] = HOLE.sub("", table[holed], count=1)
    broken = {c: (dict(t), list(d)) for c, (t, d) in langs.items()}
    broken[victim] = (table, dupes)
    # A loc call and a table literal, each of a key no file has.
    planted = ("src\\Game\\LangTestSelfTest.cpp",
               'void F() {\n\tauto a = loc::Tr("langtest.absent_call");\n'
               '\tstatic const char* kRows[] = {"map.langtest_absent_row"};\n'
               '\t// "map.langtest_comment" is in a comment, and is no key\n}\n')
    findings, _ = judge(broken, files + [planted])
    want = {
        ("KEYS", dropped): f"'{dropped}' dropped from {victim}.lang",
        ("KEYS", "langtest.absent_call"): "a loc::Tr of a key no file has",
        ("KEYS", "map.langtest_absent_row"): "a table literal of a key no file has",
        ("HOLES", holed): f"{victim}.lang '{holed}' a hole short",
        ("ONCE", twice): f"{victim}.lang '{twice}' defined twice",
    }
    caught = set()
    stray = []
    for kind, text in findings:
        # The key is the first quoted word of every finding's text.
        m = re.search(r"'([^']+)'", text)
        key = (kind, m.group(1) if m else "")
        if key in want:
            caught.add(key)
        else:
            stray.append((kind, text))
    for key, what in want.items():
        print(f"  {'[ok  ]' if key in caught else '[FAIL]'} caught: {what}")
    for kind, text in stray:
        print(f"  [FAIL] reported what was not planted: {kind}: {text}")
    # ENOUGH: a run over no source finds no missing key - and must be refused.
    empty = starved([], judge(langs, [])[1])
    print(f"  {'[ok  ]' if empty else '[FAIL]'} refused: a run over no source file "
          f"({empty or 'it would have passed on zero work'})")
    return len(caught) == len(want) and not stray and empty is not None, len(findings)


def main():
    langs = load_langs()
    if REFERENCE not in langs:
        print(f"no {REFERENCE}.lang in {LANG}")
        return 2
    files = source_files()
    selftest = "--selftest" in sys.argv
    findings, used = judge(langs, files)
    # ENOUGH, before either verdict: a run that read no tree is no verdict -
    # and the self-test needs a real tree to plant its faults in.
    why = starved(files, used)
    if why:
        print(f"  [FAIL] ENOUGH: {why}")
        print(f"langtest RESULT=FAIL files={len(langs)} sources={len(files)} keys={len(used)} "
              f"starved=1 self_test={int(selftest)}")
        return 2
    if selftest:
        ok, n = self_test(langs, files)
        # The check FAILED on the faults it was handed, as it must: report that
        # verdict, and exit 0 when it was exactly the faults planted.
        print(f"langtest RESULT={'FAIL' if ok else 'PASS'} files={len(langs)} findings={n} self_test=1")
        return 0 if ok else 1
    report(findings)
    print(f"  {len(used)} literal key(s) in {len(files)} source file(s), "
          f"against {len(langs)} language file(s): {', '.join(langs)}")
    print(f"langtest RESULT={'PASS' if not findings else 'FAIL'} files={len(langs)} "
          f"sources={len(files)} keys={len(used)} findings={len(findings)} self_test=0")
    return 0 if not findings else 1


if __name__ == "__main__":
    sys.exit(main())
