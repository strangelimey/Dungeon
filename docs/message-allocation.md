# Messages must not allocate — the plan

## Why this exists

The steady-state allocation guard (Core/AllocTrack, CLAUDE.md "Checking the
rule") kept reporting ordinary gameplay: walk into a wall, the bump message
prints, the frame allocates, the guard warns. `CLAUDE.md` explained this away as
the EVENT FRAME policy — "allocation proportional to events isn't what the rule
forbids" — and the warnings were left in place for a human to interpret.

That policy is wrong, and the cost of it is concrete: a warning that fires during
normal play trains you to ignore the guard. It did exactly that to me — asked
what those warnings were, I answered "safe to ignore". Michael's objection was
the right one: **if it is normal behaviour, it should not be a warning.**

## The actual cause

`loc::Tr` returned `std::string` BY VALUE — a copy of text the language table
already owns and goes on owning:

```cpp
return it != g_table.end() ? it->second : std::string(key);   // Loc.cpp
```

Every one of its 365 call sites allocated. It only surfaced as a gameplay warning
because during settled play the only thing calling it is a message.

So the guard was right and the code was wrong. There was never an "event frames
legitimately allocate" trade-off to make — the allocation was a copy nobody
needed.

## The target

**Building and printing a message allocates nothing.** Then the guard needs no
notion of an event, no threshold, no exception list: any allocation in a settled
frame is a bug, full stop. That is the property that makes it trustworthy.

It also disentangles a second question that had been folded into this one.
Something firing events every frame is a MESSAGE-RATE problem — visible in the
log, detectable on its own terms — not a memory problem. Once messages cost
nothing, the guard stops pretending to be two checks.

## What has landed (commit: the Loc foundation)

- `loc::View(key)` — the same lookup, returning `std::string_view` into the
  table's own storage. No copy. This is the root fix and the thing new code
  should reach for.
- `loc::Line` — a formatted line held INLINE (255 chars, no heap, no lifetime
  rules, cheap to pass). Longer text is clipped rather than grown: a log line
  past 255 characters is a bug in the line, and paying an allocation to print it
  is worse than clipping it.
- `loc::FormatLine` / `VFormatLine` — `View()` + `vformat` into a `Line`. The
  clipping lives in a small output iterator (`ClipIter`) because `vformat_to` is
  the only type-erased formatting sink the standard offers — `format_to_n` wants
  a compile-time format string, which a runtime language table cannot give it.
- `Tr` and `Format` are UNCHANGED, so all 365 + 138 existing sites still compile.

### Why the foundation is additive rather than a conversion

The first attempt changed `Format` itself to return `Line`. The compiler lit up
across ~40 files: inspectors returning `std::string`, dialog titles taking
`const std::string&`, `std::format("{}", ...)` with no formatter for the new
type. That is the whole-codebase migration (below) arriving uninvited in the
middle of a feature branch. Backed out; the non-allocating path is additive, and
the migration is its own job.

## The pipeline — DONE

1. **Signatures to `std::string_view`** — `DungeonWorld::onMessage` /
   `onMemberMessage` / `MemberMessage`, `GameUI::AddLogLine` (both overloads),
   `fx::ITarget::Say` and its two adapters. Nothing along the path owns a
   message any more; it is copied exactly once, into the log's own slot.
2. **`MessageLog` is a fixed ring.** Not the planned ring of `std::string`s
   reusing capacity — a ring of `loc::Line`, whose text is INLINE. That is a
   stronger property and a far easier one to state: it allocates nothing from
   the FIRST line, rather than nothing once every slot has grown big enough.
   The ring is one allocation at construction, and `Clear()` forgets its slots
   rather than releasing them.
3. **152 lookups at message call sites** moved to `loc::FormatLine` /
   `loc::View`, by a call-AWARE pass — a blind replace would have been wrong,
   since a `loc::Tr` feeding a widget label still wants to own its string.

### One thing the plan did not foresee: the KEY allocated too

11 sites built the key by concatenation — `loc::Tr("monster." + kind->name)`,
the convention dynamic ids follow (`monster.`/`item.`/`skill.`/`stat.`). That
temporary `std::string` is an allocation per message, and those are most of
what combat says out loud. `loc::ViewKey(prefix, id)` assembles the key in a
stack buffer instead.

It also closes a hazard the conversion would otherwise have INTRODUCED:
`View` returns a missing key AS ITSELF, so a view of a concatenated temporary
dangles the moment anyone holds on to it. `ViewKey` returns by value, so the
fallback is owned. (A `std::formatter<loc::Line>` comes with it —
`make_format_args` resolves on the static type, so Line's implicit conversion
to `string_view` never got a look in.)

### Measured

Identical session both times: new game, walk into the wall six times, toggle
the console, read `dungeon.log`.

| | guard reports | allocating sites |
|---|---|---|
| before | 3 | `loc::Tr`, `loc::VFormat`, `GameUI::AddLogLine`, `MessageLog::AddLine`, `MessageLog::Msg::Msg`, `AudioEngine::Play` |
| after | 1 | `AudioEngine::Play` |

Every message allocation is gone. The report left is not a message — below.

## Still open

**`AudioEngine::Play` allocates** (`AudioEngine.cpp:148`). The pool reuses an
idle voice of a MATCHING format, but the first play of each format
`make_unique`s a `PooledVoice`. So it is once-per-format WARM-UP rather than a
per-event cost — a different shape from the message defect, and a real choice
rather than an oversight: either that counts as legitimate warm-up (and the
harness warms the sound before it measures), or the pool is built up front at
load and the guard's promise holds with no asterisk at all. Deliberately not
decided here.

**`tools\AllocTest.ps1` cannot catch this class of defect** — and says so in
its own header: *"The party stands still on purpose. Player-driven EVENTS (a
bump message, a level line) legitimately allocate."* It encodes the very
rationalisation this document retired, which is why it passed throughout: it
passes on the UNFIXED build too. Once the audio question is settled, the
harness should bump a wall inside the measured window. That is the run that
would have caught this, and the one that stops it coming back.

## The half after that (optional, larger)

Make borrowing the DEFAULT: `Tr` returns a view and the 365 sites that want
ownership say so (`std::string(loc::View(k))`). That is what stops the copy being
reintroduced by accident — until then, `Tr` remains an allocating trap that reads
like the obvious thing to call. Worth doing when no long-lived branch is open,
since it touches nearly every file that shows text.
