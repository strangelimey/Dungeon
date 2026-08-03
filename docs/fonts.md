# Fonts — shipping real typefaces, and per-role font selection

Branch `fonts` (own worktree). Goal: the game ships its own typefaces instead of
falling back to Consolas, and a widget can ask for a **role** — body, display,
script, mono — instead of every surface drawing in the one face its UIContext
happens to own.

## Background: how text is drawn today

`ui::Font` (src/UI/Font.cpp) wraps stb_truetype. It rasterizes each codepoint
into a growing alpha atlas on first use, pre-warms Latin-1 (32..255) at bake
time, re-bakes every cached glyph when `SetHeight` changes the size, and uploads
via `Commit()` — which calls `WaitIdle()`, so it must run between frames, never
mid-record. All of that already works and none of it needs changing.

What does not exist is any font *selection*. Every construction site passes an
empty path:

```
m_hudUi(device, "", kHudFontH), m_menuUi(device, "", kMenuFontH), ...
```

`Font`'s constructor tries the path, fails, and falls through to
`C:\Windows\Fonts\consola.ttf`. So **the entire game — HUD, menus, the DUNGEON
title, the character sheet, every editor dialog — renders in Consolas**, a
monospace developer font, at 26 separate call sites. `docs/font_test_menu.png`
looks like a typeface trial but is not: it was the Unicode-range work (Cyrillic
renders, CJK shows tofu), still all Consolas.

Two structural facts matter for the plan:

**One font per context.** `UIContext` owns a single `Font m_font`; every draw
site reaches it through `ctx.GetFont()` (~40 call sites across Controls.cpp,
CharacterSheet_*.cpp, MessageLog.cpp, SpellbookPanel.cpp, TreeInspector.cpp).
There is no way for one widget in a context to draw in a different face.

**Each `Font` owns a full copy of the TTF.** `m_ttf` is kept resident so
`SetHeight` can re-bake. With 26 `Font` objects that is 26 copies of the font
file plus 26 atlases and 26 SRV slots. Sharing the face bytes is not just tidy —
it is what makes shipping several families affordable.

The 26 owners: 8 in `GameUI` (hud / menu / settings / pause / saves / sheet /
confirm contexts + `m_titleFont`), `DevConsole`, `MapView`, and 8 editor dialogs
that each hold **both** an `m_font` and an `m_ui` UIContext with its own font
(AssetPicker, BalanceDialog, InspectPicker, InstanceInspector,
LevelSettingsDialog, MonsterConfigDialog, ProjectileInspector, TypeEditorDialog).

### The seam this was designed for

`UI/Units.h` already reserved the extension point:

> `em` — relative to the widget's OWN font — collapses onto rem while every
> widget draws with its context's font. Em() exists so the seam has a name: if
> per-widget font sizes ever arrive, Em is the one that changes and every call
> site already says which it meant.

That is the invariant this work must honour, and it is load-bearing:

**`Rem` stays the CONTEXT root font size. Only `Em` follows the widget.**

Padding, row heights, scrollbar widths and thumb minimums are authored in rem
against the context's root. If `Rem` started tracking a widget's own font, a
label switching to a script face would silently re-space every control around
it. Role selection inherits down the tree like CSS `font-family`; the document's
root size does not move.

## Design

### Roles, not filenames

```cpp
enum class FontRole { Body, Display, Script, Mono };
```

- **Body** — the default. HUD, message log, settings, character sheet, all
  dialogs. Must survive 17px unhinted.
- **Display** — the DUNGEON title, menu headings, sheet section headers.
- **Script** — scrolls, spell descriptions, item flavour. In-world text.
- **Mono** — dev console, editor numeric fields. Alignment matters; dev-facing.

Content and layout name a role. Which *file* a role resolves to is data.

### FontLibrary

New `UI/FontLibrary.h/.cpp`, owned once at app level:

- Owns face blobs as `shared_ptr<const std::vector<u8>>` keyed by path, so N
  `Font`s on one family share one copy of the bytes. `Font` gains a constructor
  taking a shared blob; the path-taking one stays for the fallback path.
- Vends `Font*` keyed by **(face, rounded pixel height)** — see the thrash trap
  below.
- Owns `CommitAll()` and the resize re-bake, as one loop over live fonts.
- Resolves role → face through `assets/fonts/fonts.cat`.

### fonts.cat

Block format (`serialize::Block`, the existing catalog primitive), living in the
shared pool beside `lang/` — a font is engine chrome, not per-project content:

```
[body]
file  = Alegreya/static/Alegreya-Regular.ttf
scale = 1.00

[display]
file  = Cinzel/static/Cinzel-Regular.ttf
scale = 1.10
```

`scale` is not optional decoration. Faces differ substantially in x-height and
cap-height at the same em size — Cinzel set at 28px reads much smaller than
Consolas at 28px. Without a per-role optical multiplier, swapping a face breaks
every layout that was tuned against the old one. The multiplier folds into the
`SetHeight` call, so rem/em and all authored geometry stay put.

### Resolution in the tree

- `UIContext` holds a `FontLibrary&` plus its root role and root size.
  `GetFont()` keeps its exact signature and returns the root role's font — so
  all ~40 existing draw sites compile unchanged.
- `Widget` gains an inherited `std::optional<FontRole> fontRole`, resolved
  parent-ward: my role if set, else my parent's, else the context root.
- `Em(ctx)` resolves against the widget's font; `Rem(ctx)` is untouched.

This keeps the phases independent: nothing changes visually until a widget
actually sets a role.

## Phases

### Phase 0 — Acquire and record — **DONE (2026-08-02)**

11 faces across 10 families installed under `assets/fonts/<Family>/`, each with
its `OFL.txt`, 2.4 MB total, **committed** (unlike textures and models these are
small and are *source*, so a fresh clone and a new worktree get them for free —
nothing added to the gitignored-asset provisioning dance). Full provenance in
`assets/fonts/README.md`; ledger rows in `docs/costs.md`.

| Role | Candidates installed |
|---|---|
| Display | Cinzel (Roman inscriptional), Marcellus (softer), Grenze Gotisch (blackletter-ish) |
| Body | Alegreya (literary serif), Spectral (screen-designed), Gentium Book Plus (huge accent coverage), Bitter (slab, robust small) |
| Script | IM Fell English roman + italic (period, inky), Petit Formal Script (cleaner) |
| Mono | JetBrains Mono (Consolas stays as the missing-file fallback) |

Three findings that changed the plan's assumptions:

**The variable-font trap is not hypothetical, and it is worse than "you only get
Regular".** `fonts.google.com/download` now serves an SPA shell rather than a
zip, so faces came from the google/fonts repo — where five of the ten families
publish *only* a `[wght]` variable file. Reading their `fvar` defaults:

```
cinzel         wght  min 400  default 400  max 900
grenzegotisch  wght  min 100  default 400  max 900
alegreya       wght  min 400  default 400  max 900
bitter         wght  min 100  default 100  max 900   <-- default is THIN
jetbrainsmono  wght  min 100  default 400  max 800
```

**Bitter's default instance is Thin.** Shipping the variable file would have
rendered the sturdiest body candidate as a spindly hairline, and the audition
would have thrown it out for a fault that isn't its own. All five were instanced
to `wght=400` with fontTools, which also drops the dead variation tables
(Alegreya 425 KB -> 268 KB). The lesson generalises: *never trust a variable
font's default to be Regular — read `fvar` before auditioning.*

**Latin-1 coverage is a non-issue.** All ten families cover printable Latin-1
(0x20-0x7E, 0xA0-0xFF) completely, except `U+00AD SOFT HYPHEN` in Bitter and
Spectral — an invisible formatting control no game text uses.

**Optical spread is ~30%, confirming the `scale` knob is mandatory.** Measured
x-heights run 445 (IM Fell italic) to 579 (Petit Formal Script) per 1000 upem,
cap-heights 603 to 786. At an identical pixel size those faces do not read as
the same size. Full table in `assets/fonts/README.md`; use it to seed each
role's initial `scale`.

### Phase 1 — FontLibrary

Shared face blobs, (face, size) keying, `CommitAll`, fonts.cat loading. No
behaviour change: every role points at the Consolas fallback and the game looks
identical. This phase is pure plumbing and should be verifiable by "nothing
moved".

### Phase 2 — The Em seam

`Widget::fontRole` + inherited resolution, `Em` following the widget, `Rem`
explicitly unchanged. Extend `uitree` to print each widget's resolved role so
inheritance is inspectable the way the rest of the tree already is.

### Phase 3 — Migrate the 26 owners

GameUI's 8, DevConsole, MapView, and the 8 dialogs' font+context pairs become
library lookups. Centralizing `SetHeight`/`Commit` into one loop over live fonts
also **fixes a live bug**: `m_savesUi` is constructed and themed (GameUI.cpp:153,
161) but is absent from `UpdateFonts`, so the saves page font never rescales
with the window and never commits new glyphs. One loop cannot forget a context
the way seven hand-written lines can.

### Phase 4 — The audition harness

Dev console `font <role> <file>` hot-swaps live, `fonts` lists installed
families. Then drive the game (docs/drive.ps1) across HUD, menu, character
sheet, editor and console for each candidate and judge them **in-game at real
sizes against real dungeon art** — not from a specimen sheet. The winner is a
one-line fonts.cat edit, not a code change.

This is the phase that actually decides the typefaces. Everything before it is
scaffolding to make the decision cheap and reversible.

### Phase 5 — Apply the roles

Display on the title and menu/sheet headers; Mono on the dev console and editor
numeric fields; Script on spell descriptions in the sheet's Known Spells tab
(scrolls do not exist as content yet, but spell `.desc` text does, and it is
exactly the in-world register Script is for).

### Phase 6 — Localization hook and docs

Let a `.lang` file name a font override (`lang.font = <family>`), the same
self-describing idiom as `lang.name`, so a script the chosen families do not
cover — CJK above all — can point at one that does. Then CLAUDE.md and this doc.

## Traps

**Variable fonts.** stb_truetype has no font-variations support
([nothings/stb#509](https://github.com/nothings/stb/issues/509)). Google Fonts
now ships variable `.ttf` by default; those render only at the default axis
position. Take the `static/` instances from the download. Consequence: **bold is
a separate face**, not a flag — so if headings need weight, that is another
fonts.cat entry, and today emphasis is carried by colour (`theme.text` /
`textDim` / `accent`) which may well be enough.

**SetHeight thrash.** Dialogs currently call `SetHeight` on their own font and
their context's font independently. If two owners ever shared one `Font` at
different sizes they would re-bake against each other every frame, and `Commit`
calls `WaitIdle` — a per-frame full GPU drain. Keying the library by (face,
**rounded** pixel height) makes that structurally impossible: different sizes
are different `Font` objects.

**17px is the real constraint.** stb_truetype is unhinted with grayscale AA, and
the HUD is 17px at the 900px design window. A high-contrast old-style serif
turns to mush there. The backbuffer is `R8G8B8A8_UNORM` (non-sRGB), so blending
happens in gamma space — the forgiving case for thin stems, but it will not
rescue a delicate face. If the audition finds nothing survives, the fallback is
a sturdy body face (Bitter, Gentium) with character carried by Display and
Script instead.

**Optical size.** Covered by the `scale` knob above; flagged again because it is
the thing most likely to look like "the layout broke" after a face swap.

**OFL obligations.** Ship the license text with the fonts, and do not rename a
font's internal name (the Reserved Font Name clause). Renaming files on disk is
fine; modifying the font and keeping the name is not.

**Atlas count.** Each (face, size) pair costs an atlas and an SRV slot from the
recycling free list (kSrvHeapCapacity = 1024). Sharing should leave the net
count *below* today's 26; watch it anyway once four roles are live.
