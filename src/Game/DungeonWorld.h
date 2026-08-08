// ============================================================================
// Game/DungeonWorld.h — the 3D world and everything living in it.
//
// Owns the dungeon's two map layers (static .map structure, dynamic .ent
// spawns), the party, the monsters, the fires (sconces + braziers with
// particle effects), the batched surface geometry with its texture
// variants, the lights, and the camera — plus the per-frame simulation and
// both render passes (cube shadow maps, then the main scene).
//
// The world knows nothing about menus or the app state machine: Game decides
// when to load it (AppendLoadTasks feeds the staged loader), when to step it
// (Update only runs while playing), and when to draw it. Feedback flows out
// through onMessage (the HUD log) and the shared SoundBank.
// ============================================================================
#pragma once

#include "Animation/Animator.h"
#include "Animation/CreatureState.h"
#include "Assets/Model.h"
#include "Audio/AudioEngine.h"
#include "Game/Balance.h"
#include "Game/Character.h"
#include "Game/Combat.h"
#include "Game/DungeonEntities.h"
#include "Game/DungeonMap.h"
#include "Game/DungeonMeshBuilder.h" // WallPanels (the worn wall block's variants)
#include "Game/FireEffect.h"
#include "Game/GameSettings.h"
#include "Game/LoadQueue.h"
#include "Game/Magic.h"
#include "Game/MonsterAI.h"
#include "Game/Party.h"
#include "Game/Project.h"
#include "Game/Projectiles.h"
#include "Game/SaveGame.h"
#include "Game/ShadowScheduler.h"
#include "Game/SlotGrid.h"
#include "Game/SoundBank.h"
#include "Graphics/Camera.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/ModelPreview.h" // gfx::PreviewSubmesh (editor instance previews)
#include "Graphics/ParticleBatch.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"

#include <array>
#include <flat_map>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

struct GeometryChunk; // DungeonMeshBuilder.h (MakeSurfaceChunk takes one by ref)

// Non-rune items reuse the rune tablet mesh as a placeholder, rendered at this
// scale (bigger than a rune so they read on a dark floor) — see SubmitScene-
// Geometry. Pickup is a floor-quarter click test (TryPickItem), independent of
// the rendered size.
inline constexpr float kItemPlaceholderScale = 2.2f;

class DungeonWorld {
public:
	DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
				 audio::AudioEngine& audio, const SoundBank& sounds,
				 const GameSettings& settings, const Project& project,
				 threads::Manager& threadManager);

	// Appends the dungeon's load tasks (blocks, textures, batched meshes,
	// monsters, fires, dust) to the staged loader. Order matters:
	// textures register their variant counts before the geometry task buckets
	// cells by variant. The texture work is split one task per material — the
	// scanned sets are the bulk of the load (~300 MB at Ultra), and
	// per-material tasks keep the progress bar moving through them.
	void AppendLoadTasks(LoadQueue& queue);

	// Hot-swaps the dungeon meshes (and textures, when crossing a resolution
	// boundary) for the settings' new quality tier, if they are already
	// built. Drains the GPU first — it may still be reading the old data.
	void ApplyQuality(bool textureResChanged);

	// Reloads the worn block meshes and rebuilds the batched dungeon geometry in
	// place (the ApplyQuality core). The editor's Wall Style rebake calls this
	// after re-baking a texture's worn_*.gltf to swap the new geometry in live.
	void ReloadDungeonBlocks(bool textureResChanged = false);
	// Re-reads the surface catalogs' PER-DRAW material knobs (parallax depth,
	// metallic/roughness) and pushes them live — no reload, no rebuild. The type
	// editor calls it when a surface type is saved: only texture/relief/wear
	// change baked geometry, everything else can just take effect.
	void RefreshSurfaceMaterials();
	// The PROP counterpart: drops one catalog type's cached kind so the next
	// resolve re-reads the catalog (model, texture set, material overrides,
	// scale, flags), then re-spawns the live objects that used it. A kind is
	// loaded once and cached by type name, so without this a saved edit only
	// showed on the next level entry. `catalogKey` picks the cache.
	void ReloadTypeKind(const std::string& catalogKey, const std::string& id);

	// "Start New Game": snaps the party home, re-arms the monster
	// announcements, and resets the torch palette (which speaks via
	// onMessage — the caller clears the log right after, as before).
	void ResetForNewGame();

	// One simulation step: party input/movement, animators, monster
	// announcements, lights (with shadow-slot assignment), camera, and the
	// fire particles (gathered back-to-front for the smoke blend).
	// acceptInput=false simulates the world but ignores party movement keys —
	// used while the dev console is open (the world keeps running, the party
	// stays put). Everything else (physics, monsters, lights, particles)
	// updates regardless.
	void Update(const Input& input, float dt, float time, bool acceptInput = true);

	// Per-frame arena rotation for the world-owned batches (safe pre-load).
	void NewFrame(u32 frameIndex);

	void RenderShadowMaps(ID3D12GraphicsCommandList* list);
	void RenderScene(ID3D12GraphicsCommandList* list);

	// Renders model items' 3D thumbnails into their icon render-targets: a soft
	// round halo (via `sprites`) then the lit 3D model over it. Static icons bake
	// once (before the first scene); `icon_spin` icons re-bake every frame on a
	// turntable. Redirects the OM and rebinds the back buffer.
	void UpdateItemIcons(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites);
	// The baked 3D icon for an item type (building its kind on demand), or null
	// for a model-less item (the caller falls back to its flat placeholder).
	const gfx::Texture* ItemIconFor(const std::string& typeId);
	// Renders the map overlay's baked icons: each monster kind's HEAD SHOT (its
	// mesh in rest pose framed on the model's top quarter — a skull for the
	// skeleton), each decoration kind's whole model (props read best in full;
	// covers button levers too, they share the kind cache), and the sconce +
	// brazier fixture meshes. Bakes once per kind; call every frame like
	// UpdateItemIcons (and unlike it, also while the editor covers the scene —
	// the map overlay is what draws these). Redirects the OM and rebinds.
	void UpdateMapIcons(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites);
	// Every baked icon target is this square (the shared depth target and the
	// bake's viewport), so a caller supplying its OWN target must match it.
	static constexpr u32 kIconSize = 256;
	// Bakes an arbitrary POOL mesh into a caller-owned icon target (the asset
	// picker's model tiles): the map-icon rig, nothing cached here. The target is
	// the CALLER's because creating one drains the GPU (gfx::Texture::
	// RenderTarget), which must not happen while a frame is being recorded.
	// LIFETIME: this only RECORDS the draw, so `mesh` must outlive the frame.
	void BakeIconFor(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
					 const gfx::Mesh& mesh, const Vec3& lo, const Vec3& hi,
					 const gfx::Texture& target);
	// The baked icons for already-loaded kinds, or null (not loaded / not baked
	// yet) — the map overlay then falls back to its square markers. These never
	// force-load a model (browse markers may name unloaded types).
	const gfx::Texture* MonsterIconFor(const std::string& type) const;
	const gfx::Texture* DecorationIconFor(const std::string& type) const;
	const gfx::Texture* ItemIconLookup(const std::string& type) const;
	// The baked map icon for a fixture kind (null until its one-shot bake ran —
	// the map overlay falls back to a colored marker). Kinds load lazily on
	// first use (BuildFires / placement), so the active level's are always in.
	const gfx::Texture* FixtureIcon(const std::string& type) const;

	// Torchlight palette (the HUD dropdown): 0 warm, 1 cold blue, 2 eerie
	// green. Announces the change through onMessage.
	void SetTorchPalette(int index);

	Party& GetParty() { return m_party; }

	// --- combat -------------------------------------------------------------
	// Points the world at the Game's party roster (filled once, reset in place)
	// so monster melee can drain member health and party melee can read each
	// member's derived stats. Must be set before play; null = no combat.
	void SetRoster(std::vector<Character>* roster) { m_roster = roster; }
	// A hand-slot click: the given roster member swings the given hand (0 = left,
	// 1 = right) at the monster in the cell directly ahead of the party.
	// Resolves a strike when the member is up, that hand is off cooldown, and a
	// live monster is there; logs the outcome and kills the monster at 0 hp. A
	// no-op (returns false) otherwise. `verb` is the executed melee command
	// ("stab", "chop", ...) — the ATTACK whose spec (damage type + numbers,
	// Balance::FindAttack) shades the strike; empty/unknown = neutral (the
	// dev-console path).
	bool PartyAttack(size_t member, size_t hand, std::string_view verb = {});
	// Spend stamina as EXERTION (docs/combat.md part 3): drains the bar and
	// feeds VIT's creep pool at the vit_exertion knob. The Phase 4 stamina
	// costs (swings, marching) all route through here.
	void SpendStamina(Character& member, float points);
	// Re-derive every member's resource maxima from the balance k's — after a
	// save-apply, a stat change, or an editor Balance apply.
	void RecomputePartyMaxima();
	// The live combat tuning (balance.cat + attacks.cat knobs, Balance.h). The
	// editor's Balance dialog edits it in place and Save()s it via the project.
	Balance& GetBalance() { return m_balance; }
	const Balance& GetBalance() const { return m_balance; }
	// The status-effect registry — the one place an effect id resolves to its
	// kind. The save loader (Game::ApplyState) reads it to rebuild a member's
	// effects; everything else already holds kind pointers.
	const fx::EffectBook& Effects() const { return m_effects; }
	// Land an effect on the monster the party faces (dev console). False if
	// there is nothing ahead or no such effect. The monster side of the effect
	// list has no other hand-authored entry point.
	bool ApplyEffectAhead(std::string_view id, float magnitude, float seconds);

	// --- spell casting ------------------------------------------------------
	// Façade over the MagicSystem (m_magic): the given roster member casts the
	// spell whose recipe matches the symbol sequence, fired down the party's faced
	// direction from the eye. The caster must be standing; the magic module gates
	// known-symbols / recipe / mana and spawns the bolt. This turns the cast
	// outcome into HUD/audio feedback and returns true on a successful cast. The
	// bolt's flight + impact run in MagicSystem::Update (see Magic.h). Driven by
	// the casting UI and the dev console `cast`.
	// `hand` is the hand slot the cast was fired from (0 = left, 1 = right):
	// a successful cast credits THAT hand's quick-cast MRU. -1 (dev console,
	// no hand context) casts normally but touches no MRU.
	bool CastSpell(size_t member, std::span<const SpellSymbol> sequence,
				   int hand = -1);
	// Same cast, referencing the recipe by catalog id (the hand-slot Magic menu
	// stores "cast:<id>" defaults). All the same gates apply — the member must
	// know the recipe's symbols and afford its mana. False on an unknown id.
	bool CastSpellById(size_t member, std::string_view id, int hand = -1);
	// The whole spell registry (the Magic menu filters it by known symbols).
	std::span<const std::unique_ptr<Spell>> SpellDefs() const {
		return m_magic.Book().Defs();
	}

	// Fired once when the last standing member goes down (Game ends the run).
	std::function<void()> onPartyWipe;

	// --- item pickup / drop (mouse-driven) ----------------------------------
	// Tries to pick up a floor item under the screen point (mx,my) in a w×h
	// viewport: an item is hit when the click ray, sampled at that item's own
	// visible height, lands in the quarter (the Medium 2x2 slot grid) the item
	// rests in — gated by reach (the cell is the party cell or orthogonally
	// adjacent) + seen. The top item (last in render order) wins. It is removed
	// from the floor and its catalog id returned (Game puts it on the cursor);
	// nullopt if nothing pickable is under the cursor. Pure query+remove — no
	// satchel/knowledge side effects.
	std::optional<std::string> TryPickItem(float mx, float my, float w, float h);
	// Drops a held item (catalog id) back onto the floor: ray-casts the screen
	// point to the floor plane and places it on that cell when it is walkable, in
	// reach, and seen; otherwise on the party's own cell. The tablet snaps to the
	// quarter slot nearest the hit point. Always succeeds.
	void DropItemAt(const std::string& typeId, float mx, float my, float w, float h);
	// The data-driven hand commands for an item id (its ItemKind::commands) — the
	// single source the HUD's hand right-click menu builds from.
	const std::vector<std::string>& ItemCommands(const std::string& id) {
		return ItemKindFor(id).commands;
	}
	// The Medium quarter slot (0..3) in cell (cx,cz) whose centre is nearest the
	// world point (wx,wz) and is NOT already taken by another floor item there
	// (self excluded by index, -1 = none). Falls back to the geometrically nearest
	// quarter if all four are occupied. Floor items use the Medium 2x2 grid.
	int FreeItemSlotNear(int cx, int cz, float wx, float wz, int self) const;

	const DungeonMap& Map() const { return m_map; }
	const Project& GetProject() const { return m_project; }
	const DungeonEntities& Entities() const { return m_entities; }
	const std::string& CurrentLevel() const { return m_currentLevel; }

	// --- editor: monster animation config (the right-click config dialog) -------
	// The animation table for a monster type, as N=kCreatureStateCount arrays.
	using AnimSupport = std::array<bool, anim::kCreatureStateCount>;
	using AnimClips = std::array<std::vector<std::string>, anim::kCreatureStateCount>;
	// All clip names shipped by a type's model (the pool the dialog offers per
	// state). Force-loads the kind if it wasn't placed in the level.
	std::vector<std::string> MonsterClipNames(const std::string& type);
	// The type's current supported-state set + per-state clip table.
	void MonsterAnimConfig(const std::string& type, AnimSupport& supported,
						   AnimClips& clips);
	// Writes a new config straight into the cached kind (live — DriveMonsterAnim
	// reads it every frame). Clip names are filtered to ones the model has; Idle
	// is forced supported (the rest floor). Does NOT persist (Game writes the .cat).
	void ApplyMonsterAnimConfig(const std::string& type, const AnimSupport& supported,
								const AnimClips& clips);

	// Read/apply the type's BEHAVIOUR fields (archetype + params) for the config
	// dialog's Behavior tab, mirroring the anim-config pair. Apply sets the live
	// kind (AI reads it via the snapshot next frame); it does NOT persist.
	void MonsterBehaviorConfig(const std::string& type, ai::Archetype& archetype,
							   float& keepRange, float& fleeBelow, std::string& spell,
							   ThreatTuning& threat);
	void ApplyMonsterBehavior(const std::string& type, ai::Archetype archetype,
							  float keepRange, float fleeBelow, const std::string& spell,
							  const ThreatTuning& threat);
	// Catalog ids of the project's spells — the options for the caster spell dropdown.
	std::vector<std::string> SpellIds() const;

	// Editor entity inspector: read a placed monster's per-INSTANCE overrides at a
	// cell (the first live one there), and apply edits back to it by runtimeId (live;
	// the .ent writer persists on SaveLevel). Read returns false if no monster is
	// there. `type` is the kind's catalog id.
	bool MonsterInstanceAt(int cx, int cz, u32& runtimeId, std::string& type, bool& asleep,
						   float& leashRange, ai::Archetype& archetype, float& keepRange,
						   float& fleeBelow, std::string& spell, Direction& facing) const;
	// Same read, keyed by the stable runtimeId (for the multi-object inspect picker,
	// which resolves one of several monsters sharing a cell). False if id not found.
	bool MonsterInstanceById(u32 runtimeId, std::string& type, bool& asleep,
							 float& leashRange, ai::Archetype& archetype, float& keepRange,
							 float& fleeBelow, std::string& spell, Direction& facing) const;
	void ApplyMonsterInstance(u32 runtimeId, bool asleep, float leashRange,
							  ai::Archetype archetype, float keepRange, float fleeBelow,
							  const std::string& spell, Direction facing);
	// A live monster's threat table + lock, read-only (the inspector's threat
	// row; display-only runtime state, never authored). False if id not found.
	bool MonsterThreatById(u32 runtimeId, std::array<float, 4>& threat,
						   int& lock) const;
	// Every grudge in the live world, ONE STRING PER monster with any threat
	// ("<type>#<runtimeId> [b c d e] lock=<name|->") — the dev console `threat`
	// command prints them a line each. Empty = nobody holds one.
	std::vector<std::string> ThreatReport() const;

	// Patrol-route editing (grid-click authoring in the editor). Append/undo/clear a
	// monster's waypoint route by runtimeId (live; the .ent writer persists it on
	// SaveLevel), and read it back for the route overlay.
	void AddPatrolWaypoint(u32 runtimeId, int cx, int cz);
	void RemoveLastPatrolWaypoint(u32 runtimeId);
	void ClearPatrol(u32 runtimeId);
	const std::vector<ai::Cell>* MonsterPatrol(u32 runtimeId) const;
	// The runtimeId of the first live monster at (cx,cz), or 0 — for editor selection.
	u32 MonsterRuntimeIdAt(int cx, int cz) const;
	// Every live monster on (cx,cz) as (runtimeId, kind catalog id) — for the editor's
	// multi-object inspect picker (a cell may stack several monsters).
	std::vector<std::pair<u32, std::string>> MonstersAt(int cx, int cz) const;
	// A wall sconce (torch) on (cx,cz)? Reports the wall it hangs on — for editor
	// selection / the fixture inspector.
	bool SconceAt(int cx, int cz, Direction* wall = nullptr) const;
	// Every wall sconce (torch) on (cx,cz), by the wall each hangs on — several may
	// share a cell on different walls (the inspect picker lists them).
	std::vector<Direction> SconcesAt(int cx, int cz) const;
	// The solid neighbour walls of (cx,cz) a sconce may hang on (for the torch
	// facing dropdown).
	std::vector<Direction> SolidWallsAt(int cx, int cz) const;
	// Re-hang the sconce at (cx,cz) from wall `from` onto `to` (live: updates the
	// map and rebuilds fires/dust). `from` disambiguates several sconces in a cell.
	bool RemountSconce(int cx, int cz, Direction from, Direction to);
	// Read/write a torch's per-instance light/smoke settings (identified by cell +
	// wall). Set is live: the light/flame/smoke follow next frame. `brightness` is in
	// cells, `turbidity` 0..1. Both return false if no such sconce.
	bool TorchSettings(int cx, int cz, Direction wall, bool& lit, float& brightness,
					   float& turbidity) const;
	bool SetTorchSettings(int cx, int cz, Direction wall, bool lit, float brightness,
						  float turbidity);
	// A floor brazier on (cx,cz)? Plus its per-instance light/smoke settings (live
	// on Set). Both return false if no brazier is there.
	bool BrazierAt(int cx, int cz) const;
	bool BrazierSettings(int cx, int cz, bool& lit, float& brightness, float& turbidity) const;
	bool SetBrazierSettings(int cx, int cz, bool lit, float brightness, float turbidity);
	// A fixture instance's catalog id (for the inspector's preview/title);
	// falls back to the classic ids when the instance isn't found.
	std::string SconceTypeAt(int cx, int cz, Direction wall) const;
	std::string BrazierTypeAt(int cx, int cz) const;

	// Editor multi-object inspect: decorations / floor items on a cell. Decorations
	// come back as (index into the live decoration list, catalog id); items as
	// (stable entity id, type). Those handles drive the *Facing accessors below.
	std::vector<std::pair<int, std::string>> DecorationsAt(int cx, int cz) const;
	std::vector<std::pair<int, std::string>> ItemsAt(int cx, int cz) const;

	// In-flight projectiles (spells/arrows/thrown items): transient content the
	// editor draws on the map and can inspect. LiveProjectiles is every one (map
	// markers); ProjectilesAt filters to a cell (the inspect picker); Find/Remove
	// address one by its stable runtime id (the inspector's dismiss action).
	std::vector<ProjectileInfo> LiveProjectiles() const { return m_projectiles.Live(); }
	std::vector<ProjectileInfo> ProjectilesAt(int cx, int cz) const;
	bool ProjectileById(u32 id, ProjectileInfo& out) const {
		return m_projectiles.Find(id, out);
	}
	bool RemoveProjectile(u32 id) { return m_projectiles.Remove(id); }
	Direction DecorationFacing(int index) const;
	void SetDecorationFacing(int index, Direction facing); // rebakes the transform
	Direction ItemFacing(int entityId) const;
	void SetItemFacing(int entityId, Direction facing);
	// Targeted removal by the same handles (the inspectors' Delete button —
	// unlike RemoveEntityAt's ladder, these take out exactly the inspected
	// object). Decorations refuse a stair prop (RemoveStairAt owns those); the
	// item's .ent record goes with it. Both return false on a stale handle.
	bool RemoveDecorationByIndex(int index);
	bool RemoveItemById(int entityId);
	// The rest of the inspectors' Delete, likewise TARGETED rather than
	// RemoveEntityAt's ladder — a cell can hold several kinds at once, and the
	// dialog knows which one it is showing. Monsters key off the stable
	// runtimeId because they move; the others are one-per-cell.
	bool RemoveMonsterByRuntimeId(u32 runtimeId);
	bool RemoveDoorAt(int x, int z);
	bool RemoveButtonAt(int x, int z);
	// The inspected decoration's raw catalog id (its display name is what the
	// picker shows) — "" on a stale handle.
	std::string DecorationTypeByIndex(int index) const;
	// The per-TYPE map facing-arrow flag (catalog facing_arrow, default 1):
	// no-load queries for the marker/browse paths, and the live half of the
	// inspector checkbox's type edit (Game writes the catalog field + saves).
	bool DecorationShowsFacing(const std::string& type) const;
	bool MonsterShowsFacing(const std::string& type) const; // faces= flag
	void SetDecorationFacingArrow(const std::string& type, bool show);
	// Any editor-inspectable object on the cell (monster/torch/brazier/decoration/item)?
	bool AnyInspectableAt(int cx, int cz) const;
	// Erase a fixture (sconce or brazier) at the cell, live (rebuilds fires/dust).
	bool RemoveFixtureAt(int cx, int cz);
	// Removes the sconce on ONE named face (the editor's edge-pick). A cell can
	// ring itself with a sconce per wall, so the cell-wide call picks arbitrarily.
	bool RemoveFixtureAtFace(int x, int z, Direction wall);

	// Everything the editor's monster-config dialog preview needs to animate a
	// type's mesh: the (stable, cached) mesh + skeleton + clips a borrowed Animator
	// plays, the resolved render material, and the render-time size fixup. The
	// skeleton/clips pointers stay valid for the session (they live in the cached
	// MonsterKind), so the caller may hold an Animator over them.
	struct MonsterPreviewData {
		const gfx::Mesh* mesh = nullptr;
		const assets::SkeletonData* skeleton = nullptr;
		const std::vector<assets::AnimationClipData>* clips = nullptr;
		gfx::MaterialParams material;
		// Every drawable piece: one entry for a single-mesh kind, one per
		// primitive for a multi-material rig. Consumers draw these (with the
		// palette); mesh/material above remain the meshes[0] view.
		std::vector<gfx::PreviewSubmesh> subs;
		float modelScale = 1.0f;
		float modelYaw = 0.0f; // render-time facing fixup, so the preview matches in-world
	};
	MonsterPreviewData MonsterPreviewFor(const std::string& type); // force-loads the kind
	// Whether a monster type's <model>.gltf exists (so the editor can guard the
	// right-click force-load and warn instead of aborting on a missing asset).
	bool MonsterModelAvailable(const std::string& type) const;

	// Fixture flame geometry, shared by the in-world fires (BuildFires) and the
	// inspector previews so the preview flame sits exactly where the world one
	// burns: local Y of the flame origin IN UNITS (it is a point on the model,
	// so it scales with kUnit like the mesh), and the dimensionless
	// particle-effect scale (a sconce burns smaller than a brazier).
	static constexpr float kSconceFlameY = 0.712f, kSconceFlameScale = 0.55f;
	static constexpr float kBrazierFlameY = 0.288f, kBrazierFlameScale = 1.0f;
	// The fixture prop submesh(es) + resolved material for the fixture inspector's
	// 3D preview (mesh pointers are stable for the session). `flameHeight` is the
	// local Y of the flame origin above the base, so the preview can place its fire.
	struct FixturePreviewData {
		std::vector<gfx::PreviewSubmesh> subs;
		float scale = 1.0f;
		float flameHeight = kSconceFlameY;
		float flameScale = kSconceFlameScale;
	};
	// A fixture kind's preview (prop mesh(es) + flame origin) for the instance
	// inspector — resolves/loads the kind on demand.
	FixturePreviewData FixturePreviewOf(const std::string& type);
	// Preview submeshes for a placed decoration (by list index) or item (by entity
	// id) — single-mesh or authored multi-material, resolved to mesh+material pairs.
	// Empty if the handle doesn't resolve or has no previewable mesh.
	std::vector<gfx::PreviewSubmesh> DecorationPreviewSubs(int index) const;
	// Items are small/loose, so the preview auto-fits + spins them: this also reports
	// the model-space AABB [fitMin,fitMax] to frame against.
	std::vector<gfx::PreviewSubmesh> ItemPreviewSubs(int entityId, Vec3& fitMin,
													 Vec3& fitMax) const;

	// --- level transitions (P6 multi-level) ---------------------------------
	// Swaps the active level to `stem` and resets all per-level state (map,
	// entities, fog, monster/decoration/fire instances, surface chunks, shadow
	// cache), keeping the shared asset caches (kinds, prop textures) and the
	// party object. The heavy rebuild is NOT done here: the caller re-stages the
	// world's load tasks (AppendLoadTasks) behind a loading screen, then calls
	// PlacePartyAt for the arrival cell. Drains the GPU first. Relies on m_map /
	// m_entities being move-assignable into the existing objects so Party's map
	// reference stays valid.
	// `stashCurrent` saves the level being left into the in-memory per-level
	// store (so returning restores its fog/progress); pass false when the active
	// level is a throwaway baseline (e.g. loading a save onto a different level).
	void BeginLevelLoad(const std::string& stem, bool stashCurrent = true);
	// Places the party at a cell + facing (the stair arrival point), revealing it.
	void PlacePartyAt(int x, int z, Direction facing);
	// Restores the active level's saved dynamic state (fog + entity diffs) from
	// the per-level store, if it was visited before, then drops that entry (the
	// live state is now authoritative). Call after a level's load completes (the
	// entity diffs need the monsters built). A no-op for a first visit.
	void ApplyActiveSnapshot();

	// A pending level transition: the destination level + arrival cell/facing,
	// raised when the party steps onto a stair (see the .map "stairs" records).
	struct LevelTransition {
		std::string level;
		int x = 0, z = 0;
		Direction facing = Direction::South;
	};
	// A pit fall is in progress (the step glide onto the pit, then the camera
	// drop). Movement is swallowed while it runs — the keyboard path gates in
	// Update, the HUD arrow path gates in Game's onMoveAction callback.
	bool Falling() const { return m_pendingFall.has_value(); }

	// Returns and clears a transition raised since the last call (the party
	// stepped onto a stair this frame); nullopt otherwise. Game polls this after
	// the world Update and drives the actual swap (BeginLevelTransition), so the
	// map never changes mid-Update.
	std::optional<LevelTransition> ConsumeLevelTransition();
	size_t MonsterCount() const { return m_monsters.size(); }
	// One human-readable line per monster group (id, count, kinds, cells#slot) for
	// the dev console `groups` command — the Phase-3 group model's reader.
	std::vector<std::string> GroupsReport() const;

	// --- fog of war (dynamic/save-side state, not in DungeonMap) -------------
	// Whether a cell has been revealed (the party has stood on it or an
	// adjacent cell). The map overlay draws only seen cells. This belongs to
	// the dynamic layer — the save serializes it (SaveData::LevelState::seen),
	// never the .map file.
	bool IsSeen(int x, int z) const;

	// --- save / load --------------------------------------------------------
	// Gathers the world's dynamic state into `out`: party pose, torch palette,
	// fog of war (whole), and the per-level entity diff/spawn list (monsters,
	// items, buttons — see SnapshotActive). The character roster is the Game's
	// half, filled separately.
	void CaptureState(SaveData& out) const;
	// Applies a loaded save onto a freshly-built level (call after
	// ResetForNewGame): snaps the party, restores fog + palette, and stages each
	// level's entity diff/spawn list into the per-level store (the active level's
	// is applied by ApplyActiveSnapshot once Game has routed to it).
	void ApplyState(const SaveData& in);

	// --- map editor seam (driven by MapView) --------------------------------
	// Paints a cell to a new type, revealing it and rebuilding the affected
	// surface geometry. Drains the GPU, so it is an interactive edit, not a
	// per-frame call. Whatever the repaint strands is pruned with it — fixtures
	// (DungeonMap::PruneFixturesForCell) and dynamic entities / decorations
	// (PruneEntitiesForCell) — so live state always matches the new grid.
	void EditCell(int x, int z, Cell cell);

	// Which surface a variant edit targets.
	enum class SurfaceSel { Wall, Floor, Ceiling };
	// Pins a floor cell's wall/floor/ceiling texture variant to a palette index
	// (the variant index into the level's surface palette), then rebuilds like
	// EditCell. No-op on solid cells (variants live on floor cells).
	void EditVariant(int x, int z, SurfaceSel sel, int variant);
	// The loaded albedo behind a surface-palette catalog id, for the editor
	// map's textured cell fill. Only the ACTIVE level's palette sets are ever
	// loaded, so the lookup searches those; null for any other id (a browsed
	// level on a foreign palette) — the map falls back to its flat ink.
	const gfx::Texture* SurfaceAlbedoForId(SurfaceSel sel,
										   const std::string& id) const;

	// --- surface palette membership (editor) --------------------------------
	// A level paints only the surface types its `palette` record lists (the
	// order IS the variant index), so a catalog type has to JOIN the palette
	// before the brush can reach it. Appends `id` to the active level's palette
	// and reloads that surface's texture sets + worn block meshes so it paints
	// immediately. False when the id is unknown, already in the palette, or its
	// baked assets are missing (SurfaceAssetsAvailable). Append-only by design:
	// `variant` records key on the INDEX, so an insert would repaint cells.
	bool AddPaletteEntry(SurfaceSel sel, const std::string& id);
	// The browsed-level counterpart: appends to that level's stashed map (no
	// texture work — the level isn't rendered). Same append-only contract.
	bool AddPaletteEntryRemote(const std::string& stem, SurfaceSel sel,
							   const std::string& id);
	// Ensures `id` is in level `stem`'s <sel> palette (appending it, and on the
	// active level reloading its textures) and returns its VARIANT INDEX; -1 if
	// the type's baked assets are missing. Chooses the live map or the stash by
	// whether `stem` is the current level. The editor's surface paint calls this
	// so a "Catalogue"-view type joins the level the moment it is first painted —
	// append-only, so a type already present keeps its index.
	int EnsureSurfaceVariant(const std::string& stem, SurfaceSel sel,
							 const std::string& id);
	// True when the type's baked assets are on disk at the CURRENT quality tier
	// (its texture set and worn_<set>_<tier>.gltf). Guards AddPaletteEntry: the
	// worn-mesh load is a LoadModelOrDie, so an unbaked type would abort the
	// game instead of failing the edit (the MonsterModelAvailable pattern).
	bool SurfaceAssetsAvailable(SurfaceSel sel, const std::string& id) const;
	// The project catalog behind a surface selector (walls/floors/ceilings).
	const Catalog& SurfaceCatalog(SurfaceSel sel) const;

	// --- type rename / delete (editor) --------------------------------------
	// What a sweep found: how many records name the type, and which levels.
	struct TypeUsage {
		int count = 0;
		std::vector<std::string> levels; // stems referencing it, in project order
		bool Any() const { return count > 0; }
	};
	// Walks EVERY level of the project — the live one, the edit stashes, and
	// any not yet in memory (parsed on demand, like a browse) — counting the
	// records that name catalog type `id` of category `catalogKey`. With
	// `newId`, retypes them all instead: that is the rename, and it has to be
	// this exhaustive or a level would load a record naming a type that no
	// longer exists. The caller re-spawns live objects afterwards
	// (RespawnFromRecords) and saves (`savemap`) to persist.
	TypeUsage SweepTypeRefs(const std::string& catalogKey, const std::string& id,
							const std::string* newId = nullptr);
	// Rebuilds the live dynamic objects from the current records — the tail of
	// an undo restore, reused after a type rename retypes those records.
	// Surfaces are untouched (a rename doesn't move geometry) EXCEPT wall
	// features, whose panels are baked into the chunks; those set the geometry
	// dirty flag so the editor's FlushGeometry re-stamps on the way out.
	void RespawnFromRecords(bool geometryToo = false);

	// Live entity placement (editor). type is a catalog id (decorations.cat /
	// monsters.cat). Each instantiates the kind (loading its model/textures on
	// first use — ExecuteImmediate uploads synchronously, so it is safe mid-
	// frame) and appends a runtime instance that draws next frame. Returns false
	// when the cell is solid, the type is unknown, or (monsters) the cell is
	// already occupied. Edits are in-memory only (no .map/.ent write yet).
	bool AddDecoration(const std::string& type, int x, int z, Direction facing);
	// Hangs a `mount = wall` decoration flat on ONE wall of (x,z) — the editor's
	// edge-pick names the face. Mounts like a sconce (offset to the wall, turned
	// into the room), non-solid so the floor stays clear, and round-trips as the
	// record's `wall=` param. False if that neighbour isn't solid.
	bool AddWallDecoration(const std::string& type, int x, int z, Direction wall);
	bool AddMonster(const std::string& type, int x, int z, Direction facing);
	// Places a fixture (sconce/brazier from fixtures.cat — `mount` decides wall vs
	// floor) and rebuilds the fire instances + dust so it lights immediately and
	// persists. Returns false on an invalid cell (e.g. a sconce with no wall).
	bool AddFixture(const std::string& type, int x, int z);
	// Same, but hangs a wall kind on the NAMED face (the editor's edge-pick). A
	// floor kind ignores the face and stands at the cell centre.
	bool AddFixture(const std::string& type, int x, int z, Direction wall);
	// Places a wall niche (wallfeatures.cat) on a free solid wall of (x,z) and
	// re-stamps the cell's wall panel as the recessed niche. False if no free
	// solid wall. Removes it with RemoveNicheAt.
	bool AddNiche(const std::string& type, int x, int z);
	// Same, but carves the NAMED face (the editor's edge-pick), so a corridor's
	// two walls — or a lone block's four — are each addressable. False if that
	// neighbour isn't solid or the face already holds a niche.
	bool AddNiche(const std::string& type, int x, int z, Direction wall);
	// Puts a surface feature (surfacefeatures.cat) on walkable cell (x,z) and
	// re-stamps that surface's block as the feature tile. The TYPE decides which
	// surface (`surface = floor|ceiling`), so a caller never has to. False if the
	// cell isn't walkable or already carries one on that surface. Removed with
	// RemoveFeatureAt, which drops whichever surface has one.
	bool AddSurfaceFeature(const std::string& type, int x, int z);
	bool RemoveFeatureAt(int x, int z);
	// Bores a see-through window (wallfeatures.cat `type`) through solid wall block
	// (x,z) and re-stamps the two flanking faces. False if it isn't a 1-block wall
	// between two spaces.
	bool AddBore(const std::string& type, int x, int z);
	// Same, along an EXPLICIT axis (0 = X, 1 = Z) — the editor derives it from
	// the wall face pointed at, so a free-standing block can be bored either way.
	bool AddBore(const std::string& type, int x, int z, int axis);
	bool RemoveBoreAt(int x, int z);
	// True if solid cell (x,z) can be seen/shot through along `axis` (0=X, 1=Z).
	// AUTHORED BORES ONLY. The Sight spells (SightSpell — Farsight and friends)
	// deliberately do NOT come through here: they are a shader peephole
	// (Renderer's sightHole), so they let the party SEE past a wall without
	// letting line-of-sight or projectiles through it. If a spell should ever
	// open a real hole, a transient set ORed in here is the seam for it — that
	// gains LoS/projectiles without re-baking chunk geometry.
	bool WallSeeThrough(int x, int z, int axis) const;
	// Removes the niche carved into solid wall block (wx,wz) — the erase tool
	// selects niches by their wall, matching the inspector.
	bool RemoveNicheAtWall(int wx, int wz);
	// Removes the niche on ONE named face. Now that faces are individually
	// placeable, a block can hold several niches — this erases the one pointed
	// at instead of whichever RemoveNicheAtWall happens to find first.
	bool RemoveNicheAtFace(int x, int z, Direction wall);
	// Moves the niche at (x,z) from one face to another (the inspector's Face
	// dropdown), carrying any treasure in its pocket — live items and their .ent
	// records — so nothing is stranded on a face with no niche. False if `to`
	// isn't solid, already holds a niche, or no niche sits on `from`.
	bool RemountNiche(int x, int z, Direction from, Direction to);
	// Removes the topmost runtime entity in a cell (a monster first, then a
	// door — live instance + its .ent record — else a decoration). Stair props
	// are skipped — a stair is link + prop + a paired record on another level,
	// so it only goes through RemoveStairAt. Returns true if something was
	// removed.
	bool RemoveEntityAt(int x, int z);

	// Places a door (doors.cat id) on a DOORWAY cell: the travel direction is
	// auto-detected from the flanking walls (exactly one axis must be walled),
	// so the brush needs no facing UI. Authors the .ent record (the writer and
	// the level stash carry it) and spawns the live door closed. Refuses — with
	// a specific onMessage line — when the cell isn't a doorway or is occupied.
	bool AddDoor(const std::string& type, int x, int z);
	// Click interaction: toggles the door on the cell directly AHEAD of the
	// party (arm's reach), sliding the panel open/shut. A monster standing in
	// the doorway jams a closing door. A keyed (key=) closed door opens only
	// while some member carries that item (not consumed; wired buttons bypass
	// the lock — mechanisms don't need the key). False if no door is ahead.
	bool ToggleDoorAhead();
	// True if any party member carries the item (equipment or pack contents).
	bool PartyHasItem(std::string_view typeId) const;

	// Places a button (buttons.cat id) on (x,z), auto-mounted on the cell's
	// first solid wall (refused with a message when the cell has none).
	// Record-backed like doors: the .ent record and the live instance are
	// authored together. Wiring (target=) is edited in the button inspector.
	bool AddButton(const std::string& type, int x, int z);
	// Places a floor item (items.cat id) on (x,z): record-backed like doors and
	// buttons — authors the .ent record and lays the live item in the quarter
	// slot nearest the cell centre. Refused when the cell already holds four
	// items (one per quarter). The party can pick a placed item up in play like
	// any authored one (baseline record + collected save diff).
	bool AddItem(const std::string& type, int x, int z);
	// Click interaction: presses the button on the party's OWN cell mounted on
	// the wall the party faces. False if there isn't one.
	bool PressButtonFacing();
	// Button instance surface for the inspector: presence + wired target, and
	// the live/record edit (target= param; in-memory until savemap).
	bool ButtonSettings(int x, int z, std::string& target) const;
	void SetButtonSettings(int x, int z, const std::string& target);
	// Distinct non-empty door names on the ACTIVE level, for the inspector's
	// Target dropdown (buttons only reach doors on their own level).
	std::vector<std::string> DoorNames() const;
	// Distinct non-empty niche names on the ACTIVE level — the button inspector's
	// Target dropdown lists these alongside door names (a button reveals either).
	std::vector<std::string> NicheNames() const;
	// A button targeting `name` flips every niche with that name open/closed and
	// re-stamps their walls (the secret-niche reveal). False if none matched.
	bool ToggleNichesNamed(const std::string& name);
	// One niche face: its floor cell + the wall it is carved into. A niche is
	// SELECTABLE from either side — clicking its floor cell or the wall block.
	struct NicheFace {
		int x = 0, z = 0;
		Direction wall = Direction::North;
	};
	// Every niche face touching cell (cx,cz): the niches on this floor cell's
	// walls, OR (when cx,cz is a solid wall) the niches on adjacent floor cells
	// carved into it. Each is one selectable target ("Niche — <wall>").
	std::vector<NicheFace> NicheFacesAt(int cx, int cz) const;
	// The niche on (x,z) facing `wall`, or null (the inspector reads its props).
	const WallNiche* NicheOn(int x, int z, Direction wall) const;
	// True if the (x,z)/wall niche exists AND is open (items in it are visible).
	bool NicheOpenAt(int x, int z, Direction wall) const;
	// World position an item sitting in the (x,z)/wall niche renders + pick-tests
	// at (the pocket centre — recessed into the wall, at pocket-floor height).
	Vec3 NicheItemPos(int x, int z, Direction wall) const;
	// Places an item into the (x,z)/wall niche (record-backed, piles at the
	// pocket). False if there is no niche there. Editor placement + in-game drop.
	bool AddNicheItem(const std::string& type, int x, int z, Direction wall);
	// Save the (x,z)/wall niche's authored props (name / hidden / type); Delete it.
	void SetNichePropsAt(int x, int z, Direction wall, const std::string& name,
						 bool hidden, const std::string& type);
	bool RemoveNiche(int x, int z, Direction wall);
	// The button's lever mesh for the inspector's preview pane.
	std::vector<gfx::PreviewSubmesh> ButtonPreviewSubs(int x, int z) const;
	// Live doors for the map overlay (bar markers across the travel axis).
	struct DoorMarker {
		int x = 0, z = 0;
		Direction facing = Direction::South;
		bool open = false;
	};
	std::vector<DoorMarker> DoorMarkers() const;

	// Door instance surface for the editor's inspector. DoorSettings reports
	// the door on (x,z) (false = none); SetDoorSettings applies open/key/name
	// to the LIVE door (slide anim, initialOpen follows — the editor edits the
	// AUTHORED state) and to its .ent record's open=/key=/name= params
	// (in-memory until savemap, like every editor edit). `name` is what a
	// button's target= points at (ToggleDoorsNamed).
	bool DoorSettings(int x, int z, bool& open, std::string& key,
					  std::string& name) const;
	void SetDoorSettings(int x, int z, bool open, const std::string& key,
						 const std::string& name);
	// The door's frame + panel meshes for the inspector's preview pane.
	std::vector<gfx::PreviewSubmesh> DoorPreviewSubs(int x, int z) const;

	// Places a stair on level `stem` (stairs.cat id; its `up` field picks the
	// level above or below in the project's level order) and AUTO-AUTHORS the
	// paired return stair at the same cell of that destination level. Either
	// side lands on the ACTIVE level's live map (prop placed too) when it is
	// the active level, else on the level's in-memory stash (created from the
	// file on first edit; a stash wins over the file on entry, and `savemap`
	// writes every stashed level). Each stair's dest is the other's cell, so
	// the party arrives standing on the counterpart. Validates both cells
	// (walkable, no stair, no brazier) against their authoritative maps and
	// refuses with a specific onMessage line otherwise; all feedback (success
	// too) goes through onMessage.
	bool AddStairAt(const std::string& stem, const std::string& type, int x, int z);
	// Removes the ACTIVE level's stair at (x,z): the link, its prop, and the
	// paired return stair on its destination (live or stash — see
	// RemovePairedStair). False if the cell has no stair. The remote-level
	// counterpart is the stair rung of EraseRemote.
	bool RemoveStairAt(int x, int z);

	// --- remote level editing (the map overlay edits ANY level) --------------
	// Counterparts of the live editing seam for a NON-ACTIVE level `stem`:
	// they operate on the level's in-memory stashes (see m_levelMaps /
	// m_levelEnts) — no geometry or live-instance work, since the level isn't
	// simulated or rendered in 3D. Feedback goes through onMessage like the
	// live seam. `savemap` (SaveAllLevels) persists the stashes.
	void EditCellRemote(const std::string& stem, int x, int z, Cell cell);
	void EditVariantRemote(const std::string& stem, int x, int z, SurfaceSel sel,
						   int variant);
	bool AddDecorationRemote(const std::string& stem, const std::string& type,
							 int x, int z);
	bool AddMonsterRemote(const std::string& stem, const std::string& type,
						  int x, int z);
	bool AddFixtureRemote(const std::string& stem, const std::string& type,
						  int x, int z);
	bool AddNicheRemote(const std::string& stem, const std::string& type, int x,
						int z);
	// Wall-face variants of the three above (the editor's edge-pick, applied to a
	// browsed level's stash rather than the live world).
	bool AddDecorationRemote(const std::string& stem, const std::string& type,
							 int x, int z, Direction wall);
	bool AddFixtureRemote(const std::string& stem, const std::string& type, int x,
						  int z, Direction wall);
	bool AddNicheRemote(const std::string& stem, const std::string& type, int x,
						int z, Direction wall);
	// A floor or ceiling has no face to pick, so there is only the cell form.
	bool AddSurfaceFeatureRemote(const std::string& stem, const std::string& type,
								 int x, int z);
	bool AddDoorRemote(const std::string& stem, const std::string& type, int x,
					   int z);
	bool AddButtonRemote(const std::string& stem, const std::string& type, int x,
						 int z);
	bool AddItemRemote(const std::string& stem, const std::string& type, int x,
					   int z);
	// The erase ladder for a remote cell, mirroring the live tool: stair (pair
	// removed too) → one monster/door/button/item record → one decoration
	// record → fixture → reset the cell's surface variants. Always acts (the
	// last rung is a reset), messaging what it did.
	void EraseRemote(const std::string& stem, int x, int z);

	// Saves every level with unsaved edits: the active one (SaveLevel) plus
	// each stashed level (WriteStashedLevel). Returns the stems written.
	std::vector<std::string> SaveAllLevels();

	// Renames a level's world-side state: moves the .map/.ent files, rekeys
	// the three per-level stashes (+ the active stem), and repoints every
	// stair dest= across all levels (disk-only ones via a lazy stash, so the
	// fix persists on the next savemap). Drops the undo history (its
	// snapshots are keyed by the old stem). The MANIFEST is the owner's:
	// Game updates Project::levels + saves it after this returns true.
	// Existing save files keep the old stem and won't load — dev-cycle cost.
	bool RenameLevel(const std::string& oldStem, const std::string& newStem);

	// --- editor undo/redo (snapshot-based) ------------------------------------
	// One undo step = a full copy of every level's editor-visible state: the
	// active level's map (live decoration placements synced into records), its
	// .ent records, and a dynamic-state snapshot (the same LevelState the
	// level-swap stash uses, so editor-placed monsters and door/button state
	// round-trip), plus copies of every remote-edited level's stashes. Levels
	// are a few KB, so whole-state snapshots beat per-operation inverses —
	// structural paints cascade (fixture/entity/stair-pair pruning) and stair
	// placement spans two levels, and a snapshot restore is correct by
	// construction. MapEditor brackets each brush edit (a drag stroke is ONE
	// step): BeginUndoStep snapshots before the first cell, CommitUndoStep
	// pushes it (clearing the redo stack) or drops a no-op. Undo/Redo restore
	// in place — the active level respawns its dynamic layer from the records
	// + diffs and fully rebakes its geometry (the quality-swap path). The
	// stacks clear on a level transition: a step's active-level snapshot is
	// only meaningful while that level is live.
	void BeginUndoStep();
	void CommitUndoStep(bool changed);
	bool CanUndo() const { return !m_undoStack.empty(); }
	bool CanRedo() const { return !m_redoStack.empty(); }
	void Undo();
	void Redo();
	// Drops both stacks. A level transition does this (a step's snapshot is
	// only meaningful while its level is live), and so does a type RENAME:
	// every held snapshot names the type by its old id.
	void ClearUndoHistory();
	// An undo/redo restore DEFERS the expensive surface rebake: the full-screen
	// editor hides the scene and shadow passes, so the stale chunks are never
	// drawn while it stays up, and repeated undos pay nothing. GeometryDirty
	// reports the debt; FlushGeometry pays it (GPU drain + full rebake + shadow
	// invalidation) — Game calls it when editor mode ends, behind a one-frame
	// "rebuilding geometry" notice. Any full rebake (level load, quality swap)
	// clears the flag itself.
	bool GeometryDirty() const { return m_geometryDirty; }
	void FlushGeometry();

	// A live entity's cell + type, for the map overlay (placed/erased entities
	// show immediately, and the marker can label its type + stack count). Built
	// fresh per call (editor-only, off the per-frame perf path).
	struct MapMarker {
		int x = 0, z = 0;
		std::string type; // catalog id (monster kind / decoration kind)
		Direction facing = Direction::South; // for the editor's facing arrow
		// Baked head-shot icon (monsters), or null — the overlay then falls back
		// to its colored square + type initial.
		const gfx::Texture* icon = nullptr;
		// Whether the editor draws the facing arrow: the type's facing_arrow
		// flag (decorations) / faces= (monsters — a blob never turns anyway).
		bool facingArrow = true;
	};
	std::vector<MapMarker> MonsterMarkers() const;
	std::vector<MapMarker> DecorationMarkers() const;

	// A read-only snapshot of ANOTHER level for the map overlay's up/down level
	// browsing: its static map (the in-session edit stash wins over the file),
	// its .ent baseline (authored spawns — live/dynamic state exists only for
	// the active level), and the fog set stashed when the party last left it
	// (empty = never visited, nothing revealed). Built fresh per switch — two
	// cheap ASCII parses, no GPU work.
	struct LevelBrowse {
		std::string stem;
		DungeonMap map;
		DungeonEntities entities;
		std::vector<u8> seen; // w*h mask like m_seen; empty = nothing revealed
		LevelBrowse(std::string s, DungeonMap m, const std::string& entPath)
			: stem(std::move(s)), map(std::move(m)), entities(entPath, map) {}
		LevelBrowse(std::string s, DungeonMap m, DungeonEntities e)
			: stem(std::move(s)), map(std::move(m)), entities(std::move(e)) {}
	};
	std::unique_ptr<LevelBrowse> BrowseLevel(const std::string& stem);

	// Writes the active level back to the project's .map + .ent files,
	// reconstructing records from the live state (grid + variant overrides +
	// palette/fixtures/stairs + decorations + monsters), so editor edits persist
	// across a relaunch. Returns false if either file could not be written.
	bool SaveLevel() const;

	// --- dev console hooks ---------------------------------------------------
	// "kind @ x,z" for each live monster.
	std::vector<std::string> MonsterList() const;
	// Toggles the activated state of the button in cell (x,z) (no-op if none),
	// returning the new state via `out`. Exercises the button save path until the
	// P5 mechanism wiring drives it from gameplay; the map overlay reflects it.
	bool ToggleButtonAt(int x, int z, bool& out);
	// "id @ x,z = on|off" for each live button (dev console `buttons`).
	std::vector<std::string> ButtonList() const;
	// Point lights submitted this frame (after UpdateLights).
	size_t ActiveLightCount() const { return m_lights.points.size(); }
	// Camera vertical FOV in degrees (clamped); UpdateCamera applies it.
	void SetFov(float degrees);
	float Fov() const { return m_fovDegrees; }
	// Toggle the shadow pass (off = lights still lit, just unshadowed).
	void SetShadowsEnabled(bool on) { m_shadowsEnabled = on; }
	bool ShadowsEnabled() const { return m_shadowsEnabled; }
	// Toggle volumetric dust (off feeds the renderer clear air).
	void SetDustEnabled(bool on) { m_dustEnabled = on; }
	bool DustEnabled() const { return m_dustEnabled; }
	// Live atmosphere tuning for the lighting mood pass (dev console `dust
	// <density>` / `haze <x>` / `ambient <x>`). Design-time knobs: once a feel
	// is chosen, bake the values into the defaults (gfx::Atmosphere, the
	// kBaseAmbient constant) — nothing here persists.
	void SetDustDensity(float d) { m_atmosphere.density = std::max(0.0f, d); }
	float DustDensity() const { return m_atmosphere.density; }
	void SetHazeAmbient(float h) { m_atmosphere.hazeAmbient = std::max(0.0f, h); }
	float HazeAmbient() const { return m_atmosphere.hazeAmbient; }
	void SetAmbientScale(float s); // scales the base ambient fill
	float AmbientScale() const { return m_ambientScale; }
	// Per-level atmosphere (the editor's Level settings dialog, persisted as
	// the .map `atmosphere` record). EffectiveAtmosphere resolves a map's
	// overrides against the global defaults (static: works on a browsed
	// level's snapshot too); SetLevelAtmosphere writes the values — the live
	// map + immediate apply for the active level, the level's stash otherwise
	// (like the other remote edits; savemap persists both).
	static void EffectiveAtmosphere(const DungeonMap& map, float& dust,
									float& haze, float& ambient);
	void SetLevelAtmosphere(const std::string& stem, float dust, float haze,
							float ambient);

	// HUD log feedback (bump lines, monster announcements, palette flavor).
	// Set before play starts; the party/monster callbacks route through it.
	std::function<void(const std::string&)> onMessage;
	// Like onMessage, for lines ABOUT a party member (casting, learning,
	// being struck, going down): carries the member's identity color so the
	// log tints the line in it. MemberMessage routes here, falling back to
	// plain onMessage when unwired.
	std::function<void(const std::string&, const Vec4&)> onMemberMessage;

private:
	// A texture variant set: parallel albedo / normal+height pairs plus the
	// batched mesh bucket per variant.
	// One uploaded geometry chunk: a spatial region's mesh + its texture variant
	// + world AABB, for per-chunk frustum/sphere culling (see DungeonMeshBuilder).
	struct SurfaceChunk {
		int variant = 0;
		int chunk = 0; // spatial chunk index, for region-local edit rebuilds
		std::unique_ptr<gfx::Mesh> mesh;
		Vec3 boundsMin{}, boundsMax{};
	};
	// A surface TYPE's material overrides, the counterpart of DecorationKind's
	// (catalog metallic=/roughness=). -1 = "leave the ORM map authoritative",
	// which is what every hand-authored set wants; a value REPLACES the draw's
	// factor, and with an ORM map the shader multiplies it over the map.
	struct SurfaceMaterial {
		float metallic = -1.0f;
		float roughness = -1.0f;
	};
	struct Surface {
		std::vector<std::unique_ptr<gfx::Texture>> albedo;
		std::vector<std::unique_ptr<gfx::Texture>> normal;
		std::vector<std::unique_ptr<gfx::Texture>> mr; // ORM map (null = none yet)
		std::vector<SurfaceChunk> chunks;              // cullable, tagged by variant
		// Parallax depth PER texture variant (parallel to albedo). Each type's
		// height_scale folded with its `wear` so a flat (wear 0) wall type reads
		// flat — the mesh loses its relief AND the per-pixel parallax goes to 0.
		std::vector<float> heightScale;
		// Material factors per variant, likewise parallel (ApplySurfaceFactors
		// refills them; a short/empty vector simply means no overrides).
		std::vector<SurfaceMaterial> factors;
		// Each variant's texture ASPECT (width/height; 1 for a square scan), read
		// off the loaded albedo — ten installed sets are 2:1 tiles. The worn
		// blocks already carry it in their baked UVs (ModelBaker); this exists for
		// the wall FEATURES, one shared mesh across all 54 surfaces, which can
		// therefore only be corrected where it is stamped (DungeonMeshBuilder).
		std::vector<float> uAspect;
		// Drops the texture variants (keeps the chunks) before a (re)load of the
		// set — the staged loader and the quality hot-swap both reuse the Surface.
		void ResetTextures() {
			albedo.clear();
			normal.clear();
			mr.clear();
			heightScale.clear();
			factors.clear();
			uAspect.clear();
		}
	};

	// A textured material set shared by props (defined in full below); forward-
	// declared here so monster kinds can point at one before the definition.
	struct PropTextures;

	struct MultiMaterialModel; // defined below (shared with decorations/items)

	// Per-kind monster assets (shared) and per-instance state. Kinds are
	// entity type names from the .ent file ("skeleton" loads skeleton.gltf).
	struct MonsterKind {
		assets::ModelData model; // must outlive the Animators pointing into it
		std::unique_ptr<gfx::Mesh> mesh;
		std::string name;
		// PBR set bound by type name (skeleton_<res>, ...); null = flat material.
		const PropTextures* tex = nullptr; // points into m_propTextures (stable)
		// Authored multi-material rig (bought kits: bones/armor/weapons, each
		// with its own embedded glTF textures) — set when the model has >1
		// primitive. The draw, icon bake, and dialog preview loop these subs
		// with the animator palette; `mesh` stays meshes[0] elsewhere.
		std::unique_ptr<MultiMaterialModel> multi;
		// Combat stats from monsters.cat (fallbacks in MonsterKindFor).
		float maxHp = 12.0f;
		float damage = 4.0f;
		float accuracy = 0.65f;
		float evasion = 0.1f;
		float armor = 0.0f; // flat soak (docs/combat.md part 4)
		// The defender side (docs/combat.md part 4): per-type resists
		// (catalog `resists = pierce 0.5, bash -0.5`; a cell of 1.0 =
		// authored immunity) and what this monster's melee deals AS
		// (`dmgtype`, default bash). Ranged/spell attacks type by school.
		ResistTable resists;
		DamageType damageType = DamageType::Bash;
		// What a LANDED melee blow may leave behind (monsters.cat `on_hit =
		// poison 1 20, bleed 2 10 0.5`): effects named by ID, rolled and
		// landed by fx::ApplyProcs. The older one-per-line `poison =` /
		// `bleed =` fields still load, appended as the same procs.
		std::vector<fx::Proc> onHit;
		// Melee reach in cells (Phase 7, catalog `reach`): 1 = must be in the
		// adjacent ring; 2 = a pike — melees from its QUEUE post down a clear
		// shared row/column (the monster mirror of the party's rear-rank
		// polearm rule). Ranged/caster types don't read it.
		int reach = 1;
		float attackInterval = 1.6f; // seconds between swings
		float aggroRange = 6.0f;     // cells of party distance to engage at
		float moveInterval = 0.6f;   // seconds per grid step while chasing
		// How clever the monster is: drives how OFTEN it re-decides (the AI tick
		// bucket), not what it decides. Higher iq -> a faster bucket (thinks more
		// often); see ai::Scheduler::BucketForIq. Default ~100 = middle of the pack.
		float iq = 100.0f;
		// Behaviour strategy (monsters.cat `archetype`): brute closes to melee (the
		// default, unchanged); skirmisher holds `keepRange` cells away and shoots.
		// Drives the AI intent (Engage vs Kite) and which host executor runs.
		ai::Archetype archetype = ai::Archetype::Brute;
		float keepRange = 4.0f;      // skirmisher/caster: cells of party distance to hold
		float fleeBelow = 0.0f;      // flees when hp/maxHp drops below this (0 = never)
		std::string spell;           // caster: spells.cat id its bolt casts (empty = a
									 // plain bolt, e.g. a skirmisher's arrow)
		// Per-type threat shading (monsters.cat threat_*): multipliers on the
		// balance.cat globals, 1 = the global as-is (see Balance.h ThreatTuning).
		ThreatTuning threatTuning;
		// Behaviour/appearance, data-driven from the catalog so AI and the
		// flat-material fallback never branch on the type name.
		bool facesTarget = true;     // turn to face the party once engaged
		// (radially-symmetric models like the blob set faces=false to skip it)
		float fallbackRoughness = 0.9f; // flat-material roughness when no PBR set
		// Render-only orientation/size fixups for imported models that don't ship
		// in the engine's convention (e.g. a Mixamo-rigged asset facing the wrong
		// way). modelyaw is radians added to the facing rotation; modelscale is a
		// uniform visual scale about the model's foot. Both default to no-op.
		float modelYaw = 0.0f;
		float modelScale = 1.0f;
		// Sub-cell occupancy (monsters.cat `size=`, default large). Decides the
		// monster's footprint + how many share a cell — see Game/SlotGrid.h.
		SizeClass size = SizeClass::Large;
		// Data-driven animation table: for each CreatureState, the candidate clip
		// NAMES to play (a variation is chosen at random when the state is entered).
		// An empty list = the state is unauthored for this kind, so the resolution
		// ladder (DesiredState) falls through to a simpler one. Filled from the
		// monsters.cat `anim_<state>` rows; a state with no row defaults to a clip
		// named after the state itself when the model ships it (a plain idle/walk/
		// attack/die rig needs zero config). See Animation/CreatureState.h.
		std::array<std::vector<std::string>, anim::kCreatureStateCount> animClips;
		// Which states this kind can be in — the explicit `states = ...` catalog
		// row. The resolution ladder (DesiredState) only resolves to a supported
		// state, falling through to a simpler one otherwise (a mindless blob with no
		// `alert` just stays idle when it notices the party). With no `states` row it
		// falls back to "supported iff the state has clips" (back-compat). Idle is
		// always on (the rest pose); Die is always considered on death regardless.
		std::array<bool, anim::kCreatureStateCount> stateSupported{};
		// Baked head-shot icon for the map overlay (a skull for the skeleton, the
		// slime's dome, ...): the kind's mesh rendered once into a small RT,
		// framed on the model's upper portion (UpdateMonsterIcons). Data-driven —
		// every kind gets one from its own model, no authored 2D art. Starts
		// transparent until the bake runs.
		std::unique_ptr<gfx::Texture> iconTarget;
	};
	struct Monster {
		const MonsterKind* kind = nullptr; // points into m_monsterKinds (stable)
		int id = -1; // source Entity::id, for save overrides
		u32 runtimeId = 0; // STABLE per-session id (never reused) that async AI plans
						   // key off, so a plan always finds the right monster
						   // regardless of vector reordering/erasure (0 = unassigned)
		// Logical GROUP this monster belongs to: monsters currently sharing a cell.
		// Recomputed each frame by ReconcileGroups (merge when together, split when
		// apart); not saved (the id numbers are opaque, re-derived every frame). The
		// substrate for formation behaviour — gates lone front-centre vs in-cell
		// reposition (Phases 4-5).
		u32 groupId = 0;
		int x, z;
		// Sub-cell slot within (x,z) on the size's slot grid (Game/SlotGrid.h);
		// 0 = the only slot for Large/Huge. visualPos glides to SlotCenter, not
		// CellCenter. Initial slot derived by fill order; PERSISTED (Phase 3) so a
		// monster's exact stance within a cell survives save/reload.
		int slot = 0;
		int spawnX = 0, spawnZ = 0; // .ent baseline, for the save diff
		// Per-INSTANCE AI overrides from the .ent record (authored, not saved — the
		// .ent is their source; the editor inspector edits them + the .ent writer
		// round-trips them). asleep: starts dormant, waking only when the party comes
		// very close or it is hit (a per-placement lurker). leash: cells from the
		// anchor (leashX/Z, default the spawn cell) it will chase before breaking off
		// and returning; 0 = unleashed. patrol: a waypoint route walked when idle
		// (P3b; empty = none).
		bool asleep = false;
		int leashX = 0, leashZ = 0;
		float leashRange = 0.0f;
		std::vector<ai::Cell> patrol;
		size_t patrolIdx = 0; // next waypoint to walk toward (transient, wraps the route)
		// Per-instance BEHAVIOUR overrides (.ent archetype/keeprange/fleebelow/spell).
		// Absent = inherit the type default (so editing the type still updates every
		// un-overridden instance live); the inspector sets them, the .ent stores them.
		std::optional<ai::Archetype> archOverride;
		std::optional<float> keepOverride;
		std::optional<float> fleeOverride;
		std::optional<std::string> spellOverride;
		// Effective behaviour = the override if set, else the kind's default.
		ai::Archetype Archetype() const { return archOverride ? *archOverride : kind->archetype; }
		float KeepRange() const { return keepOverride ? *keepOverride : kind->keepRange; }
		float FleeBelow() const { return fleeOverride ? *fleeOverride : kind->fleeBelow; }
		const std::string& Spell() const { return spellOverride ? *spellOverride : kind->spell; }
		float yaw = 0.0f;         // current visual facing (eased toward targetYaw)
		float targetYaw = 0.0f;   // desired facing: travel direction, or the party
		Direction facing = Direction::South; // for the .ent writer
		bool announced = false;
		// Has noticed the party (sticky): set when the brain first engages via the
		// sight cone, or immediately on a hit (provoke). Once aware the monster
		// stays engaged even if the party slips behind it. Saved (dynamic state).
		bool aware = false;
		// Threat (aggro): damage each roster member has dealt this monster,
		// scaled by balance threat_scale and draining threat_decay/second. Once
		// a member crosses threat_threshold the monster LOCKS onto the highest
		// (threatLock = roster index; sticky — another member must EXCEED the
		// locked score by threat_switch to steal it); with nobody above the
		// threshold targeting stays uniform-random. Saved (v19).
		std::array<float, 4> threat{};
		int threatLock = -1;
		bool ThreatAny() const {
			return threat[0] > 0.0f || threat[1] > 0.0f || threat[2] > 0.0f ||
				   threat[3] > 0.0f;
		}
		float hp = 1.0f;          // current hit points (maxHp at spawn)
		float attackCd = 0.0f;    // seconds until this monster can swing again

		// Status effects riding this monster — the SAME list a Character
		// carries, holding the same fx::Inst (docs/effects.md decision 2: full
		// symmetry). A burn lives here now rather than in fields of its own,
		// which is what lets a monster be poisoned, chilled or warded without
		// another slot being invented for each. Not saved yet (P5).
		std::vector<fx::Inst> effects;

		// The flame plume rising off a burning body — PRESENTATION, derived
		// from the list above every frame (SyncPlumes): allocated when an
		// effect that burns arrives, dropped when it goes. Not state: the
		// effect is the truth, this is just what it looks like. Held by
		// pointer because a FireEffect is fat (its own mt19937) and only the
		// handful actually alight should pay for one.
		std::unique_ptr<FireEffect> plume;

		// Chase movement (AI v1). The logical cell (x,z) snaps the instant a step
		// commits — like the party — so occupancy/blocking is atomic; visualPos
		// glides from moveFrom to the new cell centre over moveInterval. moveCd
		// gates the next step. Set visualPos = SlotCenter(x,z,size,slot) at spawn/load.
		Vec3 visualPos{};
		Vec3 moveFrom{};
		float moveT = 0.0f;     // 0..1 tween progress while moving
		float moveCd = 0.0f;    // seconds until the next step is allowed
		bool moving = false;
		anim::Animator animator;

		// Data-driven animation state (DriveMonsterAnim): DesiredState maps live sim
		// onto a CreatureState each frame, then the kind's animClips table resolves
		// that to a clip name (with variations). animState is the one currently
		// playing; the timers count down the active one-shot (a state holds until
		// its timer elapses, then the ladder falls through). The *Req flags are
		// momentary EVENT triggers (a swing launched / a blow landed) that bridge an
		// instantaneous event into a held visual state — set by the host, consumed
		// (cleared) by DriveMonsterAnim. deathAnim doubles as the corpse-held timer
		// (0 = gone, or no die clip → vanishes instantly as before).
		// Cosmetic/transient — not saved (a reloaded corpse just replays its death).
		anim::CreatureState animState = anim::CreatureState::Idle;
		float spawnAnim = 0.0f;  // rise/appear one-shot remaining
		float attackAnim = 0.0f; // swing one-shot remaining
		float hitAnim = 0.0f;    // flinch one-shot remaining
		float deathAnim = 0.0f;  // corpse drawn + animating while the death clip plays
		bool spawnReq = true;    // play a spawn clip on first frame (if the kind has one)
		bool attackReq = false;  // a swing was launched this step
		bool hitReq = false;     // took a (non-fatal) blow this step

		// Formation target (Phase 5): the attack cell around the party this monster
		// is assigned to (or the party cell when unassigned/queuing). Set each frame
		// by AssignFormation, fed into the AI snapshot so the brain paths here.
		// Transient — re-derived every frame, never saved.
		int targetX = 0, targetZ = 0;

		// Standing orders from the async brain (Game/MonsterAI.h). The worker
		// threads refresh intent + aiPath at this monster's IQ-bucket cadence; the
		// main thread executes them every frame, popping aiPath at aiCursor and
		// validating each cell against live occupancy. Transient AI state — not
		// saved; the next plan rebuilds it within a bucket period.
		ai::Intent intent;
		std::vector<ai::Cell> aiPath; // cached chase route (start cell excluded)
		size_t aiCursor = 0;          // next unstepped cell in aiPath

		float MaxHp() const { return kind ? kind->maxHp : 1.0f; }
		bool Alive() const { return hp > 0.0f; }
	};

	// Per-kind item behaviour (shared) and per-instance world state. Items carry a
	// CATEGORY (rune|weapon|armor|clothing|food|misc), a carry WEIGHT, and a list
	// of hand COMMANDS (the right-click menu builds from these). RUNES are the
	// fully-built specialization — a carved-stone tablet (the shared m_runeMesh,
	// drawn with this element's texture set) the party picks up by clicking it;
	// An authored model rendered with its OWN glTF materials: one GPU texture per
	// embedded image and one submesh (mesh + resolved MaterialParams) per glTF
	// primitive, so a multi-material model (a dagger's steel blade, brass guard,
	// leather grip) draws each part with its real material instead of one flat set.
	// MaterialParams holds raw Texture* into `textures`, which is built once and
	// never resized, so those pointers stay valid for the model's lifetime. Shared
	// by decorations, items (floor + icon), and the icon bake.
	struct MultiMaterialModel {
		std::vector<std::unique_ptr<gfx::Texture>> textures; // one per model.images
		struct Sub {
			std::unique_ptr<gfx::Mesh> mesh;
			gfx::MaterialParams material;
		};
		std::vector<Sub> subs; // one per model.meshes
		Vec3 boundsMin{}, boundsMax{}; // world-space AABB of the baked geometry
		// Grounded height (min y -> 0) and the y-offset that grounds the model.
		// One source for "where it sits / how tall it is", so the floor draw and
		// the pick test can't disagree (and a non-grounded .glb still sits right).
		float Height() const { return boundsMax.y - boundsMin.y; }
		float GroundOffsetY() const { return -boundsMin.y; }
	};
	// they implicitly get the "memorize" command. Other categories so far reuse
	// the tablet mesh, tinted, as a placeholder (see ItemKindFor).
	struct ItemKind {
		std::string id;          // catalog id (the .ent record type)
		std::string nameKey;     // loc key for the display name ("item.rune_fire")
		std::string category;    // rune|weapon|armor|clothing|food|misc (free-form)
		std::string skill;       // weapon class this item trains/uses (catalog
								 // `skill`, docs/skills.md); "" = untrained swing
		// Weapon stats (docs/combat.md Phase 1). 0 = unstated: the swing falls
		// back to the attacker's unarmed numbers (the unarmed_* knobs), so
		// non-weapon holdables swing unchanged.
		float damage = 0.0f;     // base damage of a clean hit with this weapon
		float speed = 0.0f;      // seconds between swings (before dexterity)
		// The associated stats (docs/combat.md part 2, catalog `stats = str,
		// dex`): their average is the attack bonus input AND what trains on a
		// landed blow. Empty = the unarmed default (strength).
		std::vector<std::string> stats;
		// Weapon reach (Phase 7, catalog `reach = polearm`): a polearm swings
		// from the party's REAR rank (roster slots 2-3); everything else —
		// bare hands included — is front-rank only.
		bool polearm = false;
		// ENCHANTMENT (the fire sword, catalog `element = fire`): the weapon
		// carries a school's element into every LANDED blow, on top of the
		// physical damage — `element_bonus` of the blow's assembled damage as
		// that element, through the target's resist for it. enchanted=false
		// (no `element` line) = an ordinary weapon, the term skipped.
		bool enchanted = false;
		SpellSymbol element = SpellSymbol::Fire;
		float elementBonus = 0.0f;
		// What a landed blow leaves behind (`on_hit = burn 3 6 0.5`), the same
		// authored form a monster uses. An enchanted weapon lends its element
		// as the effects' flavour, so the SAME `on_hit = burn` reads as fire on
		// one blade and as a freezing burn on another. The older `element_dot`
		// line still loads, appended as an `on_hit = burn ...` proc.
		std::vector<fx::Proc> onHit;
		// The defender side of a WORN piece (part 4): per-type resist cells
		// plus a small flat soak, summed across the equipment slots.
		ResistTable resists;
		float armor = 0.0f;
		float weight = 0.0f;     // carry weight (kg); sums into a member's load
		std::vector<std::string> commands; // hand right-click command ids (data-driven)
		bool isRune = false;
		// Uniform size trim (items.cat `scale`) over the model's authored unit
		// size — the DecorationKind knob, for floor/niche draws. 1 = as authored.
		float modelScale = 1.0f;
		SpellSymbol runeSymbol = SpellSymbol::Fire;
		Vec4 glow{1, 1, 1, 1};   // accent-glow tint (element colour / category tint)
		// Carved-stone tablet look: the shared tablet mesh (m_runeMesh) drawn with
		// this element's PBR set (rune_<elem>). null tex falls back to flat stone.
		const PropTextures* tex = nullptr;
		// Authored multi-material model (catalog `model`): when set, the item draws
		// as this on the floor and its baked 3D thumbnail is the icon/cursor. null =
		// the tablet+tint placeholder above.
		std::unique_ptr<MultiMaterialModel> model;
		// Baked 3D icon render-target (one per type, owned here; reused by every
		// slot/grid/cursor instance via the icon bank). Null for placeholder items;
		// transparent until UpdateItemIcons renders into it.
		std::unique_ptr<gfx::Texture> iconTarget;
		// Catalog `icon_spin`: re-bake this icon every frame on a turntable spin
		// (vs the static shape-aware pose). The cursor + every slot animate with it.
		bool iconAnimated = false;
	};
	struct Item {
		const ItemKind* kind = nullptr; // points into m_itemKinds (stable)
		int id = -1;                    // source Entity::id (>= 0 = .ent baseline)
		int x = 0, z = 0;
		bool collected = false; // picked up — hidden + saved so it stays gone
		// Sub-cell quarter (Medium 2x2 slot, 0..3) the tablet rests in — a dropped
		// item snaps to the quarter nearest the cursor; up to 4 share a cell. Render
		// + pick + the glow light use SlotCenter(x,z,Medium,slot). See SlotGrid.h.
		int slot = 0;
		// The wall NICHE this item sits in (Direction index; -1 = an ordinary floor
		// item). Niche items pile at the pocket centre (NicheItemPos), ignore `slot`,
		// and are hidden + unpickable while the niche is closed.
		int niche = -1;
	};

	// A wall-mounted button/lever (EntityKind::Button from the .ent layer). The
	// runtime carries the one bit that can change in play — `activated` — plus its
	// wiring (`target`, an id another entity reads) so the save layer can diff it
	// by `id` like a monster. The interaction itself (what a target does) is the
	// P5 door/mechanism work; this is the persistent state it will toggle. Buttons
	// have no model of their own yet (the map overlay marks them).
	struct DecorationKind; // declared with the decoration machinery below

	struct Button {
		int id = -1;                         // source Entity::id (.ent baseline)
		int x = 0, z = 0;                    // the cell it mounts in
		Direction facing = Direction::South; // the solid wall it faces
		std::string target;                  // wired door name (target= param)
		bool activated = false;              // pressed / toggled on (saved)
		// The lever, in TWO parts (buttons.cat), wall-mounted at hand height.
		// `kind` is the HANDLE and the render tilts it by `activated`; `plate`
		// is the static mount ([lever_plate]) drawn without the tilt, because a
		// plate bolted to stone does not move — one mesh for both rocked it in
		// and out of the wall. The door's frame/panel pair does the same thing.
		// Splitting them also gives each its own texture: stone mount, wooden
		// handle. Either may be null for a type the catalog doesn't know
		// (hand-authored legacy records) — such a button works but is invisible.
		const DecorationKind* kind = nullptr;
		const DecorationKind* plate = nullptr;
	};

	// A door filling a doorway cell (side walls flank the travel axis). Closed
	// it blocks the party, monsters, and projectiles; the panel slides sideways
	// into the wall as it opens (openT animates toward `open`). `facing` is the
	// travel direction through it; `name` is what buttons target (name= param).
	// Open-state diffs ride the save like button toggles (kind Door, activated).
	struct Door {
		int id = -1;                         // source Entity::id (.ent record)
		int x = 0, z = 0;
		Direction facing = Direction::South; // travel axis (panel spans the other)
		std::string name;                    // button-target id ("" = unwired)
		std::string key;                     // item id that unlocks it ("" = none)
		bool open = false;
		bool initialOpen = false;            // authored state (open= param)
		float openT = 0.0f;                  // slide anim, 0 closed .. 1 open
		const DecorationKind* panel = nullptr;
		const DecorationKind* frame = nullptr;
	};

	// Static architecture decorations from the .map layer (column, archway,
	// fountain, statue, barrel, ...). One shared model+mesh per type; instances
	// are placed and oriented once at load and never move or animate, so they
	// carry no per-frame state and stay out of the save (static = .map only).
	// A textured material set shared by props (loaded once per set name). Mirrors
	// Surface but single-variant: albedo (sRGB) + normal/height + ORM, linear.
	struct PropTextures {
		std::unique_ptr<gfx::Texture> albedo, normal, mr;
		float heightScale = 0.0f;
	};
	struct DecorationKind {
		assets::ModelData model; // kept alive for the shared mesh
		std::unique_ptr<gfx::Mesh> mesh;
		Vec4 color{1, 1, 1, 1};
		const PropTextures* tex = nullptr; // points into m_propTextures (stable)
		// Authored multi-material models render their own glTF textures instead of
		// a single bound set; non-null replaces the mesh/tex/color path above.
		std::unique_ptr<MultiMaterialModel> multi;
		std::string id;            // catalog id (the record type), for the writer
		bool authored = false;     // imported model: consistently wound -> back-cull
		bool solidDefault = true;  // floor-standing blocks the party (passages don't)
		float alphaCutoff = 0.0f;  // > 0: alpha-test cutout (masked set, e.g. a gate)
		// Whether the editor map draws the green facing arrow on instances of
		// this type (catalog `facing_arrow`, default 1). Radially symmetric
		// props — columns, pots, boulders — turn it off; the inspector's
		// checkbox beside the Facing dropdown edits it per type.
		bool facingArrow = true;
		// Baked whole-model map icon (like MonsterKind's head shot, but props
		// read best in full). Transparent until UpdateMapIcons bakes it.
		std::unique_ptr<gfx::Texture> iconTarget;
		// Catalog material overrides (the asset dialog's sliders persist here as
		// metallic=/roughness=/height_scale=/color= fields; absent = -1/untinted =
		// leave the resolved material alone). metallic/roughness REPLACE the draw's
		// factors — with an ORM map the shader multiplies them over the map, flat
		// fallbacks take them directly. tint replaces baseColor (over the albedo).
		float metallic = -1.0f;
		float roughness = -1.0f;
		float heightScale = -1.0f;
		bool hasTint = false;
		Vec4 tint{1, 1, 1, 1};
		// Uniform size trim (catalog `scale`) applied ON TOP of the model's
		// authored unit size, so a prop can be nudged without re-exporting it
		// from Blender. 1 = as authored. The same knob monsters have had as
		// `modelscale`; multiplied into UnitScale at every draw.
		float modelScale = 1.0f;
		// World-space cull radius: the farthest vertex from the model's OWN
		// ORIGIN, carried through kUnit and modelScale (DecorationKindFor).
		// Origin-centred rather than AABB-centred, so an instance's translation
		// is a valid sphere centre under any yaw the placement applied — a
		// little conservative, correct by construction. This replaced a
		// hardcoded 0.85-unit sphere that silently clipped the edges off
		// anything larger than one cell, which is exactly what the composed
		// architecture (multi-cell arches, ceiling vaults) is made of.
		float cullRadius = 0.0f;
	};
	struct Decoration {
		const DecorationKind* kind = nullptr; // points into m_decorationKinds
		Mat4 world;                           // baked transform (cell + facing)
		int x = 0, z = 0;
		bool solid = true; // blocks the party (passages like archways do not)
		// Record-level fields, kept so the editor can write the .map back faithfully.
		Direction facing = Direction::South;
		bool wallMounted = false;        // hung on a wall (wall= record param)
		Direction wall = Direction::North;
		bool stair = false;              // a stair prop (written as a stairs record)
	};

	// Fires: wall sconces (at 'T' cells, mounted on the adjacent wall) and
	// floor braziers ('F' cells). Each carries a flickering point light at
	// its flame origin and a FireEffect particle simulation.
	struct FixtureKind; // defined with the fixture members below

	struct Fire {
		const FixtureKind* kind = nullptr; // resolved catalog id (mesh/tex/flame)
		bool brazier = false;    // floor-standing (light params branch on this)
		bool lit = true;         // false: prop still drawn, but no light/flame/smoke
		float lightRadius = 7.0f; // point-light reach in metres (sconce brightness * cell)
		Mat4 world;        // prop transform
		Vec3 flamePos;     // particle + light origin
		float phase = 0;   // flicker phase
		FireEffect effect;
	};

	// --- loading ---------------------------------------------------------------
	// The project's first level stem (the level the game opens). A static member
	// because both the ctor (DungeonWorld.cpp) and AppendLoadTasks
	// (DungeonWorld_Load.cpp) resolve it.
	static std::string FirstLevel(const Project& project);
	// One surface's texture sets + the height scale its parallax uses. The three
	// SurfaceDefs (walls/floors/ceilings) point at the resolved palettes
	// (m_wallSets/...) and scales filled by ResolveSurfacePalettes, shared by the
	// staged loader and the quality hot-swap.
	struct SurfaceDef {
		Surface& surface;
		std::span<const std::string> names;
		std::span<const float> heights; // per-variant parallax depth (× wear)
		std::span<const SurfaceMaterial> factors; // per-variant metallic/roughness
	};
	std::array<SurfaceDef, 3> SurfaceDefs();
	// Copies the resolved per-variant factors into each Surface. Called from
	// BuildDungeonMeshes (every load / quality swap / restore path runs through
	// it) and by RefreshSurfaceMaterials for a live catalog edit.
	void ApplySurfaceFactors();
	// Resolves the map's palette ids (DungeonMap::WallPalette etc.) through the
	// project's surface catalogs into texture set names + per-surface height
	// scales. Called once at construction; the results drive both the texture
	// load and the worn block mesh names (worn_<set>_<tier>.gltf).
	void ResolveSurfacePalettes();

	// One PBR material set's three maps (sRGB albedo + linear normal/height +
	// ORM), the shared result of LoadPbrSet — surfaces append it into their
	// variant arrays, props copy it into a PropTextures.
	struct PbrMaps {
		std::unique_ptr<gfx::Texture> albedo, normal, mr;
	};
	// Loads a PBR set by base name at the current quality tier, falling back to
	// the always-present 2k set. `required` (surfaces) dies if even the albedo is
	// missing; otherwise (props) returns maps with a null albedo so the caller
	// keeps its flat material. The single source of the res→2k fallback.
	PbrMaps LoadPbrSet(const std::string& name, bool required);

	void LoadDungeonBlocks();      // loads the worn block set for the quality tier
	void LoadSurfaceMaterial(Surface& surface, const std::string& name,
							 float heightScale);
	void LoadTextureSet(const SurfaceDef& def); // resets, then loads the set
	void LoadAllSurfaceTextures(); // reloads every set (quality hot-swap)
	void BuildDungeonMeshes();
	void LoadMonsters();
	void LoadItems(); // instantiates EntityKind::Item records (runes) from .ent
	void LoadButtons(); // instantiates EntityKind::Button records from .ent
	void LoadDoors();   // instantiates EntityKind::Door records from .ent
	// Builds one live Door from its record (kinds lazily loaded via the doors
	// catalog: the type's entry = the panel, [door_frame] = the shared frame).
	void SpawnDoor(const Entity& record);
	Door* DoorAt(int x, int z);
	const Door* DoorAt(int x, int z) const;
	// The travel direction for a door on (x,z): exactly ONE axis must be
	// flanked by solid walls (the panel spans it); false = no doorway there.
	static bool DoorwayFacing(const DungeonMap& map, int x, int z, Direction& out);
	// Toggles one door (with the doorway-occupied jam check + message/anim) /
	// every door whose name matches a button's target.
	bool ToggleDoor(Door& door);
	void ToggleDoorsNamed(const std::string& name);
	// Lazily loads (and caches) the shared behaviour for an item type, resolved
	// through the items catalog (category=rune → symbol + element glow colour).
	ItemKind& ItemKindFor(const std::string& type);
	// Renders a soft round halo (sprites) + one model's submeshes into an icon
	// render-target (fit to bounds, flat face to camera, 3/4 view). Shared depth
	// target; the bake list redirects the OM.
	void BakeIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
				  const MultiMaterialModel& model, const gfx::Texture& target,
				  bool animated, float spin);
	// Draws every submesh of an authored multi-material model at `world`, each with
	// its own glTF material. Shared by decorations, floor items, and the icon bake.
	void DrawMultiMaterial(ID3D12GraphicsCommandList* list,
						   const MultiMaterialModel& model, const Mat4& world);
	// Builds one monster instance (kind/id/cell/facing → stats + animator) ready
	// to push into m_monsters. Shared by the initial .ent load, live editor
	// placement, and save restore of editor-placed monsters. The caller pushes.
	Monster MakeMonster(MonsterKind& kind, int id, int x, int z, Direction facing);
	// Re-derive monster groups from CURRENT co-location: all monsters sharing a
	// cell get one groupId (merge), monsters in different cells are different groups
	// (split). Recomputed each frame (top of UpdateMonsters) so groups track who is
	// actually together. (Phase 3 introduced groups by spawn cell; Phase 5 made them
	// dynamic so a swarm merges/splits — see docs/movement.md.)
	void ReconcileGroups();
	// Count of live monsters in a group (Phase 4: gates lone front-centre + the
	// grouped front-slot reposition).
	int AliveInGroup(u32 group) const;
	// Formation pass (Phase 5): assign each AWARE monster a target attack cell
	// (a walkable orthogonal neighbour of the party), spreading them around the
	// party (surround) before doubling up a side; overflow / not-yet-aware target
	// the party cell. Sets Monster.targetX/targetZ; called before BuildAISnapshot.
	void AssignFormation();
	// The world point a settled monster wants WITHIN its current cell (Phase 4):
	// the front-centre toward the party for a lone Medium-or-smaller monster, else
	// its slot centre. partyPos is the party's cell centre.
	Vec3 DesiredAnchor(const Monster& m, const Vec3& partyPos) const;
	// Where a step's glide lands in the monster's CURRENT cell: a LONE
	// sub-cell monster that isn't engaging the party (intent Idle —
	// wandering / patrolling / asleep) walks the CENTRE of each square (a
	// solo creature hugging one quarter of every cell reads wrong); anything
	// grouped, engaged, or cell-filling keeps its slot centre. Shared by the
	// move tween and DesiredAnchor's settled fallback.
	Vec3 MonsterStepTarget(const Monster& m) const;
	void LoadDecorations();
	void LoadStairs(); // places stair props (P6) from the map's stair links
	// Instantiates one stair link's prop (a non-solid decoration flagged stair).
	// Shared by LoadStairs and the editor's live placement (AddStair).
	void PlaceStairProp(const StairLink& link);
	// Lazily loads (and caches) the shared assets for a monster / decoration
	// type (model + mesh + PBR set), resolved through `catalog` (decorations.cat
	// for props, stairs.cat for stair props). Shared by the initial load and live
	// editor placement.
	MonsterKind& MonsterKindFor(const std::string& type);
	DecorationKind& DecorationKindFor(const std::string& type, const Catalog& catalog);
	// World-space mount for a prop hung flat against the `wall` of cell (x,z):
	// origin pushed to the wall face, +Z (authored front) turned to face the
	// room. Shared by wall sconces and wall-mounted decorations.
	struct WallMount {
		Vec3 pos;
		float yaw;
	};
	WallMount MountOnWall(int x, int z, Direction wall) const;
	// Loads (once, cached in m_propTextures) a prop PBR set by name: sRGB albedo
	// + linear normal/height + ORM, with the same res→2k fallback as surfaces.
	// Returns null only if even the 2k albedo is missing.
	const PropTextures* LoadPropTextures(const std::string& set);
	// Binds an albedo+normal+ORM trio onto a material (factors at 1.0 so the ORM
	// drives metallic/roughness per-texel), or a flat color + roughness fallback
	// when there is no albedo. The shared core of every textured draw — props and
	// the per-variant surfaces both route through it.
	static void ApplyPbr(gfx::MaterialParams& m, const gfx::Texture* albedo,
						 const gfx::Texture* normal, const gfx::Texture* mr,
						 float heightScale, const Vec4& fallbackColor,
						 float fallbackRoughness);
	// Fills a draw's material from a prop set (albedo + bump/parallax + ORM), or
	// a flat color + roughness when the set is missing. Shared by every textured
	// prop draw (decorations, fires, monsters).
	static void ApplyPropMaterial(gfx::MaterialParams& m, const PropTextures* tex,
								  const Vec4& fallbackColor, float fallbackRoughness);
	// The DecorationKind flavour: the set/color fill above plus the kind's catalog
	// material overrides (metallic=/roughness=/height_scale=/color=). Every draw
	// of a single-mesh catalog prop (decoration/door/button/stair + previews)
	// routes through this so the overrides apply everywhere alike.
	static void ApplyPropMaterial(gfx::MaterialParams& m, const DecorationKind& kind,
								  float fallbackRoughness);
	// Fixture routing info for DungeonMap's parser (see the declaration below).
	// Routing info for DungeonMap's fixture-record parser, derived from the
	// project's fixtures catalog (wall-mount ids + the glyph default ids).
	// Passed at every DungeonMap construction.
	static FixtureTypes FixtureTypesOf(const Project& project);
	// Builds an authored model's own GPU resources (one texture per embedded glTF
	// image, one submesh per primitive with its material) for the multi-material
	// decoration path.
	static std::unique_ptr<MultiMaterialModel> BuildMultiMaterialModel(
		gfx::GraphicsDevice& device, const assets::ModelData& model);
	// Bakes the entry's metallic=/roughness=/color= overrides into an authored
	// model's per-submesh materials (the multi-material twin of the DecorationKind
	// ApplyPropMaterial overload).
	static void BakeCatalogMaterial(MultiMaterialModel& model,
									const CatalogEntry* def);
	void BuildFires();
	void BuildTurbidityMap();
	void RebuildFiresAndDust(); // WaitIdle + rebuild fires + dust (live sconce edits)
	// A structural repaint strands whatever occupied the cell: painted solid ⇒
	// remove the monsters/items/buttons/decorations (and their .ent records)
	// standing on it; painted open ⇒ re-mount or drop the neighbouring buttons /
	// wall decorations that hung on it. The dynamic-layer sibling of DungeonMap::
	// PruneFixturesForCell — keeps live state consistent with the grid so
	// SaveLevel and the level stash never persist a record the loaders reject.
	void PruneEntitiesForCell(int x, int z);

	// --- per-frame --------------------------------------------------------------
	void UpdateCamera();
	void UpdateLights(float time);
	void UpdateMonsters(float dt);
	// One monster's melee strike against a standing party member (called from
	// UpdateMonsters when the monster is adjacent and off cooldown). The victim
	// comes from PickMeleeVictim — threat-driven with the near-row blocking rule,
	// uniform-random below the threat threshold.
	void MonsterAttack(Monster& monster);
	// Per-frame clip state machine: resolves the monster's CreatureState from its
	// live state (DesiredState), looks the state up in the kind's animClips table
	// (a variation chosen at random), cross-fades on a change, then advances the
	// animator. Runs for downed monsters too, so the death clip plays out.
	void DriveMonsterAnim(Monster& monster, float dt);
	// The ladder from live simulation to a CreatureState (highest priority first:
	// die > spawn > hit > attack > walk > incombat > idle). Pure read of monster state.
	anim::CreatureState DesiredState(const Monster& monster) const;
	// Picks a clip name for a state from the kind's table — a random variation when
	// several are authored, or empty when the state is unauthored for the kind.
	// Returns a REFERENCE into that table, never a copy (see the definition).
	const std::string& PickClip(const MonsterKind& kind, anim::CreatureState state);
	// Duration (seconds) of a named clip in the kind's model, or 0 if absent.
	float ClipDuration(const MonsterKind& kind, const std::string& name) const;
	// Wakes a struck monster: latches awareness (sticky) and engages it toward the
	// party THIS frame, independent of its neighbours. Called where party damage
	// (melee or spell) lands on a monster.
	void ProvokeMonster(Monster& monster);
	// --- threat (aggro; see the Monster::threat comment) ----------------------
	// Accrues member-dealt damage onto the monster's threat table (× balance
	// threat_scale) and re-evaluates the lock, announcing a lock change.
	void AddThreat(Monster& monster, size_t member, float damage);
	// The lock state machine: keeps a valid lock until another alive member
	// EXCEEDS it by threat_switch; otherwise locks the alive argmax at/above
	// threat_threshold (or releases). `announce` logs a lock CHANGE (the accrual
	// path); the decay tick re-evaluates silently.
	void UpdateThreatLock(Monster& monster, bool announce);
	// The member this monster wants dead right now: the locked member while
	// alive and at/above threshold, else the alive argmax at/above threshold,
	// else -1 (uniform-random targeting). Pure read — used by the melee pick,
	// the ranged lane aim, and the projectile impact preference.
	int ThreatTarget(const Monster& monster) const;
	// Picks the melee victim's roster index (-1 = nobody standing) under the
	// PER-FILE blocking rule: relative to a reach-1 monster's approach the
	// party stands in two files, and the FIRST STANDING member of each file is
	// touchable — a living near member shields the one behind, a fallen one
	// opens the file so the monster steps into the gap and reaches the far
	// member directly. The threat target is taken when reachable, else their
	// standing file mate (the blocker) soaks the swing. Below the threshold:
	// uniform-random among the reachable members.
	int PickMeleeVictim(Monster& monster);
	// A standing member's facing-relative sub-cell position (the quadrant the
	// portraits read: front pair a quarter-cell toward the facing, rear away,
	// even indices the on-screen-LEFT column). Shared by the projectile lane
	// test, the ranged lane aim, and the melee near-row math.
	Vec3 PartyMemberSubPos(size_t member) const;
	// Resolves a spell bolt reaching `impact.pos` with its strike profile: finds
	// a live monster in that cell, runs the strike (combat + log + slain), and
	// returns true if a monster was there (the bolt is consumed). A landed hit
	// with `impact.push` shoves the survivor that many cells along the bolt's
	// travel (walls/occupants stop it early). The moving-item engine
	// (m_projectiles) owns the bolt; this is the TargetSide::Monsters branch of
	// its impact hook.
	bool ResolveSpellHit(const ProjectileImpact& impact);
	// TargetSide::Party branch of the moving-item engine's impact hook: a
	// monster bolt reaching the party's cell strikes a random standing member
	// IN ITS LANE (the quadrant mirror of the party's shots — nobody in the
	// lane and it flies straight past; the air ward may deflect whoever it
	// picks) and is consumed (true, hit or miss). False while it is short of
	// the party or slides through an empty lane.
	bool ResolveMonsterProjectileHit(const ProjectileImpact& impact);
	// Skirmisher executor (intent == Kite): hold `keepRange` from the party (greedy
	// 1-step, LoS-preferring), and fire a ranged bolt when it has a clear line and is
	// off cooldown. `selfIndex` is the monster's index in m_monsters (for slot tests).
	void UpdateKiter(Monster& monster, int selfIndex);
	// Flee executor (intent == Flee): a wounded monster runs from the party — greedy
	// orthogonal 1-step that maximises distance; no attack. Holds if cornered.
	void UpdateFleer(Monster& monster, int selfIndex);
	// Leash-return: a leashed monster that idled beyond its range walks back to its
	// anchor (greedy orthogonal 1-step toward leashX/Z). Runs while idle + displaced.
	void UpdateReturner(Monster& monster, int selfIndex);
	// Patrol: an idle monster with a route walks it waypoint to waypoint (greedy
	// orthogonal step toward the next, advancing + wrapping on arrival). P3b.
	void UpdatePatroller(Monster& monster, int selfIndex);
	// Commit a one-cell move: snap the logical cell + slot, start the visual glide
	// from the current position, and arm the step cooldown. The single place a
	// monster's step is committed (chase-path follow, kite, flee all route here).
	void StepMonsterTo(Monster& monster, int x, int z, int slot);
	// Greedy local step shared by the kite/flee executors: among this monster's own
	// cell and its four free orthogonal neighbours, step to the one MINIMISING
	// `score(x,z)` (its own cell is the baseline, so it holds when nothing beats it).
	// `selfIndex` is its index in m_monsters (excluded from the slot test).
	void GreedyStep(Monster& monster, int selfIndex,
					const std::function<int(int cx, int cz)>& score);
	// Launches a monster bolt from `monster` toward the party through the shared
	// moving-item engine (TargetSide::Party); sets the attack cooldown + swing gesture.
	void MonsterRangedAttack(Monster& monster);
	// True if an unobstructed line runs between cells (x0,z0)->(x1,z1) over the LIVE
	// map (walls block). The host mirror of ai::SnapshotView::HasLineOfSight, used by
	// the kiter for firing + repositioning; endpoints never block.
	bool CellHasLineOfSight(int x0, int z0, int x1, int z1) const;
	// What a wound did to a member beyond taking health off: nothing, put them
	// down, or killed them outright (the overkill rule).
	enum class Fall : u8 { None, Down, Dead };
	// Apply `damage` to a member: clamp health, flash the hit splat (severity
	// by raw damage), and REPORT a downing/death rather than logging it — the
	// line is said by whoever narrated the blow (PartyTarget::NarrateFall), so
	// the cause reads before the effect. `quiet` is the DoT ticks' mode: no
	// splat (a per-frame tick must not flash one every frame).
	Fall WoundMember(Character& target, float damage, bool quiet = false);
	// A landed monster blow rolls its type's on-hit DoT (Phase 6): chance,
	// then land/refresh the effect with its log line. No-op for dps 0.
	// Strip a monster's effects (and with them its plume) — a corpse carries
	// nothing. Called from the apply stage when a blow finishes it.
	static void Extinguish(Monster& monster);

	// ONE frame of a combatant's effects, whoever they are: age them, bite
	// with their DoTs, and drop the expired.
	//
	// Each DoT is dealt as ITS OWN damage type (a burn as fire, a bleed as
	// pierce) and RESISTED at the moment it bites — so a ward raised while
	// you are burning starts helping immediately (docs/effects.md decision 1).
	// The bites accumulate per type and land AFTER the aging loop: dealing
	// damage runs the whole pipeline, which can mutate this very list (a water
	// veil bursting as it soaks a tick), so it must not run mid-iteration.
	//
	// `onExpire(inst)` says the line for an effect that ran out — the one
	// thing the two sides word differently. A template rather than a
	// std::function so a per-frame call allocates nothing.
	template <class OnExpire>
	void TickEffects(fx::ITarget& target, std::vector<fx::Inst>& effects,
					 float dt, OnExpire onExpire) {
		std::array<float, kDamageTypeCount> bite{};
		for (fx::Inst& e : effects) {
			e.timeLeft -= dt;
			if (e.IsDot())
				bite[static_cast<size_t>(e.kind->DamageTypeOf(e))] +=
					e.magnitude * dt;
			if (e.timeLeft <= 0.0f) onExpire(e);
		}
		std::erase_if(effects,
					  [](const fx::Inst& e) { return e.timeLeft <= 0.0f; });
		for (size_t i = 0; i < bite.size(); ++i) {
			if (bite[i] <= 0.0f) continue;
			fx::DamageEvent ev = fx::DamageEvent::Tick(
				static_cast<DamageType>(i), bite[i], DotSource(effects));
			fx::Deal(ev, target, m_balance.Strike(), m_combatRng);
		}
	}
	// Who to credit a DoT tick's damage to: the first sourced DoT on the list
	// (they are nearly always one, and threat is a coarse signal — the point
	// is that a hit-and-run torch keeps the grudge alive).
	static int DotSource(const std::vector<fx::Inst>& effects);
	// Where a burning body's flames rise from (torso height above visualPos):
	// the per-frame plume origin and the glow agree because both ask here.
	static Vec3 BurnOrigin(const Monster& monster);
	// How the flames READ per school — the FireEffect palette is authored
	// orange, so fire burns untinted and the other three recolour it (a water
	// burn is the freezing kind: the plume runs cold blue).
	static Vec3 BurnTint(SpellSymbol school);
	// The effect making this monster visibly burn (the first whose kind sets
	// effects.cat `plume`), or null. The plume and its light both read it, so
	// what is drawn always follows what is actually on the monster.
	static const fx::Inst* PlumeEffect(const Monster& monster);
	// Read a catalog's on-hit procs: the `on_hit` list, plus the older
	// one-effect-per-line fields (`poison`/`bleed` on a monster,
	// `element_dot` on a weapon) appended as procs naming the same effects.
	// `where` names the entry in any warning.
	static void ParseOnHit(const CatalogEntry* def, std::vector<fx::Proc>& out,
						   std::string_view where);
	// A log line ABOUT `member`: routes through onMemberMessage with their
	// identity color (the HUD tints it), falling back to plain onMessage.
	void MemberMessage(const Character& member, const std::string& line) const;
	// If no member is standing, latch the one-shot party wipe (message + callback).
	// Returns true the frame it latches. Shared by the melee/ranged/bump paths.
	bool CheckPartyWipe();
	// Award skill XP to a member (docs/skills.md): logs a level-up, and drips
	// the SOURCE's associated stats forward (docs/combat.md part 2: the gain
	// splits evenly across `stats`; a stat point + log when a pool passes 1,
	// re-deriving the resource maxima). The ONE place skills grow — every
	// award site (successful cast, landed blow) routes through it.
	void GrantSkillXp(Character& member, std::string_view skillId, float xp,
					  std::span<const std::string> stats);
	// A whole stat point lands: increment, log, and re-derive the resource
	// maxima (stats feed them now). Shared by the creep pools and SpendStamina.
	void GrantStatPoint(Character& member, std::string_view stat);
	// --- the effect pipeline's two faces (docs/effects.md) --------------------
	// Damage flows through fx::Deal, which knows nothing of Character or
	// Monster; these adapters ARE that knowledge, and they are the only place
	// the two sides differ. Both are cheap stack values built at the call site.
	//
	// The one genuinely per-side stage is Wound: a member has splats, the
	// unconscious/overkill rules and the wipe latch; a monster has threat
	// credit, a flinch and a slain line. Everything before it — deflect,
	// strike, mitigate, absorb — is shared.
	class PartyTarget final : public fx::ITarget {
	public:
		PartyTarget(DungeonWorld& world, Character& member)
			: m_world(world), m_member(member) {}
		float Evasion() const override;
		float Soak() const override;
		float Resist(DamageType type) const override;
		std::vector<fx::Inst>& Effects() override { return m_member.effects; }
		void Wound(float amount, fx::DamageEvent& ev) override;
		void Absorb(float amount, fx::DamageEvent& ev) override;
		std::string Name() const override { return m_member.name; }
		void Say(const std::string& line) const override;
		void SayApplied(const fx::EffectKind& kind) const override;

		// Say the one-shot fall line for the wound just applied ("has
		// fallen!" / "has died!"), if any. The CALLER calls this after its own
		// "hits for N", so that cause still precedes effect in the log — the
		// apply stage knows what happened but not where to say it.
		void NarrateFall() const;

	private:
		DungeonWorld& m_world;
		Character& m_member;
		Fall m_fall = Fall::None;
	};

	class MonsterTarget final : public fx::ITarget {
	public:
		MonsterTarget(DungeonWorld& world, Monster& monster)
			: m_world(world), m_monster(monster) {}
		float Evasion() const override;
		float Soak() const override;
		float Resist(DamageType type) const override;
		std::vector<fx::Inst>& Effects() override { return m_monster.effects; }
		void Wound(float amount, fx::DamageEvent& ev) override;
		void Absorb(float amount, fx::DamageEvent& ev) override;
		std::string Name() const override;
		void Say(const std::string& line) const override;
		void SayApplied(const fx::EffectKind& kind) const override;

	private:
		DungeonWorld& m_world;
		Monster& m_monster;
	};

	// The balance knobs an effect's own maths needs, in the shape the module
	// takes them (it never sees Balance.h).
	fx::Knobs EffectKnobs() const { return {m_balance.stoneskinResist}; }
	// What a reacting effect deals its reprisal through (a fire shield's burn
	// goes back out via fx::Deal like any other damage), so every React call
	// site hands over the same two things this world resolves damage with.
	fx::ReactCtx Reaction() { return {m_balance.Strike(), m_combatRng}; }
	// The world lands a blow: Impact bash damage on every standing member,
	// through the ordinary pipeline (armour, Stone Skin and a water veil all
	// answer it), returning the WORST amount dealt for the caller's line.
	float CollideParty(float amount);
	// Blocked-move recoil reached its peak: jar every standing member for a
	// small amount of damage, flash a splat over each portrait, grunt once, and
	// latch a party wipe if the bruise is somehow the end of them.
	void OnBumpImpact();
	// The pit plunge LANDED (DungeonWorld::Update sequences the drop, then calls
	// this the moment before the level swap). The world's other collision: the
	// same Impact bash the bump deals, at the fall_damage knob — so armour,
	// Stone Skin and a water veil all answer a shaft exactly as they answer a
	// wall, and a party already at death's door can be finished by the floor.
	void OnFallImpact();
	// True if a monster of `self`'s size may stand on (x,z): in bounds, walkable,
	// not the party cell, and with a free SLOT (see FreeSlotInCell). Thin wrapper
	// over FreeSlotInCell for callers that only need yes/no.
	bool CellFreeForMonster(int x, int z, int self) const;
	// The index of a free sub-cell SLOT for a monster of `size` standing on (x,z),
	// or -1 if none (unwalkable, the party cell, full, or already held by a
	// different-size group). `self` (a monster array index, or -1) is excluded from
	// the occupancy scan. Slots are filled lowest-index-first. See Game/SlotGrid.h.
	int FreeSlotInCell(int x, int z, SizeClass size, int self) const;
	// A solid decoration standing on (cx,cz)? Blocks monsters exactly like the
	// party (the isOccupied lambda checks the same flag). Wall-mounted props
	// default non-solid, so only floor-standing blockers register.
	bool SolidDecorationAt(int cx, int cz) const;

	// True if a continuously-animating caster (a monster) is within the
	// light's reach — such a cube must re-render every frame.
	// Fed to m_shadows.ShouldRender as the world's per-light verdict.
	bool AnimatedCasterNear(const gfx::PointLight& light) const;

	// Reveals a cell and its eight neighbors in the fog-of-war set.
	void MarkSeen(int x, int z);

	// Captures the ACTIVE level's live dynamic state (revealed cells + monster
	// diff) as a SaveData::LevelState. Shared by StashActive and CaptureState.
	SaveData::LevelState SnapshotActive() const;
	// Stashes the active level's live state into m_levelStates[m_currentLevel],
	// so a later return (or a save) can restore it.
	void StashActive();
	// Whether (x,z)'s floor / ceiling block is an OPENING the builders skip
	// (CellHolesFn): driven by the stair link's stairs.cat `hole` field —
	// "floor" for down stairs and pits (defaulted for any up=0 type), "ceiling"
	// for a pit's lower half on the level below. Stair placement/erase rebuilds
	// the touched chunks so holes open/close immediately.
	bool FloorHoleAt(int x, int z) const;
	bool CeilingHoleAt(int x, int z) const;
	// Rebuilds only the surface chunks an edit at (x,z) touched — the cell's own
	// chunk plus, via shared wall faces, its orthogonal neighbours' chunks — so a
	// paint costs a handful of chunk uploads, not the whole map. Drains the GPU
	// first (the old chunk meshes may still be in flight). No-op before the
	// geometry is built. The full (re)bake lives in BuildDungeonMeshes (load /
	// quality hot-swap).
	void RebuildChunksAround(int x, int z);
	// Rebuilds the single chunk region (chunkX, chunkZ) in place.
	void RebuildChunkRegion(int chunkX, int chunkZ);
	// GeometryChunk -> SurfaceChunk (uploads the mesh). Shared by the full bake
	// and the region rebuild.
	SurfaceChunk MakeSurfaceChunk(GeometryChunk& gc);

	// --- rendering / culling ----------------------------------------------------
	// A rune's pulse multiplier (its emissive glow + the light it casts breathe in
	// lockstep). A static member because UpdateLights (DungeonWorld.cpp) and
	// SubmitSceneGeometry (DungeonWorld_Render.cpp) both read it.
	static float RunePulse(float time, int id);
	// One culler for both passes: a camera frustum (main pass) or a light sphere
	// (shadow pass). Chunks test as AABBs, discrete meshes as bounding spheres.
	// "Inside" for the frustum is plane·p >= 0 on all six planes.
	struct ViewCull {
		bool isSphere = false;
		Vec4 planes[6]{}; // frustum planes, world space, normalized
		Vec3 sphereC{};
		float sphereR = 0.0f;
		bool TestAABB(const Vec3& lo, const Vec3& hi) const;
		bool TestSphere(const Vec3& c, float r) const;
		static ViewCull FromFrustum(const Mat4& viewProj);
		static ViewCull FromSphere(const Vec3& center, float radius);
	};

	// All 3D draw calls, shared by the shadow and main passes. `cull` skips
	// chunks (AABB) and discrete meshes (sphere) outside the view/light; null
	// draws everything.
	void SubmitSceneGeometry(ID3D12GraphicsCommandList* list,
							 const ViewCull* cull = nullptr);
	void DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface,
					 const ViewCull* cull);

	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	audio::AudioEngine& m_audio;
	const SoundBank& m_sounds;
	const GameSettings& m_settings;
	const Project& m_project;   // content catalogs + level paths

	DungeonMap m_map;           // static layer (.map): structure, fixtures
	DungeonEntities m_entities; // dynamic layer (.ent): monsters, items, buttons
	Party m_party;
	std::string m_currentLevel; // active level stem (for transitions + saves)
	std::vector<u8> m_seen;     // fog of war, parallel to map cells (1 = revealed)
	gfx::Camera m_camera;
	gfx::LightSet m_lights;
	// Shadow-slot budgeting + cube-cache scheduling (UpdateLights feeds it the
	// frame's lights; RenderShadowMaps asks it which cubes to redraw). See
	// ShadowScheduler.h.
	ShadowScheduler m_shadows;
	gfx::Atmosphere m_atmosphere; // per-cell air turbidity (dust)
	std::unique_ptr<gfx::Texture> m_turbidityMap;
	// See-through peek (the Sight spell): recomputed each UpdateLights from the
	// active Sight effects. m_sightCell xy/zw = the ghosted wall cell's world
	// box (inactive when zw <= xy); m_sightTint rgb/a = the school ghost tint.
	// RenderScene copies them into the frame's Atmosphere; a fire-school peek
	// also drops a fill light in the revealed cell (UpdateLights).
	Vec4 m_sightCell{0.0f, 0.0f, 0.0f, 0.0f};
	Vec4 m_sightTint{0.0f, 0.0f, 0.0f, 0.0f};
	Vec4 m_sightHole{0.0f, 0.0f, 0.0f, 0.0f}; // x = eye Y, y = radius, z = across-axis is X

	Surface m_walls;
	Surface m_floors;
	Surface m_ceilings;
	// Resolved surface palettes: texture set names parallel to the map's palette
	// ids, plus the per-surface parallax height scale — filled by
	// ResolveSurfacePalettes, read by SurfaceDefs and LoadDungeonBlocks.
	std::vector<std::string> m_wallSets, m_floorSets, m_ceilingSets;
	// Per-variant parallax depth (height_scale × wear), parallel to the *Sets.
	std::vector<float> m_wallHeights, m_floorHeights, m_ceilingHeights;
	// Per-variant material factors (catalog metallic=/roughness=), likewise.
	std::vector<SurfaceMaterial> m_wallFactors, m_floorFactors, m_ceilingFactors;
	// Worn block geometry, one entry per texture variant (same order as the
	// surface texture sets), held between the load and mesh-build tasks. A wall
	// carries its side-pin combinations (WallPanels) rather than a single mesh,
	// so a face whose neighbour is the same surface can be stamped unpinned.
	std::vector<WallPanels> m_wallBlocks;
	std::vector<assets::MeshData> m_floorBlocks, m_ceilingBlocks;
	// Niche panels by wallfeatures.cat type (each entry's `model`.gltf); the mesh
	// builder stamps the one matching a niche's type. NicheMeshFor resolves it.
	std::flat_map<std::string, assets::MeshData> m_nicheMeshes;
	const assets::MeshData* NicheMeshFor(const std::string& type) const;
	// See-through bore panels by wallfeatures.cat type (its `model`.gltf); stamped
	// on the two flanking faces of a bored wall block. BoreMeshFor resolves it.
	std::flat_map<std::string, assets::MeshData> m_boreMeshes;
	const assets::MeshData* BoreMeshFor(const std::string& type) const;
	// Surface-feature tiles by surfacefeatures.cat type (its `model`.gltf), split
	// by the type's `surface` so each resolver answers only for its own side —
	// which is what lets the builder ask "is there a floor feature here?" and
	// "is there a ceiling one?" independently, without knowing the catalog.
	std::flat_map<std::string, assets::MeshData> m_floorFeatureMeshes;
	std::flat_map<std::string, assets::MeshData> m_ceilingFeatureMeshes;
	const assets::MeshData* FloorFeatureMeshFor(const std::string& type) const;
	const assets::MeshData* CeilingFeatureMeshFor(const std::string& type) const;
	// True if the surfacefeatures.cat type mounts on the ceiling (`surface =
	// ceiling`). The editor routes a placement through this.
	bool FeatureIsCeiling(const std::string& type) const;


	std::flat_map<std::string, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	std::flat_map<std::string, std::unique_ptr<ItemKind>> m_itemKinds;
	// Baked 3D item-icon thumbnails: each model ItemKind owns its RT texture
	// (ItemKind::iconTarget), rendered once before the first scene via
	// BakeItemIconsIfNeeded. Shared depth target + halo for the bakes.
	// (kIconSize is public — a caller that supplies its own target must match it.)
	gfx::ComPtr<ID3D12Resource> m_iconDepth;
	gfx::ComPtr<ID3D12DescriptorHeap> m_iconDsvHeap;
	std::unique_ptr<gfx::Texture> m_iconHalo; // soft round disc, white w/ radial alpha
	bool m_itemIconsBaked = false;
	// Monster head-shot + decoration whole-model map icons (each kind's
	// iconTarget), sharing the item bakes' depth/halo. A flag resets when a new
	// kind loads so it bakes next frame; the two fixture icons gate on their
	// texture existing instead (their meshes load once at boot).
	bool m_monsterIconsBaked = false;
	bool m_decorationIconsBaked = false;
	// Creates the shared icon depth target + halo on first use (all bakers).
	void EnsureIconBakeTargets();
	// One kind's head-shot bake: rest-pose mesh, framed on the model's top.
	void BakeMonsterIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
						 const MonsterKind& kind);
	// A static mesh baked whole (fit by its bounds): decorations, fixtures.
	void BakeMeshIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
					  const gfx::Mesh& mesh, const gfx::MaterialParams& material,
					  const Vec3& lo, const Vec3& hi, const gfx::Texture& target);
	std::vector<Item> m_items;
	std::vector<Button> m_buttons; // .ent buttons (toggle wired doors by name)
	std::vector<Door> m_doors;     // .ent doors (live open/anim state)
	// Shared carved-stone tablet, loaded once on the first rune kind; every rune
	// draws this mesh with its own element texture set.
	assets::ModelData m_runeModel;
	std::unique_ptr<gfx::Mesh> m_runeMesh;
	// Tablet AABB, cached on load so the floor draw (FloorItemWorld) can lay the
	// upright slab flat and re-ground it without rescanning the mesh each frame.
	Vec3 m_runeBoundsMin{}, m_runeBoundsMax{};
	// Ids for items dropped at runtime (not from the .ent baseline, which use
	// id >= 0). Decreasing from -2 so each dropped tablet has a unique save id
	// (-1 is the "no id" sentinel).
	int m_nextDropId = -2;

	// Combat: the Game's roster (not owned) + the strike RNG. UpdateMonsters
	// ticks cooldowns and runs monster melee; PartyAttack runs the party's.
	std::vector<Character>* m_roster = nullptr;
	std::mt19937 m_combatRng{0xC0FFEEu};
	bool m_partyWiped = false; // latches onPartyWipe so it fires once
	// The attack formula's tuning (docs/combat.md): balance.cat knobs +
	// attacks.cat numbers, loaded with the project in the constructor.
	Balance m_balance;

	// Magic: the self-contained spell system (recipe table + mana/cast resolution).
	// CastSpell delegates to it for the bolt spec, then Spawns it into m_projectiles.
	// See Magic.h.
	MagicSystem m_magic;

	// The status-effect registry (Effect/Effect.h): every effect kind, built
	// once from the classes + the project's effects.cat. EVERY fx::Inst in the
	// world — on a party member, and on a monster from P3 — points into it, so
	// it must outlive them all; being a member here, it does.
	fx::EffectBook m_effects;

	// The shared moving-item engine (Projectiles.h): flies + resolves + draws every
	// projectile — spell bolts today, monster ranged attacks next. The world seam
	// (cell blocking, faction-aware impact resolution, fizzle sound) is wired in the
	// constructor; live items/sparks are transient (never saved).
	ProjectileSystem m_projectiles;

	// Monster AI runs ASYNCHRONOUSLY (Game/MonsterAI.h): worker threads (one per
	// IQ bucket) think + path on their own cadence against a published snapshot,
	// while the main thread executes the resulting plans. The director owns the
	// threads (started on construction, stopped on destruction). See UpdateMonsters
	// / BuildAISnapshot / ConsumeAIPlans for the per-frame handoff.
	ai::AsyncDirector m_director;
	// Stable monster-id source: monotonic, session-global, never reused — so an
	// async plan keyed by a monster's runtimeId can never be misapplied to a
	// different monster that shifted into its old array slot (and a plan whose
	// monster is gone simply finds no match). Replaces the old index+generation
	// scheme, which broke on any mid-flight reorder/erase. Starts at 1 (0 = none).
	u32 m_nextMonsterId = 1;
	// Monster group-id source: a per-frame counter ReconcileGroups stamps cells
	// with; session-local, not saved (groups are re-derived from co-location).
	u32 m_nextGroupId = 1;
	// Last plan-batch sequence applied per bucket, so we adopt a batch only once.
	uint64_t m_lastPlanSeq[ai::Scheduler::kBucketCount] = {};
	// Walkability grid shared into snapshots, rebuilt only when the map changes.
	std::shared_ptr<const std::vector<uint8_t>> m_walkableCache;
	u32 m_walkableRev = 0xFFFFFFFFu; // map Revision() the cache was built for
	// Snapshot pool so steady-state frames allocate nothing (CLAUDE.md memory
	// strategy): BuildAISnapshot reuses a buffer no worker still holds (use_count
	// == 1), zero-filling its flat grids and clear()ing its vectors in place
	// (capacity retained) instead of make_shared.
	std::vector<std::shared_ptr<ai::Snapshot>> m_snapshotPool;
	// AssignFormation's aware-attacker index list — member scratch so the
	// every-frame formation pass doesn't heap-allocate (cleared, not freed).
	std::vector<int> m_formationScratch;

	// Build the immutable snapshot the AI workers read, and hand it over. Cheap:
	// reuses the cached walkability grid unless the map's revision changed.
	void BuildAISnapshot();
	// Adopt the freshest plan batches into each monster's intent + cached path.
	void ConsumeAIPlans();
	// Live monster with this stable runtimeId, or null if none (died/erased/level
	// changed). Linear scan — fine at this scale; swap for a map if counts explode.
	Monster* MonsterByRuntimeId(u32 id);

	std::flat_map<std::string, std::unique_ptr<DecorationKind>> m_decorationKinds;
	// unique_ptr so DecorationKind::tex stays valid as more sets are added
	// (flat_map stores values contiguously and reallocates on insert).
	std::flat_map<std::string, std::unique_ptr<PropTextures>> m_propTextures;
	std::vector<Decoration> m_decorations;
	std::optional<LevelTransition> m_pendingTransition; // raised by a stair step
	// A pit fall in flight: the transition latched when the party stepped onto
	// a `fall` link. The step glide finishes first (m_fallT < 0 = still
	// waiting), then the camera drops through the hole (PartyEye) and the
	// stashed transition is raised. See Update's fall block.
	std::optional<LevelTransition> m_pendingFall;
	float m_fallT = -1.0f;
	// The plunge's IMPACT, owed on the far side of the swap: the host clears
	// the message log as it places the party on the new level, so the bruise is
	// charged on the first frame after they arrive rather than before they
	// leave (OnFallImpact — otherwise its line would never be read).
	bool m_fellPending = false;
	// The party's eye for the camera / carried torch / particle sort:
	// Party::EyePosition plus the pit-fall drop, so the view and the light it
	// carries sink through the opening together.
	Vec3 PartyEye() const;
	// Dynamic state of INACTIVE visited levels (the active level's state is live
	// in m_seen/m_monsters). Stashed on leave, restored on return; the source for
	// a multi-level save (CaptureState) and filled by a load (ApplyState).
	std::flat_map<std::string, SaveData::LevelState> m_levelStates;
	// STATIC layer of inactive levels — the static twin of m_levelStates, so
	// UNSAVED editor edits (cells, variants, fixtures, stairs, decorations)
	// survive a level swap in memory instead of being dropped by the disk
	// re-parse. Stashed on leave (StashStaticMap), consumed on entry
	// (BeginLevelLoad), CREATED ON DEMAND by remote-level editing
	// (EnsureMapStash — the map overlay can edit any level, not just the active
	// one). unique_ptr so references survive sibling insertions. `savemap`
	// (SaveAllLevels) writes every stashed level back to its files.
	std::flat_map<std::string, std::unique_ptr<DungeonMap>> m_levelMaps;
	// The .ent-record twin of m_levelMaps: baseline records of inactive levels
	// whose RECORDS were edited (remote placements/erases, or active-level
	// prunes carried out by structural paints). Record ids are stable across
	// removals, so m_levelStates' per-id dynamic diffs stay valid against a
	// stashed baseline. Only edited levels get an entry (m_entsDirty tracks the
	// active level) — an untouched .ent file is never rewritten.
	std::flat_map<std::string, std::unique_ptr<DungeonEntities>> m_levelEnts;
	// The active level's m_entities records diverged from the .ent file on disk
	// (a prune/re-face edited them); stash them on leave so the divergence
	// survives the swap and savemap writes it.
	bool m_entsDirty = false;
	// Copies the active map into m_levelMaps, first syncing the live decoration
	// placements back into its records (AddDecoration only appends a live
	// instance; LoadDecorations rebuilds from records on return).
	void StashStaticMap();
	// The stash for `stem`, parsing the level's files on first use. The map
	// variant also creates nothing else; the ents variant needs the map for
	// record validation (soft: stale records skip with a warning). Never call
	// for the ACTIVE level (its truth is m_map/m_entities).
	DungeonMap& EnsureMapStash(const std::string& stem);
	DungeonEntities& EnsureEntStash(const std::string& stem);
	// Removes the paired return stair that `removed` (just taken off level
	// `fromStem`) points at: on the live map when that side is the active level
	// (prop erased too), else on its stash. Matched by cell + back-link, so a
	// hand-authored one-way link is left alone. True if a pair was removed.
	bool RemovePairedStair(const std::string& fromStem, const StairLink& removed);
	// The record-level twin of PruneEntitiesForCell for a STASHED level: a cell
	// painted solid buries the records standing on it (stairs via the pair
	// helper); one painted open re-faces neighbouring button records onto
	// another solid wall of their cell (or drops them). Wall-mounted decoration
	// records are left to the soft loaders (skip + warn) — they re-resolve on
	// the next entry.
	void PruneStashRecordsForCell(const std::string& stem, int x, int z);
	// Serializes a stashed level back to its .map (+ .ent when its records were
	// edited). The static writer is shared with SaveLevel.
	bool WriteStashedLevel(const std::string& stem) const;

	// --- editor undo/redo internals (see the public section) ------------------
	// Live decoration placements as .map records (the SaveLevel writer's emit in
	// record form). Shared by StashStaticMap and the undo capture.
	std::vector<Entity> LiveDecorationRecords() const;
	// A full editor-state snapshot: the active level (map with decoration
	// records synced, .ent records, dynamic-state diffs) + every stash. Levels
	// are a few KB, so a copy per edit is nothing.
	struct EditorSnapshot {
		std::string stem;            // active level at capture
		DungeonMap map;              // live decoration records synced in
		DungeonEntities ents;
		bool entsDirty = false;
		SaveData::LevelState state;  // live dynamic diffs (SnapshotActive)
		std::flat_map<std::string, std::unique_ptr<DungeonMap>> stashMaps;
		std::flat_map<std::string, std::unique_ptr<DungeonEntities>> stashEnts;
	};
	EditorSnapshot CaptureEditorState() const;
	// Restores a snapshot in place: static + records move-assigned, stashes
	// replaced wholesale, the dynamic layer respawned from the records and the
	// captured diffs (the level-swap flow), geometry fully rebaked (the
	// quality-swap path).
	void RestoreEditorState(EditorSnapshot snap);
	std::vector<EditorSnapshot> m_undoStack;
	std::vector<EditorSnapshot> m_redoStack;
	std::optional<EditorSnapshot> m_pendingUndo; // BeginUndoStep .. CommitUndoStep
	bool m_geometryDirty = false; // a restore skipped the rebake (FlushGeometry)
	// A restore also changed a level's surface PALETTE, so FlushGeometry must
	// reload the texture sets + worn meshes, not just re-stamp the chunks.
	bool m_surfacesDirty = false;

	std::vector<Fire> m_fires;
	// Per-fixture flame attachment (fixtures.cat flame_height / flame_scale /
	// flame_out, defaulting to the procedural meshes' constants) — an authored
	// replacement prop declares where its fire burns instead of having to be
	// modelled to the old mesh's proportions.
	struct FixtureFlame {
		// height/out are UNITS (points on the model, scaled by kUnit at use);
		// scale is a dimensionless particle-effect multiplier.
		float height, scale, out; // local Y, particle scale, offset from wall
	};
	// A fixture catalog id resolved to its renderable assets — the fixture
	// counterpart of DecorationKind, cached per id (FixtureKindFor), so every
	// fixtures.cat entry is placeable instead of the two manifest defaults.
	// part2 (mesh2/tex2/color2) is the optional co-located sub-prop with its
	// own material (the bought brazier's coal bed — the two models were
	// normalized TOGETHER at import, so their placements pre-align).
	struct FixtureKind {
		std::string id;
		bool wallMount = false; // fixtures.cat mount = wall|floor
		bool flameless = false; // fixtures.cat flame = 0: never lit (empty bowl)
		std::unique_ptr<gfx::Mesh> mesh;
		std::unique_ptr<gfx::Mesh> mesh2;
		assets::ModelData model; // kept for the map-icon bake's bounds fit
		Vec4 color{1, 1, 1, 1};
		Vec4 color2{1, 1, 1, 1};
		const PropTextures* tex = nullptr;
		const PropTextures* tex2 = nullptr;
		float modelScale = 1.0f; // fixtures.cat `scale`, like DecorationKind's
		FixtureFlame flame{0.0f, 0.0f, 0.0f};
		std::unique_ptr<gfx::Texture> iconTarget; // baked map icon
	};
	std::flat_map<std::string, std::unique_ptr<FixtureKind>> m_fixtureKinds;
	bool m_fixtureIconsBaked = false; // re-armed by a fresh kind (like decorations)
	FixtureKind& FixtureKindFor(const std::string& type);
	std::unique_ptr<gfx::ParticleBatch> m_particleBatch;
	std::vector<gfx::ParticleInstance> m_particleScratch;

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	int m_torchPalette = 0; // index behind m_torchColor (saved/restored)

	// Dev console toggles (see the hooks above).
	float m_fovDegrees = 70.0f;
	bool m_shadowsEnabled = true;
	bool m_dustEnabled = true;
	float m_ambientScale = 1.0f; // multiplies kBaseAmbient (mood-pass knob)

	float m_time = 0.0f; // latest frame time (Update), drives the rune glow pulse
};

} // namespace dungeon::game
