# code-review - notes

Michael's brain dump for the `code-review` branch (2026-10-05), captured as it
came in, then organized. The review itself is in docs/code-review-findings.md.

## Notes

- We've done a lot over the past few months. Now he wants a FULL code review
  of what we've done.
- Low-level things to look at:
  - unnecessary globals
  - memory allocation is guarded
  - repetitive or redundant code
- The big picture too: look at the entire structure and see if it all still
  makes sense as it has grown.
  - Example of the kind of thing to look for: there are lots of dialogs in the
    game. Make sure the chrome, footer buttons etc. are built in a GENERIC way,
    with the specific dialog only producing its main content.
  - Check the editor dialogs and the inspectors share ONE base class too.
- VERY IMPORTANT - one place for hit calculations:
  - The current hit calculations fold in skills, rolls, stats, etc. They also
    take care of resistances and things like "water puts out fire". There will
    be many more such things to add.
  - So there must be ONLY ONE place this is done: the "effect" pipeline.
  - Weapon hits, spell hits, etc. all need to feed through that same pipeline.
  - Each individual spell implementation class should simply be a list of
    calls to "create a projectile using the projectile system" and "create an
    effect using the effect system".
- AI gets a similar sub-classed approach: each behaviour (warrior, thief,
  etc.) has its own implementation of a base class.
- Graphics pipeline: check for redundancy, repetition or stale code - the
  whole path, from loading the resources to pumping them through the HLSL
  systems.
- "That's it for now. Be very thorough."

## Organized

Two kinds of request are mixed in the dump, and they want different outputs:
AUDITS (find what is wrong with the code as it stands) and TARGET
ARCHITECTURES (a stated shape the code should have; the review measures the
gap and the fix is a refactor). Both are reviewed now; nothing is changed until
the findings and a plan are agreed.

### A. Low-level hygiene - the whole codebase (src/ + the C++ tools)

1. GLOBALS - namespace-scope or static mutable state, singletons, function-local
   statics holding state. Each one either has a reason (the crash handler and
   the allocation tracker must work with no owner; constant tables are not
   state) or should become a member of whoever owns it.
2. ALLOCATION IS GUARDED - read three ways, all covered:
   a. the steady-state rule: no heap allocation on a per-frame path, and the
      guard (AllocTrack + SteadyStateFrame) actually sees every such path - which
      states and threads are NOT armed, which AllocTest modes leave paths
      unexercised, and whether every self-excuse is justified;
   b. ownership: raw new/delete, missing RAII, C-API handles;
   c. growth that is never bounded (pools that grow, caches without eviction).
3. REPETITIVE / REDUNDANT CODE - duplicated logic, near-copies, dead functions,
   stale comments describing removed behaviour, obsolete compatibility shims.

### B. Structure - does it still make sense as it has grown

1. The module layering (Core -> ... -> Game -> Main) and what lives in which
   library: Game is ~83% of the code (97k of 117k lines), and DungeonWorld.h
   alone is 4761 lines.
2. God objects hiding behind the split-by-category pattern (DungeonWorld, Game,
   GameUI): a class spread over 20 .cpp files is still one class.
3. DIALOGS: chrome, title, close box, tabs, "?" help and footer buttons built
   generically; each dialog produces only its main content.
4. ONE BASE CLASS for the editor dialogs AND the inspectors (and how Game routes
   input/update/render to them).

### C. Gameplay architecture - one pipeline, behaviours as classes

1. THE HIT PIPELINE (VERY IMPORTANT). Skills, rolls, stats, resistances and
   element interactions ("water puts out fire") are computed in ONE place, the
   effect pipeline. Weapon hits, spell hits, throws, monster attacks, blasts,
   DoTs, collisions all feed through it. A spell class is only a list of "create
   a projectile" / "create an effect" calls. The review maps where each of those
   rules lives today and every path that computes any of them outside fx.
2. AI BEHAVIOURS AS CLASSES. Each behaviour (warrior, thief, ...) implements a
   base class, the way spells and effects do. The review maps how behaviour
   varies today (data flags, switches, archetype checks) and what a base class
   must respect (the async director, IWorldView, IQ buckets, the
   think/act split).

### D. Graphics pipeline - end to end

Loading (Assets, AssetUtil, the world's loaders, thumbnails, previews) -> GPU
(device, renderer, PSOs, root signature, upload, passes) -> HLSL (constant
layouts mirrored by hand, shared functions, unused registers) -> the Game-side
passes (lights, shadows, particles, icon bakes). Redundancy, repetition, stale
code.

### Gaps and assumptions (to confirm after the findings, one at a time)

- "Memory allocation is guarded" is read all three ways above.
- "Water puts out fire" today acts on FIXTURES and torches as well as on
  combatants. Assumed: the one pipeline must cover the world's things (fires,
  doors, breakables), not only monsters and the party.
- The AI target names behaviours (warrior, thief) that are not all archetypes
  yet. Assumed: the review proposes the base class and maps today's archetypes
  onto it; new behaviours are content for later.
- Python build scripts (tools/Build*.py etc.) are assets, not engine code: out
  of scope unless a finding touches how the engine consumes them.
- Balance numbers are out of scope (the harness reports, it does not judge).

## Answers (2026-10-05, after the review)

The review (docs/code-review-findings.md, 483 issues) closed with eleven
questions. Michael's answers, asked one at a time:

1. **Bugs first or refactor first?** All of phase 0 first: every live bug
   and rule-break that needs no refactor, with the broken harnesses fixed
   before anything else, so later phases are judged by checks that can fail.
2. **Do worker threads fall under the steady-state allocation rule?** Yes -
   judge workers too, after their warm-up. A worker tick that allocates fails
   the check like a main-thread frame.
3. **HLSL mirrors.** Replace "by hand": C++ constants passed to the compiler
   as #defines, a shared common.hlsli through an include handler, and a
   D3DReflect check of every cbuffer layout at startup.
4. **Element interactions and world things.** Relations in damagetypes.cat
   (water douses burn and lit things, fire ignites flammable things, air fans
   fires), an OnElement effect hook, fixtures / torches / floor items as fx
   targets, and a zero-damage Touch for Splash and Flame. One rule for every
   source and target.
5. **Split the Game library?** All six targets now (GameRules, GameWorld,
   GameHud, GameEditor, GameApp, DevTools) - not GameRules first.
6. **What is one monster behaviour class?** One class per ENGAGEMENT style
   (Melee, Ranged, Thief...), perception as data, flee / leash / patrol as
   shared modifiers; swarm, lurker and sentry become presets of Melee.
7. **The player's M map and the allocation guard.** Guard it: it is ordinary
   play. The editor stays exempt.
8. **Can a blow kill a downed member?** Blasts reach the downed (bursts,
   bombs, lingering gas); melee and aimed bolts still do not single out the
   fallen.
9. **GPU object lifetime.** A deferred-release queue (GraphicsDevice::Retire,
   freed once the fence passes) replaces "drain before freeing", after the
   shared frame arena and texture cache reduce the owners.
10. **One truth for the active level.** Back monsters and decorations with
    records, like doors and items; then merge the live and *Remote editing
    APIs into one that takes a level as its target.
11. **Dialog footers.** Action buttons right-aligned across the whole panel
    (under both columns when there is a preview), the help "?" at the far
    left. The dialog base enforces it.

The organize step's assumptions stand: "allocation is guarded" covers the
steady-state rule, ownership and growth; the one pipeline covers world things
(answer 4); balance numbers stay out of scope.
