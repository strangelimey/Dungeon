# Torches and fire

How the party sees, and what the dungeon's fires are. Built by the spell-updates
thread (docs/spell-updates-plan.md Phases 3 and 4), because the tier-1 spells
light, douse and flare fires and fill containers - which needed fires to be live,
saved state and items to carry a charge. The spells themselves are in
docs/spells.md.

## The held torch is the light

There is no light at the eye any more (Michael, 2026-10-01). Each LIT torch a
member holds in a hand - and one riding the cursor - is a flickering point light
at that member's side of the view (`DungeonWorld::AppendCarriedLights`,
DungeonWorld_Light.cpp). With none, the party sees by the level's ambient alone,
and a level authored at ambient 0 is pitch black. The starter party carries one:
Sera holds a lit torch in her right hand (`CreateDefaultParty`).

## A torch burns down

A lit torch burns WHILE IT IS HELD (`TickCarriedLight`, every frame, allocating
nothing). Its CHARGE counts its seconds down from the kind's `burn_time`; over
its last tenth it dims to 35% (`TorchBrightness`); spent, it becomes its
`spent_as` (the stub) and the log says "Sera's torch burns out".

| Kind | Unlit id | Burns | Model |
| --- | --- | --- | --- |
| crude | `torch_crude` | 600 s (10 min) | fab pack model #2 |
| common | `torch` | 900 s (15 min) | fab pack model #1 |
| fine | `torch_fine` | 1500 s (25 min) | fab pack model #5 |
| spent | `torch_stub` | - | the crude model for now |

Each lit kind is its own catalog entry (`torch_lit` ...) linked both ways:
`lit_as` on the unlit kind, `unlit_as` / `spent_as` / `burn_time` on the lit one
(`ItemKind::Lit()` = has a `burn_time`). Lighting or dousing a held torch RENAMES
the item in its slot, so nothing is created or destroyed.

Put out, a torch KEEPS what it had left:
- stowed in a pack, it goes out (`TickCarriedLight`);
- dropped on the floor, it goes out (`PlaceDrop`'s `unlit_as`);
- thrown, it flies with its charge (`Projectile::cargoCharge`) and lands with it.

## Charge is part of the item, everywhere

Michael's ruling (Q2): burn state is per ITEM, so it travels with the item and a
half-burnt torch cannot be refilled by setting it down. The charge lives in
every place an item can be:
- a slot is an `ItemSlot {typeId, charge}` (Inventory.h; `kNoCharge` = -1 = none
  / a fresh one starts full) - hands, pack, bags, equipment;
- the cursor's `HeldItem` carries one (`Set`, `SwapWith`, `Charge`);
- a floor `Item` carries one, and so does a thrown item in flight;
- the save writes a slot as `id#charge` (`ItemToken` / `ItemFromToken`), and a
  floor drop's line takes a sixth field - an older save reads as "no charge".
Moves are swaps, so a charge can never be left behind in a slot or duplicated.

## Wall torches come off the wall

A sconce whose fixtures.cat kind names a `torch_item` (and an `empty_model`) can
be TAKEN: a click on it from its square puts its torch - lit if the sconce burned
- in the LEADER's free hand (right, then left), else on the cursor, and leaves the
bare bracket (`wall_torch_bracket`, made by tools/BuildWallTorchBracket.py). Any
burnable torch clicked onto an empty bracket MOUNTS (lit or not, as it was); a
stub does not. Click order in the 3D view: mount the cursor's torch, then drop,
then throw; with an empty cursor, pick up, then take the torch, then the door.

## Fires are live, saved state

A sconce or brazier's lit state is authored (`lit`) in the .map; what PLAY did to
it is a FLIP on the map's fixture (`flipped`, plus a sconce's `empty`), read
through `Burning()`. The world's `Fire` for each (DungeonWorld) carries the
particles, the light, a FLARE level (Puff of Wind: bigger and brighter, decaying
over 1.2 s) and its own `effects` list.
- `SetFireBurning(x, z, wall, burning)` is the one way to light or put out a fire
  (DungeonWorld_Fires.cpp): the spells, `castsvc light|douse`, a smashed bracket
  and the save all go through it. A flameless kind (`flame = 0`) never catches.
- The level's dynamic state saves a diff from the authored state: `fire x z wall
  burning empty` lines, restored QUIETLY on arrival (no fresh smoke).
- A new game puts every fire back as authored. TRAP fixed in Phase 8: the STASHED
  static map of a level the party left used to keep the play flags, so a fire
  doused before a new game came back out in it; `StashStaticMap` now resets them
  on the copy (fires and niches alike) and the dynamic state carries them.

## A doused fire smokes

When a fire goes out in play, the fixture kind's `on_douse` effects (fixtures.cat:
`smoke 0.6 2.5` on a sconce, `smoke 1.0 4` on a brazier - magnitude, then
seconds) land on the FIRE through the ordinary effects system (`fx::Apply`;
effects.cat `[smoke]`, the `SmokeEffect` class, a `haze` kind). Its haze is
magnitude x the share of its time left, so it thins away. Each frame the world
gathers up to `kMaxDustPuffs` (4) hazes into the frame's `Atmosphere::dustPuffs`,
and scene.hlsl's `DustDensity` adds them to the turbidity as soft spheres 1.5
squares across. Nothing is stored, so nothing can drift.

The turbidity GRID itself (a fire's ring of smoke while it burns) is refreshed IN
PLACE when a fire changes: `DungeonWorld::RefreshTurbidity` refills the kept
pixels and flags `m_turbidityDirty`, and the next RenderScene copies them into
the existing texture through `Renderer::UpdateTexture` - no texture rebuilt, no
GPU stall. (The breakables thread built the same path for a smashed fixture at
the same time; the merge kept that one and dropped this thread's
`Texture::UpdateLevel0`.) A fire changed in PLAY recomputes the map's turbidity without bumping
`DungeonMap::Revision()` (`RecomputeTurbidity`): the revision keys the AI's
walkability grid, the shadow-cube cache and the editor's undo, and a bump rebuilt
the AI grid on every hand spell. Editor edits to a fixture still bump it.

## Water containers

A waterskin has three states, each its own catalog entry: `waterskin` (full),
`waterskin_half`, `waterskin_empty`. A drink steps it DOWN (`drink_as`, 25
hydration a step); Splash fills it one step UP (`fill_as`). A full skin is not
filled again, so a Splash with a full skin in hand falls through to dousing.

## Known gaps

- REST runs the world at 60x, so a held torch burns 60x faster while resting.
- A MOUNTED torch forgets its charge: taking it back gives a fresh one.
- The stub and the pebble borrow other models; there is no visible puff in the
  hand when a tier-1 spell is cast (the log line and the world's change say it).

## Dev and checks

Dev: `torch [status | take | mount [item] | charge <member> <hand> <seconds>]`,
`castsvc fire | light | douse | flare | floor`, `equip <item|none> [member]
[hand]`. Checked by `tools\SpellTest.py` (the douse / save / new-game checks) and
`tools\AllocTest.ps1 -Hand` (lighting, dousing, flaring and filling inside a
guarded window).
