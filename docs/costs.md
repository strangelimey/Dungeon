# Costs

Running ledger of what the Dungeon project costs to build: **asset purchases**,
**AI / API usage**, and **dev tools / licenses**. (Cloud/hosting is NOT tracked
here — that belongs to the separate Stonecroft project, not this game.)

Keep this current: add a row whenever something is bought. Record amounts in
their native unit (USD, or store credits) and leave a field blank/`?` rather than
guessing. "As of" dates make stale totals obvious.

---

## 1. Asset purchases

### textures.com — scanned PBR surface/prop sets
Bought on Michael's account; credits, not direct USD. The full per-set manifest
+ workflow live in the `textures-com-sourcing` memory.

| as of | credits start | credits spent | credits left | sets bought | bundle USD |
|---|---|---|---|---|---|
| 2026-06-22 | 5,000 | 1,575 | **3,425** | 17 | **$39.00** |

- The 5,000-credit bundle cost **$39.00** (~$0.0078/credit). Value consumed so
  far: 1,575 credits ≈ **$12.29**; remaining 3,425 ≈ **$26.71** (already paid).
- **Rate card** (credits per map/set): AO flat = 1; SCAN set 2K = 41 / 4K = 101;
  PBR set 2K = 61 / 4K = 81; +metallic (metals) 2K = 76 / 4K = 101.
- **Unlock rule:** buying a resolution makes all LOWER res free and discounts
  higher res by what you paid → buy the highest res you'll want FIRST.

### fab.com — authored meshes (Epic marketplace)
Import pipeline: see the `fab-sourcing` memory + `tools/FetchModels.ps1`.
Selection rule: the listing's "Included formats" must include glb/obj/fbx —
Unreal-only `.uasset` packs are unimportable.

| date | listing | seller | price | formats | imported? |
|---|---|---|---|---|---|
| 2026-06-22 | Fantasy Assassin Weapon Pack | Deepanshu | $1.99 | glb/obj/fbx/usd | in progress — multi-material, needs render work |

Scouted candidates (prices as seen 2026-06-22, not bought):
- Fantasy Assassin Weapon Pack (Deepanshu) — **$1.99–4.99**, glb+obj+fbx, 18 meshes
  — recommended first real purchase to prove the pipeline.
- Medieval One Hand Swords w/ Scabbard (Quantum Assets) — $29.99–49.99 —
  **Unreal-only, do NOT buy** (unimportable).

### Other asset packs (sound / music / etc.)
| date | item | source | price | notes |
|---|---|---|---|---|
| — | _(none yet)_ | | | |

---

## 2. AI / API usage
Spend on Claude / LLM usage and other AI dev tooling used to build the game.
Hard to attribute per-project precisely; record known bills/top-ups here.

| period | service | amount | notes |
|---|---|---|---|
| — | Claude Code (Claude / Anthropic) | ? | _fill from billing; the project is built collaboratively with Claude_ |

---

## 3. Dev tools / licenses
One-off or recurring software bought for the project.

| item | cost | notes |
|---|---|---|
| Blender 5.1 | **free** | FBX/USD→glTF conversion in the fab pipeline (`tools/ConvertMesh.py`) |
| Visual Studio 2026 Community | **free** | compiler + bundled CMake/Ninja (`build.cmd`) |
| _(add paid tools/licenses here)_ | | |

---

## Totals (as of 2026-06-22)
- **Asset purchases:** textures.com **$39.00** (5,000-credit bundle; 3,425 left,
  ~$26.71 prepaid headroom); fab.com **$1.99** (Fantasy Assassin Weapon Pack);
  other $0.
- **AI / API:** not yet recorded.
- **Dev tools / licenses:** $0 (all free so far).
- **Cash out of pocket so far:** **$40.99** ($39.00 textures.com + $1.99 fab.com).
