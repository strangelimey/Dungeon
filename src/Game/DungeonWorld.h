// ============================================================================
// Game/DungeonWorld.h — the 3D world and everything living in it.
//
// Owns the dungeon's two map layers (static .map structure, dynamic .ent
// spawns), the party, the monsters, the fires (sconces + braziers with
// particle effects), the serpent pillar, the batched surface geometry with
// its texture variants, the lights, and the camera — plus the per-frame
// simulation and both render passes (cube shadow maps, then the main scene).
//
// The world knows nothing about menus or the app state machine: Game decides
// when to load it (AppendLoadTasks feeds the staged loader), when to step it
// (Update only runs while playing), and when to draw it. Feedback flows out
// through onMessage (the HUD log) and the shared SoundBank.
// ============================================================================
#pragma once

#include "Animation/Animator.h"
#include "Assets/Model.h"
#include "Audio/AudioEngine.h"
#include "Game/Character.h"
#include "Game/Combat.h"
#include "Game/DungeonEntities.h"
#include "Game/DungeonMap.h"
#include "Game/FireEffect.h"
#include "Game/GameSettings.h"
#include "Game/LoadQueue.h"
#include "Game/Magic.h"
#include "Game/MonsterAI.h"
#include "Game/Party.h"
#include "Game/Project.h"
#include "Game/SaveGame.h"
#include "Game/ShadowScheduler.h"
#include "Game/SlotGrid.h"
#include "Game/SoundBank.h"
#include "Graphics/Camera.h"
#include "Graphics/D3DUtil.h"
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
	// pillar, monsters, fires, dust) to the staged loader. Order matters:
	// textures register their variant counts before the geometry task buckets
	// cells by variant. The texture work is split one task per material — the
	// scanned sets are the bulk of the load (~300 MB at Ultra), and
	// per-material tasks keep the progress bar moving through them.
	void AppendLoadTasks(LoadQueue& queue);

	// Hot-swaps the dungeon meshes (and textures, when crossing a resolution
	// boundary) for the settings' new quality tier, if they are already
	// built. Drains the GPU first — it may still be reading the old data.
	void ApplyQuality(bool textureResChanged);

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

	// Renders each model item's 3D thumbnail into its icon render-target once
	// (before the first scene): a soft round halo (via `sprites`) then the lit 3D
	// model over it. Redirects the OM and rebinds the back buffer.
	void BakeItemIconsIfNeeded(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites);
	// The baked 3D icon for an item type (building its kind on demand), or null
	// for a model-less item (the caller falls back to its flat placeholder).
	const gfx::Texture* ItemIconFor(const std::string& typeId);

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
	// no-op (returns false) otherwise.
	bool PartyAttack(size_t member, size_t hand);

	// --- spell casting ------------------------------------------------------
	// Façade over the MagicSystem (m_magic): the given roster member casts the
	// spell whose recipe matches the symbol sequence, fired down the party's faced
	// direction from the eye. The caster must be standing; the magic module gates
	// known-symbols / recipe / mana and spawns the bolt. This turns the cast
	// outcome into HUD/audio feedback and returns true on a successful cast. The
	// bolt's flight + impact run in MagicSystem::Update (see Magic.h). Driven by
	// the casting UI and the dev console `cast`.
	bool CastSpell(size_t member, std::span<const SpellSymbol> sequence);

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
	// the dynamic layer — a future save serializes it, never the .map file.
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
	// per-frame call. Structural only: fixtures, turbidity, and decorations
	// are not recomputed (matches DungeonMap::SetCell).
	void EditCell(int x, int z, Cell cell);

	// Which surface a variant edit targets.
	enum class SurfaceSel { Wall, Floor, Ceiling };
	// Pins a floor cell's wall/floor/ceiling texture variant to a palette index
	// (the variant index into the level's surface palette), then rebuilds like
	// EditCell. No-op on solid cells (variants live on floor cells).
	void EditVariant(int x, int z, SurfaceSel sel, int variant);

	// Live entity placement (editor). type is a catalog id (decorations.cat /
	// monsters.cat). Each instantiates the kind (loading its model/textures on
	// first use — ExecuteImmediate uploads synchronously, so it is safe mid-
	// frame) and appends a runtime instance that draws next frame. Returns false
	// when the cell is solid, the type is unknown, or (monsters) the cell is
	// already occupied. Edits are in-memory only (no .map/.ent write yet).
	bool AddDecoration(const std::string& type, int x, int z, Direction facing);
	bool AddMonster(const std::string& type, int x, int z, Direction facing);
	// Places a fixture (sconce/brazier from fixtures.cat — `mount` decides wall vs
	// floor) and rebuilds the fire instances + dust so it lights immediately and
	// persists. Returns false on an invalid cell (e.g. a sconce with no wall).
	bool AddFixture(const std::string& type, int x, int z);
	// Removes the topmost runtime entity in a cell (a monster first, else a
	// decoration). Returns true if something was removed.
	bool RemoveEntityAt(int x, int z);

	// A live entity's cell + type, for the map overlay (placed/erased entities
	// show immediately, and the marker can label its type + stack count). Built
	// fresh per call (editor-only, off the per-frame perf path).
	struct MapMarker {
		int x = 0, z = 0;
		std::string type; // catalog id (monster kind / decoration kind)
	};
	std::vector<MapMarker> MonsterMarkers() const;
	std::vector<MapMarker> DecorationMarkers() const;

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

	// HUD log feedback (bump lines, monster announcements, palette flavor).
	// Set before play starts; the party/monster callbacks route through it.
	std::function<void(const std::string&)> onMessage;

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
	struct Surface {
		std::vector<std::unique_ptr<gfx::Texture>> albedo;
		std::vector<std::unique_ptr<gfx::Texture>> normal;
		std::vector<std::unique_ptr<gfx::Texture>> mr; // ORM map (null = none yet)
		std::vector<SurfaceChunk> chunks;              // cullable, tagged by variant
		float heightScale = 0.0f;
		// Drops the texture variants (keeps the chunks) before a (re)load of the
		// set — the staged loader and the quality hot-swap both reuse the Surface.
		void ResetTextures(float hs) {
			albedo.clear();
			normal.clear();
			mr.clear();
			heightScale = hs;
		}
	};

	// A textured material set shared by props (defined in full below); forward-
	// declared here so monster kinds can point at one before the definition.
	struct PropTextures;

	// Per-kind monster assets (shared) and per-instance state. Kinds are
	// entity type names from the .ent file ("skeleton" loads skeleton.gltf).
	struct MonsterKind {
		assets::ModelData model; // must outlive the Animators pointing into it
		std::unique_ptr<gfx::Mesh> mesh;
		std::string name;
		// PBR set bound by type name (skeleton_<res>, ...); null = flat material.
		const PropTextures* tex = nullptr; // points into m_propTextures (stable)
		// Combat stats from monsters.cat (fallbacks in MonsterKindFor).
		float maxHp = 12.0f;
		float damage = 4.0f;
		float accuracy = 0.65f;
		float evasion = 0.1f;
		float armor = 0.0f;
		float attackInterval = 1.6f; // seconds between swings
		float aggroRange = 6.0f;     // cells of party distance to engage at
		float moveInterval = 0.6f;   // seconds per grid step while chasing
		// How clever the monster is: drives how OFTEN it re-decides (the AI tick
		// bucket), not what it decides. Higher iq -> a faster bucket (thinks more
		// often); see ai::Scheduler::BucketForIq. Default ~100 = middle of the pack.
		float iq = 100.0f;
		// Behaviour/appearance, data-driven from the catalog so AI and the
		// flat-material fallback never branch on the type name.
		bool facesTarget = true;     // turn to face the party once engaged
		// (radially-symmetric models like the blob set faces=false to skip it)
		float fallbackRoughness = 0.9f; // flat-material roughness when no PBR set
		// Sub-cell occupancy (monsters.cat `size=`, default large). Decides the
		// monster's footprint + how many share a cell — see Game/SlotGrid.h.
		SizeClass size = SizeClass::Large;
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
		float yaw = 0.0f;         // current visual facing (eased toward targetYaw)
		float targetYaw = 0.0f;   // desired facing: travel direction, or the party
		Direction facing = Direction::South; // for the .ent writer
		bool announced = false;
		// Has noticed the party (sticky): set when the brain first engages via the
		// sight cone, or immediately on a hit (provoke). Once aware the monster
		// stays engaged even if the party slips behind it. Saved (dynamic state).
		bool aware = false;
		float hp = 1.0f;          // current hit points (maxHp at spawn)
		float attackCd = 0.0f;    // seconds until this monster can swing again

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

		// Animation state machine (DriveMonsterAnim): which clip is currently
		// selected, plus one-shot timers. Priority die > attack > walk > idle;
		// walk/idle loop, attack/die play once. attackAnim counts down the swing
		// clip; deathAnim keeps the corpse drawn + animating while the death clip
		// plays, then it vanishes (0 = gone, or no die clip → instant as before).
		// Cosmetic/transient — not saved (a reloaded corpse just replays its death).
		enum class Anim { Idle, Walk, Attack, Die };
		Anim anim = Anim::Idle;
		float attackAnim = 0.0f;
		float deathAnim = 0.0f;

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
		float weight = 0.0f;     // carry weight (kg); sums into a member's load
		std::vector<std::string> commands; // hand right-click command ids (data-driven)
		bool isRune = false;
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
		// transparent until BakeItemIconsIfNeeded renders into it.
		std::unique_ptr<gfx::Texture> iconTarget;
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
	};

	// A wall-mounted button/lever (EntityKind::Button from the .ent layer). The
	// runtime carries the one bit that can change in play — `activated` — plus its
	// wiring (`target`, an id another entity reads) so the save layer can diff it
	// by `id` like a monster. The interaction itself (what a target does) is the
	// P5 door/mechanism work; this is the persistent state it will toggle. Buttons
	// have no model of their own yet (the map overlay marks them).
	struct Button {
		int id = -1;                         // source Entity::id (.ent baseline)
		int x = 0, z = 0;                    // the cell it mounts in
		Direction facing = Direction::South; // the solid wall it faces
		std::string target;                  // wired entity id (target= param)
		bool activated = false;              // pressed / toggled on (saved)
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
	struct Fire {
		bool brazier = false;
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
		float heightScale;
	};
	std::array<SurfaceDef, 3> SurfaceDefs();
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
	void LoadSurfaceMaterial(Surface& surface, const std::string& name);
	void LoadTextureSet(const SurfaceDef& def); // resets, then loads the set
	void LoadAllSurfaceTextures(); // reloads every set (quality hot-swap)
	void BuildDungeonMeshes();
	void LoadMonsters();
	void LoadItems(); // instantiates EntityKind::Item records (runes) from .ent
	void LoadButtons(); // instantiates EntityKind::Button records from .ent
	// Lazily loads (and caches) the shared behaviour for an item type, resolved
	// through the items catalog (category=rune → symbol + element glow colour).
	ItemKind& ItemKindFor(const std::string& type);
	// Renders a soft round halo (sprites) + one model's submeshes into an icon
	// render-target (fit to bounds, flat face to camera, 3/4 view). Shared depth
	// target; the bake list redirects the OM.
	void BakeIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
				  const MultiMaterialModel& model, const gfx::Texture& target);
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
	void LoadDecorations();
	void LoadStairs(); // places stair props (P6) from the map's stair links
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
	// prop draw (decorations, fires, pillar, monsters).
	static void ApplyPropMaterial(gfx::MaterialParams& m, const PropTextures* tex,
								  const Vec4& fallbackColor, float fallbackRoughness);
	// Builds an authored model's own GPU resources (one texture per embedded glTF
	// image, one submesh per primitive with its material) for the multi-material
	// decoration path.
	static std::unique_ptr<MultiMaterialModel> BuildMultiMaterialModel(
		gfx::GraphicsDevice& device, const assets::ModelData& model);
	void BuildFires();
	void BuildTurbidityMap();

	// --- per-frame --------------------------------------------------------------
	void UpdateCamera();
	void UpdateLights(float time);
	void UpdateMonsters(float dt);
	// One monster's melee strike against a random standing party member (called
	// from UpdateMonsters when the monster is adjacent and off cooldown).
	void MonsterAttack(Monster& monster);
	// Per-frame clip state machine: picks the monster's clip from its live state
	// (die/attack/walk/idle) and cross-fades on a change, then advances the
	// animator. Runs for downed monsters too, so the death clip plays out.
	void DriveMonsterAnim(Monster& monster, float dt);
	// Duration (seconds) of a named clip in the kind's model, or 0 if absent.
	float ClipDuration(const MonsterKind& kind, const std::string& name) const;
	// Wakes a struck monster: latches awareness (sticky) and engages it toward the
	// party THIS frame, independent of its neighbours. Called where party damage
	// (melee or spell) lands on a monster.
	void ProvokeMonster(Monster& monster);
	// Resolves a spell bolt reaching world position `p` with strike profile `atk`:
	// finds a live monster in that cell, runs the strike (combat + log + slain),
	// and returns true if a monster was there (the bolt is consumed). The
	// MagicSystem owns the bolt; this is its impact hook into the world.
	bool ResolveSpellHit(const Vec3& p, const AttackProfile& atk);
	// Blocked-move recoil reached its peak: jar every standing member for a
	// small amount of damage, flash a splat over each portrait, grunt once, and
	// latch a party wipe if the bruise is somehow the end of them.
	void OnBumpImpact();
	// True if a monster of `self`'s size may stand on (x,z): in bounds, walkable,
	// not the party cell, and with a free SLOT (see FreeSlotInCell). Thin wrapper
	// over FreeSlotInCell for callers that only need yes/no.
	bool CellFreeForMonster(int x, int z, int self) const;
	// The index of a free sub-cell SLOT for a monster of `size` standing on (x,z),
	// or -1 if none (unwalkable, the party cell, full, or already held by a
	// different-size group). `self` (a monster array index, or -1) is excluded from
	// the occupancy scan. Slots are filled lowest-index-first. See Game/SlotGrid.h.
	int FreeSlotInCell(int x, int z, SizeClass size, int self) const;

	// True if a continuously-animating caster (a monster, or the swaying pillar)
	// is within the light's reach — such a cube must re-render every frame.
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

	Surface m_walls;
	Surface m_floors;
	Surface m_ceilings;
	// Resolved surface palettes: texture set names parallel to the map's palette
	// ids, plus the per-surface parallax height scale — filled by
	// ResolveSurfacePalettes, read by SurfaceDefs and LoadDungeonBlocks.
	std::vector<std::string> m_wallSets, m_floorSets, m_ceilingSets;
	float m_wallHeight = 0.055f, m_floorHeight = 0.045f, m_ceilingHeight = 0.035f;
	// Worn block geometry, one mesh per texture variant (same order as the
	// surface texture sets), held between the load and mesh-build tasks.
	std::vector<assets::MeshData> m_wallBlocks, m_floorBlocks, m_ceilingBlocks;

	assets::ModelData m_pillarModel;
	std::unique_ptr<gfx::Mesh> m_pillarMesh;
	anim::Animator m_pillarAnimator;
	Vec3 m_pillarPos{};
	const PropTextures* m_pillarTex = nullptr; // null: pillar uses flat jade baseColorFactor
	bool m_pillarActive = false; // the serpent pillar is level-1 flavor only

	std::flat_map<std::string, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	std::flat_map<std::string, std::unique_ptr<ItemKind>> m_itemKinds;
	// Baked 3D item-icon thumbnails: each model ItemKind owns its RT texture
	// (ItemKind::iconTarget), rendered once before the first scene via
	// BakeItemIconsIfNeeded. Shared depth target + halo for the bakes.
	static constexpr u32 kIconSize = 256;
	gfx::ComPtr<ID3D12Resource> m_iconDepth;
	gfx::ComPtr<ID3D12DescriptorHeap> m_iconDsvHeap;
	std::unique_ptr<gfx::Texture> m_iconHalo; // soft round disc, white w/ radial alpha
	bool m_itemIconsBaked = false;
	std::vector<Item> m_items;
	std::vector<Button> m_buttons; // .ent buttons (state-only until P5 wiring)
	// Shared carved-stone tablet, loaded once on the first rune kind; every rune
	// draws this mesh with its own element texture set.
	assets::ModelData m_runeModel;
	std::unique_ptr<gfx::Mesh> m_runeMesh;
	// Ids for items dropped at runtime (not from the .ent baseline, which use
	// id >= 0). Decreasing from -2 so each dropped tablet has a unique save id
	// (-1 is the "no id" sentinel).
	int m_nextDropId = -2;

	// Combat: the Game's roster (not owned) + the strike RNG. UpdateMonsters
	// ticks cooldowns and runs monster melee; PartyAttack runs the party's.
	std::vector<Character>* m_roster = nullptr;
	std::mt19937 m_combatRng{0xC0FFEEu};
	bool m_partyWiped = false; // latches onPartyWipe so it fires once

	// Magic: the self-contained spell system (recipe table + live bolts/sparks).
	// CastSpell delegates to it; the world seam (cell blocking, impact resolution,
	// fizzle sound) is wired in the constructor. See Magic.h.
	MagicSystem m_magic;

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
	// == 1) and clear()s its vectors (capacity retained) instead of make_shared.
	std::vector<std::shared_ptr<ai::Snapshot>> m_snapshotPool;

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
	// Dynamic state of INACTIVE visited levels (the active level's state is live
	// in m_seen/m_monsters). Stashed on leave, restored on return; the source for
	// a multi-level save (CaptureState) and filled by a load (ApplyState).
	std::flat_map<std::string, SaveData::LevelState> m_levelStates;

	std::vector<Fire> m_fires;
	std::unique_ptr<gfx::Mesh> m_sconceMesh;
	std::unique_ptr<gfx::Mesh> m_brazierMesh;
	Vec4 m_sconceColor{1, 1, 1, 1};
	Vec4 m_brazierColor{1, 1, 1, 1};
	const PropTextures* m_sconceTex = nullptr;  // worn-medieval iron (sconce_<res>)
	const PropTextures* m_brazierTex = nullptr; // bronze (brazier_<res>)
	std::unique_ptr<gfx::ParticleBatch> m_particleBatch;
	std::vector<gfx::ParticleInstance> m_particleScratch;

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	int m_torchPalette = 0; // index behind m_torchColor (saved/restored)

	// Dev console toggles (see the hooks above).
	float m_fovDegrees = 70.0f;
	bool m_shadowsEnabled = true;
	bool m_dustEnabled = true;

	float m_time = 0.0f; // latest frame time (Update), drives the rune glow pulse
};

} // namespace dungeon::game
