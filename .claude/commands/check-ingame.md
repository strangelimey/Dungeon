---
description: Level files, installed models, and a UI overlap sweep of every screen
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Two audits that need a running game, sharing one launch (~45 s).

- no argument → `.\tools\CheckAll.ps1 -Only ingame`
- `selftest` → `.\tools\CheckAll.ps1 -Only ingame -SelfTest` (must FAIL)

## What it is guarding

**`levelcheck`** — every level file present, every model a catalog type names
installed. The baked pool is gitignored, so a fresh clone or a stale worktree
provision has entries whose assets are absent. Scoped to **models** on purpose:
a missing texture renders magenta and is survivable, a missing model is a
`LoadModelOrDie` that takes the process down at level load — possibly on a level
nobody has visited in weeks.

**`uioverlap`** — CLAUDE.md says run it after touching any screen, and the one
manual sweep found four defects nobody had reported. Four screens: hud, paused,
map, editor.

## Reading a failure

**`MISSING MODEL '<x>' named by type '<y>'`** — the pool is incomplete. Usually
means a worktree was provisioned from a stale file list; re-run
`tools\FetchModels.ps1`, or robocopy `assets\models` from a populated sibling.

**`<label> never reached the log`** — the sweep did not actually open that
screen, so it audited nothing. This is a *coverage* failure, and it is the more
serious kind: it caught its own first version sending `Esc`/`M` while the console
was open, which ate the keystrokes and audited the HUD three times while
reporting four clean screens.

**`uioverlap found overlaps`** — a real layout defect. The findings name both
widgets; `uitree dump <context>` in the dev console gives the pixel rects.

The **settings page and character sheet are not swept** — both need a mouse
click, and a scripted click against a moving layout is how a sweep starts
silently auditing the wrong screen. The run prints that every time.
