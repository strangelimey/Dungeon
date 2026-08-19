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

## What remains — the pipeline (do this next)

Making a bump message allocation-free end to end:

1. **Signatures to `std::string_view`** — the message path currently takes and
   stores `std::string`:
   - `DungeonWorld::onMessage`, `DungeonWorld::onMemberMessage`
   - `DungeonWorld::MemberMessage`
   - `GameUI::AddLogLine`
   - `fx::ITarget::Say` (and its two adapters, PartyTarget / MonsterTarget)
2. **`MessageLog` stops allocating per line.** Today it is
   `std::deque<Msg>` with a `std::string text` per entry. Make it a FIXED RING
   whose slots are allocated once and assigned into — `assign()` reuses capacity,
   so a steady stream of messages costs nothing after warm-up.
3. **~100 message call sites** switch `loc::Format` → `loc::FormatLine` and
   `loc::Tr` → `loc::View`. Mechanical; they are the sites matching
   `onMessage(loc::`, `MemberMessage(.*loc::`, `AddLogLine(loc::`.

### How to know it worked

Run the game, walk the party into a wall a few times, toggle the console, and
read `dungeon.log`. Success is **no `[warn]` lines at all** from a session where
nothing is wrong. Today that same run produces three, all `loc::Tr` /
`loc::Format` under `TryStep` and `OnBumpImpact`.

## The half after that (optional, larger)

Make borrowing the DEFAULT: `Tr` returns a view and the 365 sites that want
ownership say so (`std::string(loc::View(k))`). That is what stops the copy being
reintroduced by accident — until then, `Tr` remains an allocating trap that reads
like the obvious thing to call. Worth doing when no long-lived branch is open,
since it touches nearly every file that shows text.
