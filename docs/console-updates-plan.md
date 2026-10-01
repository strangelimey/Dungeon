# Console updates - plan

From docs/console-updates-notes.md. Status: BUILT (2026-09-30). Deviations
from the draft below: an EMPTY FIRST params form now means the bare command
(`[status]` claimed bare and `status` were the same when they are not);
`help <word>` that is both a group and a command (`party`, `world`) prints
both; Enter on a half-typed name takes the highlighted match into the line,
and runs it at once when it takes no arguments; usage refusals were moved
onto the registered params only where the hand-written line was the whole
command's synopsis - sub-verb lines (`editor resize ...`) stay as written.

## The notes, organized

Two wants, one underlying problem ("so many commands I don't know what we have"):

1. THE LISTING (`help`)
   - grouped by function
   - shows each command's parameters
   - shows a one-line description
2. TYPE-AHEAD
   - as you type, show the commands that match

## What exists today

- 128 commands, registered from 8 files (DevConsole.cpp, DevConsole_Profile.cpp,
  Game_DevCommands / _DevWorld / _DevParty / _DevEval / _DevDiagnostics /
  _DevDungeons.cpp).
- `Register(name, help, fn)` - ONE free-text string per command. Parameters
  live inside it in at least four styles: `(dev): tp <x> <z>`, `(usage: ...)`,
  `(on/off)`, or not at all (`tp` says "teleport the party to a cell" and only
  the handler's own `usage: tp <x> <z>` line knows the arguments).
- `help` prints every command in registration order, `{:<10}` wide, so any
  name over 10 chars pushes its text out of line.
- No completion of any kind. Keys: typing, Backspace, Enter, Up/Down (history),
  PgUp/PgDn/wheel (scrollback), Esc.
- BUG FOUND ON THE WAY: `threat` is registered TWICE. Execute takes the first
  match, so Game_DevCommands' "monster kinds ranked by threat" runs and
  Game_DevParty's "per-member threat for every monster holding a grudge" has
  been unreachable. No script uses the second one.

## The plan

### 1. Structured registration (the foundation)

Replace the free-text `help` with four fields:

    m_console.Register({.name = "tp", .group = CmdGroup::Party,
                        .params = "<x> <z>",
                        .summary = "teleport the party to a cell"}, fn);

- `group` is an enum (`CmdGroup`), so a command cannot land in a misspelt
  group, and the enum order IS the listing order.
- `params` is the synopsis in the usual notation: `<required>`, `[optional]`,
  `a|b` choices, `...` repeats. Multi-verb commands (generate, worlds,
  worldloc, profile, dungeons, health, font...) get one FORM PER LINE (`params`
  holds `\n`-separated forms) instead of one long `a | b | c` run.
- `summary` is one line, no parameters in it.
- Register ASSERTS: non-empty summary, no newline in it, and NO DUPLICATE
  NAME. The last one is what would have caught `threat`.

### 2. Rewrite all 128 registrations

Mechanical but judged one at a time: split each existing string into params +
summary, strip the "(dev)" tags (the whole console is dev), and assign a group.
Draft grouping (edit freely):

| Group | Commands |
|---|---|
| Console | help, clear, echo, logecho, state, ver, quit, exit |
| Settings | quality, framecap, lang, fov, fonts, font |
| Rendering | shadows, shadowrate, dust, haze, ambient, lights, preview |
| Save and load | save, load, newgame |
| Party and movement | tp, pos, home, face, speed, noclip, forward |
| Characters and items | party, char, setstat, setskill, heal, regen, supplies, setsupply, consume, rest, give, rune, learn, wear, equip, guard, itemdetails, sheet, book |
| Combat and magic | swing, cast, blast, effect, smash, (member) threat |
| Monsters and AI | monsters, groups, spawn, freeze, lockstep, (kind) threat |
| Simulation and harness | timescale, step, seed, reset, arena, autoattack, autocast, tally |
| Levels and editor | editor, goto, mapinfo, geomhash, generate, validate, undo, redo, savemap, synctosource, levelrename, stairadd, press, buttons, levels, levelcheck, catround, mappage |
| Catalog types | newtype, typeset, typerefs |
| World map | worlds, world, worldmap, travel, quest, camp, encounter, encounters, enter, leave, worldpos, discover, worldedit, terrainbrush, paint, worldprops, worldloc, worldarea, worldsettings, saveworld, dungeons |
| Diagnostics | alloctest, allocpoke, allocguard, pipeline, pipelineguard, pipelinepoke, crashpoke, health, loadstats, uitree, uioverlap |
| Threads | threadspawn, threadwedge, throttle, governor, threadprio, threadaffinity, threadreap |
| Profiling | profile, fps |

The handlers' own `usage: tp <x> <z>` refusals switch to a
`console.Usage("tp")` helper that prints the REGISTERED params, so the listing
and the error can never disagree.

### 3. The listing

    help                 every group, headed, commands alphabetical within it
    help <group>         one group
    help <command>       that command: summary + every form of its params
    help <word>          anything else: commands whose name or summary contains it

Layout, three columns sized from the widest name / a params cap:

    -- Party and movement --------------------------------------------
      face     <n|e|s|w>           turn the party to face a direction
      tp       <x> <z>             teleport the party to a cell
      generate [dungeon|again] [knob:value ...]
                                   rough out a new level
               dialog [new|off|tab <n>]
               ...

A params run too long for its column wraps the summary to the next line
rather than truncating anything. Group headers draw in the accent colour.

### 4. Type-ahead

While the FIRST word is being typed, a suggestion box sits just above the
input line listing the matches, each with its params and summary:

- prefix matches first (alphabetical), then substring matches dimmed
  (typing `guard` also offers allocguard, pipelineguard)
- capped at ~10 rows with a "+N more" line
- Up/Down MOVE THE SELECTION while the box is open; Tab completes to the
  highlighted match. On an empty line (no box) Up/Down recall history as today
  (Michael, 2026-09-30). One consequence to hold: a line RECALLED from history
  must not open the box, or the second Up would move the selection instead of
  stepping further back. The box opens on a typed character and stays shut
  after a recall until the next keystroke edits the line.
- once a space is typed after a known command, the box collapses to ONE hint
  line: that command's params (all its forms), so you can see what to type next
- Esc with the box showing closes the box first, a second Esc closes the
  console (the popup-first rule GameUI already follows)

No allocation concern: the guard is disarmed while the console is open.

### 5. Checks

- Register's asserts (duplicates, empty/multi-line summary) fire at startup,
  so every build checks every registration.
- `help` output is read by nothing in tools/ (checked), so the format is free
  to change.
- Screenshot the listing and the type-ahead box; run `uioverlap` is NOT
  relevant (the console is outside the widget tree).
- `/check` quick tier for regressions (the eval harness drives the console
  through RunLine, which this does not touch).

## Open questions

1. ANSWERED: the per-member `threat` becomes `grudges`; the kind ranking keeps
   `threat` (levelthreat.eval uses it).
2. ANSWERED: Up/Down move the selection while the box is open.
3. The grouping table - any moves or renames?
