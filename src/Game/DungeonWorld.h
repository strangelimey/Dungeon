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
#include "Core/Easing.h" // EaseSpan (door + opener motion shaping)
#include "Game/Balance.h"
#include "Game/Character.h"
#include "Game/Combat.h"
#include "Game/DamageLedger.h" // the one-pipeline invariant, checked
#include "Game/DungeonEntities.h"
#include "Game/Validate.h"
#include "Game/DungeonMap.h"
#include "Game/WorldMap.h"
#include "Game/DungeonMeshBuilder.h" // WallPanels (the worn wall block's variants)
#include "Game/FireEffect.h"
#include "Game/GameSettings.h"
#include "Game/ItemDetails.h"
#include "Game/LightProfile.h"
#include "Game/LoadQueue.h"
#include "Game/Magic.h"
#include "Game/Mishap.h" // fumble consequence tables on the kind structs
#include "Game/MonsterAI.h"
#include "Game/Party.h"
#include "Game/Project.h"
#include "Game/Projectiles.h"
#include "Game/SaveGame.h"
#include "Game/ShadowScheduler.h"
#include "Game/SlotGrid.h"
#include "Game/SoundBank.h"
#include "Game/Threat.h"
#include "Game/Power.h"
#include "Graphics/Camera.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/ModelPreview.h" // gfx::PreviewSubmesh (editor instance previews)
#include "Graphics/ParticleBatch.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"

#include <algorithm>
#include <array>
#include <flat_map>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dungeon::game::fx {
class LightEffect;
}

namespace dungeon::game {

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

	// Hot-swaps the dungeon meshes for the settings' new quality tier, and when
	// the swap crosses a resolution boundary every texture set loaded at a tier:
	// the surfaces AND every prop set (props, doors, fixtures, monsters, runes -
	// ReloadPropTextures, in place). Drains the GPU first - it may still be
	// reading the old data.
	void ApplyQuality(bool textureResChanged);

	// Reloads the worn block meshes and rebuilds the batched dungeon geometry in
	// place (the ApplyQuality core). A surface type's RESTYLE (the type editor
	// saving a `rebakes` field - Game::StartRestyleBake, landed by
	// LandRestyleBake) calls this after re-baking a texture's worn_*.gltf and
	// writing the Save, to swap the new geometry in live.
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
	// showed on the next level entry. `catalogKey` picks the cache; an ITEM
	// catalog's kind is rebuilt in place instead (RebuildItemKind - its holders
	// keep its address, so nothing respawns), and a category with no kinds
	// (effects, flags, the world's) does nothing at all. The respawn KEEPS the
	// level as the editor and play left it (HoldActiveState / RestoreHeldState):
	// a placed monster or prop, an opened door, a corpse, a drop - all of which a
	// respawn from the records alone used to lose (C311).
	void ReloadTypeKind(const std::string& catalogKey, const std::string& id);
	// Re-reads lights.cat (lighting-updates Phase 2, Game/LightProfile.h). Every
	// light resolves its profile by id each frame, so a saved edit shows on the
	// next frame - nothing to respawn. Also re-reads the `light` field of the
	// cached item kinds, which no other reload reaches.
	void ReloadLightProfiles();
	// The profile `id` names; the built-in warm fallback for an id the project
	// lacks (warned once, at load), so a typo is a visible light, not darkness.
	const light::Profile& LightProfileFor(std::string_view id) const;
	std::span<const light::Profile> LightProfiles() const { return m_lightProfiles; }
	// The colour a fixture kind's fire gives when its placement sets none (its
	// light profile's) - what the fixture dialog's colour picker starts from.
	Vec3 FixtureLightColor(const std::string& type);
	// The `lights` dev command's readout: one line per light this frame.
	std::vector<std::string> DescribeLights() const;
	// trails.cat (lighting-updates Phase 4, Game/Trail.h), re-read with the
	// lights by ReloadLightProfiles. A trail is COPIED into each projectile at
	// launch, so a reload never reaches one already in flight. The spec `id`
	// names; one that sheds nothing for an id the project lacks.
	const trail::Spec& TrailSpecFor(std::string_view id) const;
	// The `trails` dev command's readout: the pool, and each profile.
	std::vector<std::string> DescribeTrails() const;
	// `lightstress <n> [near]`: n test lights over the level the party can
	// reach, or (`near`) within 6 steps of it (0 = none); returns how many were
	// placed (up to kLightCandidates, 256). A measuring load for the light
	// budget. `first` (`lightstress fill`) pushes them AHEAD of the fires, so a
	// full load fills the candidate list before the fire loop runs and every
	// fire's PushLight comes back null (code-review C181).
	int SetStressLights(int count, bool nearby, bool first = false); // (`near` is a Windows macro)
	// The candidate list's ceiling (kLightCandidates), for `lightstress fill`.
	static int LightCandidateCeiling();
	// The tiled light lists on or off (`lighttiles`), for measuring them.
	void SetLightTiling(bool on) { m_renderer.SetLightTiling(on); }
	bool LightTiling() const { return m_renderer.LightTiling(); }

	// "Start New Game": snaps the party home and re-arms the monster
	// announcements (the caller clears the log right after, as before). Also the
	// first half of a LOAD, which lays the save on top of it - so whatever it
	// leaves standing, a load inherits: it clears the level's transients
	// (ClearLevelTransients) and mends every door, prop and fixture (C292, C293),
	// and it ends what the GAME being left had running - a rest, quietly and
	// handing lockstep back (C295), the throw and kindle clocks (C294) and the
	// editor's undo history (C297).
	void ResetForNewGame();

	// One frame of the world: party input, then `dt` of simulation (party
	// movement, animators, monsters, projectiles, effects - see the fixed ticks
	// below), then what the frame shows of it - lights (with shadow-slot
	// assignment), camera, and the fire particles (gathered back-to-front for
	// the smoke blend).
	// acceptInput=false simulates the world but ignores party movement keys —
	// used while the dev console is open (the world keeps running, the party
	// stays put). Everything else (physics, monsters, lights, particles)
	// updates regardless.
	void Update(const Input& input, float dt, float time, bool acceptInput = true);

	// THE WORLD'S FIXED TICKS (code-review C64). A `dt` no longer than
	// kMaxWholeStep is simulated as ONE step: every ordinary frame (Core/Time
	// clamps a frame at 0.1 s) and every harness `step` tick, so play is exactly
	// what it was. A longer one - rest's 60x, which hands the world about a
	// second a frame, or a dev `timescale` - runs as fixed ticks of kTick, the
	// harness's own 60 Hz, the remainder carried to the next frame. One step of
	// a whole second let a monster think once and walk one square in it, because
	// everything that paces this game counts a timer down by dt and acts at most
	// once a step; ticks give it the thinks and the steps an awake second would.
	//
	// AT MOST kMaxTicksPerUpdate A FRAME, and the time past that is DROPPED, not
	// owed: a frame too slow to fit its ticks rests a little slower rather than
	// owing the next frame more ticks than it has time for (Michael: a slow
	// frame rests slightly slower). 90 keeps the full 60x down to 40 frames a
	// second.
	//
	// The ticks STOP EARLY when the frame's rest ends, the party changes level
	// or the party is wiped: what was left was rest's time, or a world the host
	// is about to leave.
	static constexpr int kTicksPerSecond = 60;
	static constexpr float kTick = 1.0f / kTicksPerSecond;
	static constexpr float kMaxWholeStep = 0.1f;
	static constexpr int kMaxTicksPerUpdate = 90;
	// What the last Update simulated: how many steps (a whole step is one), how
	// many world seconds they covered, whether the cap cut it short, and the
	// world time it left owed to the next Update (the carry - zero after a whole
	// step, a cap or a stop). The `frames` command's readout.
	struct UpdateRun {
		int ticks = 0;
		float seconds = 0.0f;
		bool capped = false;
		float owed = 0.0f;
	};
	const UpdateRun& LastUpdate() const { return m_lastUpdate; }
	// THE WORLD CLOCK: every Update the world has taken since it was made, and
	// the world seconds they simulated. Counted here, in the callee, so it says
	// whether the world RAN - whoever called it (code-review C78 / C125: the open
	// console used to run a paused editor's world and freeze the sheet's). Read
	// by `worldclock`.
	struct Clock {
		u64 updates = 0;
		double seconds = 0.0;
	};
	const Clock& WorldClock() const { return m_clock; }

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
	// Where a LIT item's flame stands in its icon (uv, 0..1 from the top-left):
	// the top of its longest axis, through the very pose the icon was baked in
	// (ItemIconWorld). False for an item that is not lit, has no model, or spins
	// (an icon_spin icon moves, so no fixed point would hold).
	bool ItemFlameUv(const std::string& typeId, Vec2& uv);
	// The same head in MODEL space, for any other view of the model (the
	// details dialog's turning preview). False for an item that is not lit or
	// has no model.
	bool ItemFlameHead(const std::string& typeId, Vec3& head);
	// A magical torch's flame colour (items.cat `flame_color`). False for an
	// ordinary flame, which draws in the default orange.
	bool ItemFlameTint(const std::string& typeId, Vec3& tint);
	// Where a rune tablet's carved face sits in its baked icon (uv box, 0..1 from
	// the top-left), the same for every rune: DrawItemIcon lays the school's
	// glow over it. False before the tablet mesh is loaded.
	bool RuneFaceUv(Vec2& lo, Vec2& hi) const;
	// Builds the kind of EVERY catalog item, at load. A kind's first build
	// loads its model or its rune's PBR set (a first rune drop measured 246
	// allocations / 2 MB in a guarded frame), and an item can reach the floor
	// from any pack in any frame - so no kind is left for play to build. The
	// icon pass (Game::LoadItemIcons) already built every non-rune kind by
	// asking for its icon; runes never ask, which is how they were missed.
	void PreloadItemKinds();
	// WHAT IS A RUNE TABLET is its item KIND's to say (code-review C347): items.cat
	// `category = rune` with a `symbol`, ItemKind::isRune / runeSymbol - never the
	// `rune_` spelling of an id. True, with the symbol, for an item whose kind is
	// one; false for any other, and for an id no kind was built for (every
	// catalog item's is, PreloadItemKinds). A lookup only: it builds nothing and
	// allocates nothing (Memorize asks it on a click in a guarded frame).
	bool ItemRune(std::string_view typeId, SpellSymbol& symbol) const;
	// The item this world hands out as `symbol`'s tablet (the starter kit, dev
	// `rune`): the conventional rune_<symbol> when its kind is that rune, else the
	// first rune of that symbol in the item catalogs (items, weapons, armor), else
	// empty - a world with no such tablet has none to give.
	std::string_view RuneItemFor(SpellSymbol symbol);
	// Renders the map overlay's baked icons: each monster kind's HEAD SHOT (its
	// mesh in rest pose framed on the model's top quarter — a skull for the
	// skeleton), each prop kind's whole model (props read best in full; the
	// decorations', doors', levers' and stairs' caches all bake), and the sconce +
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
	// The parts are drawn as given (the caller resolved their materials -
	// PoolModelLook below).
	// `palette` poses a rigged model (its idle frame; empty = as modelled).
	// `tilt` tips its top toward the camera, in radians (the map icons' gentle
	// 0.3, or PoolModelLook::kFromAboveTilt for a thing seen from above).
	void BakeIconFor(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
					 std::span<const gfx::PreviewSubmesh> parts, const Vec3& lo,
					 const Vec3& hi, const gfx::Texture& target,
					 std::span<const Mat4> palette = {}, float tilt = 0.3f, float yaw = 0.5f);
	// A POOL model as the editor's asset picker shows it, preview and tile alike:
	// every primitive (not just meshes[0]) with its own glTF material and
	// embedded textures (the baked .dds sidecars, as the game loads them), node
	// transforms baked in - the multi-material path the world draws bought
	// models with - and, on any part with no image of its own, the texture set a
	// catalog binds to it (`setStem`, a resolution-tagged path stem; "" = none),
	// as the world draws a prop. Owns its GPU resources: a holder must let every
	// frame that drew it finish before dropping it (the SRV rule).
	struct PoolModelLook {
		std::vector<std::shared_ptr<gfx::Texture>> textures;
		std::vector<std::shared_ptr<gfx::Mesh>> meshes;
		std::vector<gfx::PreviewSubmesh> parts; // point into the two above
		Vec3 lo{}, hi{};                        // bounds of the baked geometry
		bool rigged = false;                    // it carries a skeleton (a creature)
		// A rigged model is SHOWN in its idle, not its bind pose (a T-pose for
		// the bought kit): `data` keeps the skeleton and clips an Animator
		// borrows, `idleClip` is what it plays ("" = no idle; the rest pose).
		std::shared_ptr<const assets::ModelData> data;
		std::string idleClip;
		// Draws `context` - the thing this part belongs ON (a door trim's leaf,
		// an opener's mount) - with it, every context part dimmed to
		// kContextShade so the subject stands out, and fits the view to both.
		// A door's iron straps or bronze bosses alone were scattered specks.
		void AddContext(PoolModelLook&& context);
		// A SLENDER model (kSlender times taller than it is wide - the door
		// chain, 7.7) fitted whole into a square tile is a hairline: framed on
		// its top kSlenderTop of height instead - its mount and first links,
		// big enough to read, the rest running off the tile's foot. Tiles only;
		// the preview pane is tall enough to show it whole.
		// A HUNG model (off the floor and reaching the ceiling, y = 1: the
		// hanging chain, 4.8) qualifies from kHungSlender: its top is its
		// anchor, so its top is what to show. Ratio alone could not pick it -
		// the potion vial is 4.7 and must stay whole; the torch bracket floats
		// too but stops at 0.66 and stays whole.
		bool Slender() const {
			const float h = hi.y - lo.y, w = std::max(hi.x - lo.x, hi.z - lo.z);
			if (rigged || w <= 0.0f) return false;
			const bool hung = lo.y > 0.05f && hi.y >= 0.95f;
			return h >= kSlender * w || (hung && h >= kHungSlender * w);
		}
		// The frame is also capped at kMaxFrameAspect times the model's width
		// (the cap a wall fixture's tile takes too): 40% of the door chain was
		// still 3x as tall as it is wide, a narrow column in a square tile.
		// With a CONTEXT (a chain on its socket) the frame starts at the
		// context's top when that is lower: what the subject hangs FROM is where
		// it starts, and the loop of chain over the socket's lip spent the top
		// of the frame on its least telling part.
		void FrameSlenderTop() {
			const float w = std::max(hi.x - lo.x, hi.z - lo.z);
			const float span = std::min(kSlenderTop * (hi.y - lo.y), kMaxFrameAspect * w);
			hi.y = std::min(hi.y, contextTop);
			lo.y = std::max(lo.y, hi.y - span);
		}
		float contextTop = 1e9f; // AddContext: the merged context's top
		static constexpr float kSlender = 6.0f, kHungSlender = 3.5f, kSlenderTop = 0.4f;
		// The tallest a TILE's frame may be, as a multiple of what it shows's
		// widest horizontal extent - the slender crop and the wall fixture's.
		static constexpr float kMaxFrameAspect = 2.2f;
		static constexpr float kContextShade = 0.45f;
		// Re-measures lo/hi with the parts posed by `palette` (CPU skinning, the
		// shader's sum), so a view fits the pose it shows rather than the
		// T-pose's outstretched arms.
		void FitToPose(std::span<const Mat4> palette);
		// lo/hi are the box a VIEW frames, not the whole mesh: a shaft running
		// more than a footprint below the floor is framed to half a footprint.
		// Nothing down there is seen in play except through a mouth - a floor
		// drain's shaft runs four squares deep on purpose, so you cannot find its
		// bottom - and framing all of it drew a feature as a tall white stick. A
		// well within a square (the pit, a stairwell) is framed whole.
		void FrameAboveFloor();
		// It lies FLAT (a blade, a grate, a floor tile): its framed height is
		// under 0.35 of its longest side. A rigged model never is - a creature
		// is long and low but is not something to roll over or look down on.
		// A flat model's tile is baked looking down at it, and its preview
		// tumbles rather than turning edge-on.
		bool Flat() const {
			const float longest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
			return !rigged && hi.y - lo.y < 0.35f * longest;
		}
		// The mesh reaches BELOW the floor plane: a surface feature (a recess,
		// a drain, cracked paving), a pit, a stairwell. Such a thing is only
		// ever seen from above, through its mouth, so a view of it looks almost
		// straight down (kFromAboveTilt) - at any ordinary angle a four-square
		// shaft pokes out past the tile's near edge.
		bool sinks = false;
		// How far a from-above view tips the model's top toward the camera
		// (radians; ~86 degrees). Steep enough to hide a shaft of radius 0.22
		// four squares deep under its tile - the drain's is 0.17, the recess's
		// 0.20.
		static constexpr float kFromAboveTilt = 1.5f;
		// What the model is stamped into, if it is a FEATURE or a surface BLOCK:
		// a floor (surface at y = 0, its hole below), a wall (face at z = 0, its
		// recess behind, toward -z) or a ceiling (met from below). A floor or wall
		// feature's INSIDE - every triangle behind that plane - is split into a
		// part of its own and drawn at kInsideShade. In play the scene's shadows
		// and dust darken a niche's recess or a drain's throat; the icon rig has
		// neither, so a recess lit as evenly as its wall, in the same brick, read
		// as a flat panel. The shading stands in for them.
		// CeilingWell is the floor well turned over: a shaft rising above a
		// ceiling hole (pit_ceiling), its rim at the model's lowest point, its
		// inside above it - seen straight up, graded darker going up. A plain
		// Ceiling (the block, the vault) has no inside and a gentler view.
		// WallFixture sits ON a wall face (fixtures.cat `mount = wall`), out
		// into the room - unlike Wall, a feature cut INTO it; it has no inside.
		enum class Mount : u8 { Free, Floor, Wall, Ceiling, CeilingWell, WallFixture };
		// The inside grades from kInsideShadeTop at the mouth to kInsideShade at
		// its deepest, in kInsideBands steps (see the loader).
		static constexpr float kInsideShade = 0.3f;
		static constexpr float kInsideShadeTop = 0.65f;
		static constexpr int kInsideBands = 4;
		Mount mount = Mount::Free;
		// Where a view stands: ABOVE anything in a floor (or reaching below it),
		// BELOW anything in a ceiling - the way a player meets it - else level.
		// The signed tilt a view tips the model's top toward the camera by:
		// +kFromAboveTilt, -kFromBelowTilt, or the caller's ordinary one. From
		// below is shallower (~50 degrees): a ceiling has no shaft to hide, and
		// straight up a vault's curve flattens into a plain rhombus.
		static constexpr float kFromBelowTilt = 0.9f;
		// A FLOOR feature (and the floor block) is cut at its framed depth by
		// the loader, so nothing deep is left to hide: it takes a three-quarter
		// view from above (~63 degrees) that shows the slab AND the hole - steep
		// enough that the half-square stub of shaft stays tucked under the tile
		// (radius up to ~0.24 at that depth), which straight down did not show.
		static constexpr float kFloorFeatureTilt = 1.1f;
		// A WELL - kept whole, deeper than a quarter square, walls near the
		// cell's edge (the pit, the stairwells) - cannot take that view: at any
		// oblique angle the band of wall below the tile's near edge shows, as
		// a skin or (that culled) the far wall's inside. Hiding it takes
		// tan(tilt) >= depth / (0.5 - wall radius): ~83 degrees for the pit,
		// near 90 for the stairs. So a well is seen straight down, and its
		// inside shading (Mount) is what makes it read - rim lit, shaft dark,
		// the treads lit going down.
		bool cutAway = false; // the loader cut it at its framed depth (ClipBelow)
		// A wall-sized panel: the loader set the dark beyond behind its back
		// face (what shows through an arch is a dark passage, not the icon's
		// halo), so a view must stay on its FRONT - a preview swings across it
		// instead of turning onto the backdrop.
		bool backed = false;
		bool Well() const {
			const float footprint = std::max(hi.x - lo.x, hi.z - lo.z);
			return !cutAway && -lo.y > 0.25f * footprint;
		}
		// A PLATE: flat, and broad on BOTH horizontal axes (a grate, a slab
		// lying down) - edge-on at an ordinary tilt it is a line, so it is seen
		// three-quarter from above like a floor feature. A blade is flat but
		// long on one axis only, and keeps the ordinary view.
		bool Plate() const {
			const float longest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
			return Flat() && std::min(hi.x - lo.x, hi.z - lo.z) >= 0.5f * longest;
		}
		float ViewTilt(float ordinary) const {
			if (mount == Mount::Floor) return Well() ? kFromAboveTilt : kFloorFeatureTilt;
			if (sinks) return kFromAboveTilt;
			if (Plate() && mount == Mount::Free) return kFloorFeatureTilt;
			if (mount == Mount::CeilingWell) return -kFromAboveTilt; // straight up it
			if (mount == Mount::Ceiling) return -kFromBelowTilt;
			return ordinary;
		}
		// How far round from face-on a tile looks at it (radians; the map icons'
		// 0.5 is a gentle three-quarter). A thing ON a wall (WallFixture: a
		// sconce, its bare bracket) is authored against the face with what
		// matters sticking OUT of it - the bracket's ring - which near face-on
		// foreshortens into a loop on a strap; kSideYaw shows it in profile.
		static constexpr float kSideYaw = 1.2f;
		// A wall fixture's TILE frames at most kMaxFrameAspect times its widest
		// horizontal extent, centred on `projectY` - the mean height of what
		// sticks out of the wall (its outer half in z), measured by the loader.
		// The bare bracket is a strap 4.3x taller than wide: fitted whole it was
		// a sliver; framed on its ring it is ~2x bigger and only the strap's
		// plain ends run off. The sconce (2.0) is under the cap and unchanged.
		float projectY = 0.0f;
		void FrameWallFixture() {
			if (mount != Mount::WallFixture) return;
			const float span = kMaxFrameAspect * std::max(hi.x - lo.x, hi.z - lo.z);
			if (hi.y - lo.y <= span) return;
			lo.y = std::max(lo.y, projectY - 0.5f * span);
			hi.y = lo.y + span;
		}
		float ViewYaw(float ordinary) const {
			return mount == Mount::WallFixture ? kSideYaw : ordinary;
		}
	};
	// Null when the file will not load. `thumbPx` > 0 loads the bound set's
	// maps trimmed to that size (a tile), else at the stem's full resolution.
	// `idleHint` is the clip a catalog names as the model's idle (monsters.cat
	// `anim_idle`); without one, a clip named idle (`idle__...`, the library's
	// state prefix) is used.
	static std::unique_ptr<PoolModelLook> LoadPoolModelLook(gfx::GraphicsDevice& device,
															const std::string& modelPath,
															const std::string& setStem,
															u32 thumbPx = 0,
															const std::string& idleHint = {},
															PoolModelLook::Mount mount =
																PoolModelLook::Mount::Free,
															const CatalogEntry* liquid = nullptr);
	// THE FOUR CATALOGS A PROP KIND IS DRAWN FROM (code-review C302): the
	// decorations, the doors (a leaf and its trim, frame, opener and mount), the
	// buttons (a lever and its plate) and the stairs. An id is unique only WITHIN
	// its catalog - `portcullis` was a door and a decoration both, and whichever
	// was drawn second wore the other's mesh, texture and map icon - so every
	// cache and every lookup of a prop kind takes the catalog with the id.
	enum class PropCatalog : u8 { Decorations, Doors, Buttons, Stairs, Count };
	// Its catalog key ("decorations", "doors", ...), and the catalog a key names
	// (none for a key that is not one of the four).
	static const char* PropCatalogKey(PropCatalog cat);
	static std::optional<PropCatalog> PropCatalogFor(std::string_view key);
	// Every prop kind built so far, one line each - its catalog, its id, the model
	// file and the texture set it loaded (dev `propkinds`): how a harness sees two
	// catalogs' kinds of one id stand side by side.
	std::vector<std::string> DescribePropKinds() const;
	// The baked icons for already-loaded kinds, or null (not loaded / not baked
	// yet) — the map overlay then falls back to its square markers. These never
	// force-load a model (browse markers may name unloaded types).
	const gfx::Texture* MonsterIconFor(const std::string& type) const;
	const gfx::Texture* DecorationIconFor(PropCatalog cat, const std::string& type) const;
	const gfx::Texture* ItemIconLookup(const std::string& type) const;
	// The baked map icon for a fixture kind (null until its one-shot bake ran —
	// the map overlay falls back to a colored marker). Kinds load lazily on
	// first use (BuildFires / placement), so the active level's are always in.
	const gfx::Texture* FixtureIcon(const std::string& type) const;

	// --- the monster map icons, checked (dev `mapicons`) -----------------------
	// What the last monster-icon bake did. Every loaded kind bakes in ONE pass
	// (one frame), each skinned kind posed by its own Animator, so a pass uploads
	// exactly one skinning palette per skinned kind and reuses it only for that
	// kind's other parts: an upload reused ACROSS kinds would be one kind drawn
	// in another's pose (code-review C189).
	struct MonsterIconBake {
		u32 passes = 0;  // bakes run so far
		u32 kinds = 0;   // kinds the last pass baked
		u32 skinned = 0; // ...of them drawn with a skinning palette
		u64 uploads = 0; // palettes that pass uploaded (gfx::PaletteStats)
		u64 reuses = 0;  // draws that took an upload already made
	};
	const MonsterIconBake& LastMonsterIconBake() const { return m_monsterIconBake; }
	// Bakes every loaded kind's map icon again on the next rendered frame.
	void RebakeMonsterIcons() { m_monsterIconsBaked = false; }
	// Loads a monster kind, whose icon then bakes with the others; false when
	// its model is not installed (MonsterModelAvailable - never the abort).
	bool LoadMonsterKind(const std::string& type);
	// Each loaded kind's icon AS ITS LAST BAKE DREW IT (MonsterKind::iconDrawn,
	// written by the bake): the clip of the palette it drew with ("" = the rest
	// pose) and the box its head shot was framed on. Beside them, measured here
	// afresh: the rig's joint count, the bind pose's box (a T-posed rig's arms
	// make it wider), and the kind's idle at its first frame - that pose's box,
	// and whether the drawn palette IS that pose (one the bake took from any
	// other animator, or from none, differs). A dev readout: it builds an
	// Animator and skins every vertex twice per kind, so never call it per frame.
	struct MonsterIconInfo {
		std::string type;
		size_t joints = 0;
		std::string pose;
		Vec3 bindLo{}, bindHi{};
		Vec3 frameLo{}, frameHi{};
		Vec3 posedLo{}, posedHi{};
		bool paletteIsPose = false;
		bool baked = false;
	};
	std::vector<MonsterIconInfo> MonsterIconReport() const;
	// The clip a kind's map icon was last baked in ("" = the rest pose, or not
	// baked) - the survey overlay's label, read every frame without a report.
	std::string_view MonsterIconPose(const std::string& type) const;

	Party& GetParty() { return m_party; }

	// --- combat -------------------------------------------------------------
	// Points the world at the Game's party roster (filled once, reset in place)
	// so monster melee can drain member health and party melee can read each
	// member's derived stats. Must be set before play; null = no combat.
	void SetRoster(std::vector<Character>* roster) { m_roster = roster; }
	// The CURSOR's item (Game owns it): a lit torch carried on the cursor still
	// burns and still lights the way (DungeonWorld_Light.cpp). Borrowed.
	void SetCursorItem(HeldItem* held) { m_cursorItem = held; }

	// THE PARTY LEADER (ui-updates Phase 9): the member who does what the mouse
	// does in the world - picks up, works a door or a lever, throws. A roster
	// index, party state saved with the party (`leader` save line; slot 0 leads
	// a new game). When the leader is down or dead the next STANDING member in
	// roster order takes over, and the lead does not return when they get up
	// (Michael). Today the leader only NAMES the act; checks of their skill or
	// strength hang off LeaderMember() later.
	int Leader() const { return m_leader; }
	// The leader, or null when nobody can act: no roster, or every member down.
	const Character* LeaderMember() const;
	// The leader's name for a log line about what they did ("" with no roster).
	std::string_view LeaderName() const;
	// A click on a name. Refuses (false, with a line saying why) a member out of
	// range or not standing; picking the leader again is a quiet yes.
	bool SetLeader(int member);
	// The game's FLAGS (WorldState::flags), borrowed the same way, for the three
	// things in a level that read one - doors, levers and stairs (flag=) - and
	// the levers that write one. Null = no flags: every wait is satisfied,
	// which is what a level had before flags existed.
	void SetFlagStore(WorldState* state) { m_flagStore = state; }
	bool FlagOn(std::string_view id) const {
		return id.empty() || !m_flagStore || m_flagStore->FlagOn(id);
	}
	// The WORLD, by pointer, for the undo history ALONE (Michael's answer:
	// one history, so a step spanning tiers undoes as one thing). The world is
	// Game's and stays Game's — this is the same borrowing m_roster does, and
	// for the same reason: the snapshot has to be able to copy and restore it.
	//
	// NOTHING ELSE HERE READS IT. If a second use appears, that is the moment
	// to ask whether the world belongs somewhere both can see rather than
	// letting the level simulation grow a second opinion about the overworld.
	void SetWorldForUndo(std::optional<WorldMap>* world) { m_worldForUndo = world; }
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
	// costs (swings, marching) all route through here. RETURNS the shortfall
	// the bar could not cover, which every caller but SpendExertion ignores —
	// see the definition for why it is measured there and not by the caller.
	float SpendStamina(Character& member, float points);
	// OVER-EXERTION's bill, once per swing or cast thrown from a stance past 1:
	// `points` is what the over-exertion bought on the attack roll
	// (defense::ExertionPoints), charged at exert_cost out of stamina and then
	// out of health. Can put a member down; never kills.
	void SpendExertion(Character& member, float points);
	// Re-derive every member's resource maxima from the balance k's — after a
	// save-apply, a stat change, or an editor Balance apply.
	void RecomputePartyMaxima();
	// Put every id a member could ever train into `skillXp` at zero, so that
	// GrantSkillXp's insert branch is only ever reached HERE, at setup.
	//
	// The map is name-keyed because skill ids are open-ended (Character.h says
	// why), and an open-ended key means the first award of one INSERTS — which
	// allocates, in whatever frame the player first happens to swing or walk.
	// That is not an event exemption the memory rule grants any more
	// (docs/message-allocation.md), and the shape of the fix is the same one
	// statProgress took: the set is not actually open-ended at RUNTIME. The
	// four schools, the three resource practices and every weapon class the
	// catalogs define are all knowable the moment a project is loaded, so they
	// are seeded once and the steady-state path only ever finds.
	//
	// A zero entry is invisible: every readout of skillXp already filters on
	// xp > 0 (the sheet's two lists, the save writer, `char`), because a skill
	// you have not trained is not one you have.
	void SeedPartySkills();
	// Every skill a member can train in this world, each once: the schools, the
	// resource practices, the bare-hand / throwing / defensive skills and every
	// item `skill`. What SeedPartySkills seeds and what a starting-skill pick
	// (party creation) may name.
	std::vector<std::string> TrainableSkills() const;
	// Feed the SLOWEST member's effective pace into the Party. Lives here rather
	// than on Game because it has to run the moment CONDITIONING levels — which
	// happens deep inside the combat tick — and the world holds both the roster
	// and the party. Game::ApplyPartySpeed forwards to it for the load and
	// new-game paths.
	void ApplyPartyPace();
	// THE WORLD HALF OF `reset` (docs/eval-harness.md "Recycling the world").
	// Put the world back where a NEW GAME would leave it, WITHOUT the twelve
	// seconds of level load — which is 80% of what a suite costs, and the whole
	// reason a run of hundreds of tests was not practical.
	//
	// "Where a new game would leave it" is the definition ON PURPOSE, because it
	// is the only one that can be CHECKED: run the same script after `newgame`
	// and after `reset` and the output must be byte-identical. Anything looser
	// ("clear the obvious things") is a promise nothing can test, and an
	// incomplete reset is the worst kind of defect here — every later suite in
	// the run is quietly contaminated and its numbers still look plausible.
	// The harness has already been bitten by exactly this shape once: the
	// m_partyWiped latch survived a heal and twelve rungs measured nothing.
	//
	// ResetForNewGame does most of it. This adds what a new game gets from the
	// LOAD rather than from that call, plus the harness's own modes. It re-reads
	// the CURRENT level, so the caller (Game::ResetForEval) first makes sure the
	// party stands on the harness level: a reset from anywhere else goes there by
	// a real load instead (code-review C300).
	void ResetForEval();
	// The part of a reset NO LEVEL FILE PUTS BACK and ResetForNewGame does not:
	// the harness's modes and the other levels' stashes (a real new game keeps
	// those - they are unsaved editor work). ONE list, shared by both ways a
	// reset goes - ResetForEval (the same level, re-read in place) and Game's
	// switch to the harness level, where a new game and a staged level load do
	// the rest - so the two cannot drift apart. (What the LEVEL had under way - a
	// blast, the fixture damage table, a pit fall - is ClearLevelTransients', and
	// rest, the throw and kindle clocks and the undo history are ResetForNewGame's;
	// both ways get them from there: C292, C294, C295, C297.)
	void ResetEvalTransients();
	// What a reset is supposed to have cleared, counted (the console's
	// `transients`; code-review batch 12). resettest.eval prints it in both of
	// its baselines, so each leak batches 77-79 close shows as a line that
	// differs - and a leak nobody has injected yet reads the same either side.
	struct TransientReport {
		int blasts = 0;            // live blast wavefronts (m_activeBlasts)
		int monsterEffects = 0;    // effects riding monsters, all told...
		int monstersAffected = 0;  // ...and on how many of them
		int brokenFixtures = 0;    // smashed sconces / braziers (m_fixtureBreaks)
		int brokenDecorations = 0;
		int brokenDoors = 0;
		int hurtPieces = 0;        // breakables damaged but still standing
		int pieceEffects = 0;      // effects riding a piece of dungeon (a door burning)
		bool fallPending = false;  // a pit fall under way (m_falling)
		bool fellPending = false;  // the landing's bruise, latched apart from it
		float fallT = -1.0f;       // the plunge's clock (< 0 = not plunging)
		int undo = 0, redo = 0;    // the editor's history depth
		// OTHER LEVELS still held in memory: static maps (m_levelMaps), .ent
		// records (m_levelEnts) and dynamic states (m_levelStates), plus every
		// stem any of them names, sorted. A reset forgets all three, so after
		// one these read 0 and "none"; a leftover is a level the next script
		// would find as the last one left it rather than as its file says.
		int stashedMaps = 0, stashedEnts = 0, stashedStates = 0;
		std::string stashedLevels;
		bool resting = false;
		bool lockstep = false;     // AI driven by sim time (rest forces it on)
		// Why rest last ended (RestEndReason), "" while it never has in this game.
		// A new game or a load ends one quietly and clears it (C295): kept, it
		// read "woken" for a rest nobody woke from, or the "recovered" an
		// auto-stop wrote into the loaded game.
		const char* restEnded = "";
		std::array<float, 4> throwCooldown{};
		float kindleClock = 0.0f;  // the firelight's kindling check (6d)
	};
	TransientReport Transients() const;
	// What the next `savemap` writes besides the active level, and whether the
	// active level's own editor layers differ from its files (the console's
	// `stashes`; code-review C298, C307, C308). Every stashed map or .ent is a
	// level the save rewrites, so a READ that leaves one here broke "never stash
	// to read", and a level that was only visited must not appear.
	struct StashReport {
		std::string maps, ents, states; // stems, comma-joined; "none" when empty
		bool mapEdited = false;         // ActiveMapEdited
		bool entsEdited = false;        // m_entsDirty
		bool parked = false;
	};
	StashReport Stashes() const;
	// The RECORDS on one square of `stem`, as the level reads (never stashed),
	// each with its stable id and whether the level's HELD dynamic state carries
	// a diff naming that id - the console's `editor records` (code-review C327:
	// a new record must take an id no held diff names, and an erase undone must
	// find its record's diff still held).
	struct RecordReport {
		const char* kind = "";
		std::string type;
		int id = -1;
		bool held = false;
	};
	std::vector<RecordReport> RecordsAt(const std::string& stem, int x, int z);
	// The active level's MAP differs from its file - measured, not latched (see
	// m_mapAsFiled), so an undo back to the file's state reads clean again.
	bool ActiveMapEdited() const;
	// --- rest (docs/health-and-healing.md "Rest is a STATE") ------------------
	// A STATE you enter and leave, not a command with a duration (Michael's
	// call): time runs fast until you stop it, so you watch the meters fill and
	// decide when enough is enough instead of guessing an interval up front.
	// That also means the PLAYER is the interrupt rule, and no argument about
	// what counts as "something nearby" has to be settled.
	//
	// It multiplies TIME and nothing else. `RestTimeScale` is what Game folds
	// into the world dt, so every rate, timer and cooldown in the game
	// accelerates together — which is the whole reason rest is one knob rather
	// than a second set of resting rates that could drift.
	// `quiet` says nothing in the log: ResetForNewGame ends a rest that belongs
	// to the game being left (code-review C295), and its line would read as the
	// first thing that happened in the new one. Lockstep goes back either way.
	void SetResting(bool on, bool quiet = false);
	bool Resting() const { return m_resting; }
	float RestTimeScale() const { return m_resting ? m_balance.restScale : 1.0f; }
	// WHY rest last ended ("recovered" / "attacked" / "hungry" / "woken"), or ""
	// if it never has. The state ends by itself more often than by a click, and
	// its reason goes to the HUD message log — which a script cannot read. So
	// the one fact a measurement actually wants is kept here in English, next to
	// the flag, rather than being recoverable only by a human watching the game.
	const char* RestEndReason() const { return m_restEndReason; }
	// Eat or drink `typeId`, returning what it actually RESTORED — 0 when the
	// item feeds nobody or the member is already full, which is how the caller
	// knows to refuse the action and keep the item. Public because the HUD
	// raises it (GameUI::onConsume) and only the world has the catalogs.
	resource::Refill ConsumeItem(Character& member, const std::string& typeId);
	// What is left in the hand after consuming `typeId` (items.cat `drink_as`),
	// or empty when it is used up. A view of the kind's own string.
	std::string_view ConsumeLeaves(const std::string& typeId) {
		return ItemKindFor(typeId).drinkAs;
	}

	// --- the eval harness (docs/eval-harness.md) ----------------------------
	// Reseed the combat RNG. Every roll in the game comes off this one stream —
	// attacks, fumble chances, blast jitter, proc chances — and it is otherwise
	// constant-seeded, which makes a run perfectly reproducible AND makes every
	// run the same run. An eval needs a SAMPLE, so it varies this per encounter.
	// The PARTY half of a tick: the stabilize clock, cooldowns, regeneration,
	// supplies, status effects, the wipe check and the rest state. Public
	// because a world-map JOURNEY settles hours of it with no level to update
	// and no monsters to think (docs/world-map.md) — the dungeon's own update
	// calls exactly the same thing, so the two cannot drift.
	//
	// SLICE IT for a long span rather than passing the whole thing: the things
	// in here are state machines, and a DoT that would kill someone three hours
	// into a march has to kill them there.
	// `danger` is "a live monster is within aggro of the party", which holds
	// the unconscious back from self-stabilizing. It is a fact about A LEVEL, so
	// the caller supplies it: a travelling party is not in one, and false is not
	// a simplification there but the truth.
	void TickParty(float dt, bool danger);
	// Has a monster NOTICED the party - a live one, aware of it and acting on
	// that (not Idle)? The health bar's heartbeat quickens on it
	// (docs/icon-updates-plan.md). Set each dungeon tick; a journey reads false.
	bool PartyNoticed() const { return m_partyNoticed; }
	// Has the wipe latch fired? A settled JOURNEY reads it between slices, so a
	// party that dies three hours into a march stops being charged for the
	// other three.
	bool PartyWiped() const { return m_partyWiped; }

	void SeedCombat(u32 seed) { m_combatRng.seed(seed); }
	// The same stream the fighting draws from, for the things OUTSIDE combat
	// that must still be reproducible from a seed — a travel encounter roll.
	// Deliberately not a second generator: `seed` in a script has to mean the
	// whole run, and two streams would make it mean half of one.
	std::mt19937& Rng() { return m_combatRng; }

	// THE ARENA (DungeonWorld_Arena.cpp): carve a controlled space into the
	// LOADED map — no files written — and empty the world of everything the
	// authored level put there. The shapes are the geometries a propagating
	// blast has to be measured in.
	// THE ENCOUNTER TALLY (docs/eval-harness.md). Counted in the two fx::ITarget
	// adapters, which is the whole reason it is trustworthy: EVERY source of
	// damage in this game goes through the one pipeline (docs/effects.md), so a
	// blast, a DoT tick, a fire shield's reprisal and an ordinary sword blow are
	// all caught by the same two lines. A tally hung off the attack sites would
	// have quietly missed four of those five.
	//
	// Damage is recorded in ABSOLUTE points, not as a fraction of health. The
	// healing model is still to be designed, and fractions would silently change
	// meaning the day it lands; points will not.
	struct Tally {
		float dealt = 0.0f;   // reached monster hit points
		float taken = 0.0f;   // reached member hit points
		int hits = 0, misses = 0, crits = 0, fumbles = 0;
		int monstersSlain = 0;
		// DISTINCT MEMBERS who went down since the last `tally reset`, not the
		// number of times somebody fell. It used to count FALL EVENTS, and a
		// member who dropped unconscious and was then killed produced two of
		// them — so `downed=4` could mean four members down or two members down
		// and then finished off, which are very different readings of a rung
		// (docs/eval-audit.md F17).
		//
		// The mask is how it stays a member count without a container: one bit
		// per roster slot, cleared with the rest of the struct by `tally reset`
		// (which is `= {}`). u8 covers eight slots against a roster of four.
		int membersDowned = 0;
		u8 downedMask = 0;
		float seconds = 0.0f; // SIM seconds since the last reset
		// CARRIERS, apart from the party's swings (which are hits/misses above).
		// A party bolt that reached a monster in its lane and was resolved as a
		// hit or a miss; any carrier, either side's, that stopped WITHOUT
		// striking (a wall, or out of reach - onExpire); and every detonation,
		// whoever set it off. Counted
		// because a run that means to measure an impact must be able to show one
		// happened: `dealt` cannot tell a bolt from the burn it left behind.
		int boltHits = 0, boltMisses = 0, expiries = 0, blasts = 0;
		// Of those expiries, the ones that stopped AGAINST something - a wall or a
		// shut door (ExpiryCause::Wall), not their reach running out - and the
		// square the last of them stopped IN: the stone's or the door's own, which
		// FlightEnd then backs off to the open square in front. A flight whose
		// reach ran out short of the wall ENDS in that same open square, so only
		// these say which happened; tools\CombatTest.py's flight-end checks rest on
		// them (code-review C43, C44). -1 = no wall stop since the last reset.
		int wallStops = 0;
		int wallStopX = -1, wallStopZ = -1;
		// FLOOR ITEMS: a held item laid on the floor (the cursor drop; not a
		// weapon a fumble knocks loose) and a floor item lifted onto the cursor.
		// Counted for tools\AllocTest.ps1 -Items, which must show the moves it
		// measures actually happened.
		int drops = 0, lifts = 0;
		// THROWING (Phase 10): items thrown, and how their flights ended -
		// struck a monster (hit or miss) or landed without one. AllocTest
		// -Throw must show a throw that went and came down.
		int throws = 0, throwStrikes = 0, throwLandings = 0;
		// THE DUNGEON'S OWN EFFECTS: frames a breakable piece had its effect
		// list aged (TickBreakables) - one per piece per frame. AllocTest
		// -Impact must show a crate the blasts left alight actually burned.
		int sceneryTicks = 0;
		// Fixtures put out by breaking (DouseFixture): the light, flame and haze
		// change a wrecked brazier makes mid-fight. AllocTest -Impact must show one.
		int fixturesDoused = 0;
		// THE MONSTERS' MELEE on the party (MonsterAttack): blows that landed on
		// a member, and of those the criticals a piercing edge (`crit = pierce`)
		// drove UNDER the armour. tools\CombatTest.py weighs the armour lessons a
		// piercing attacker taught against these (code-review C11).
		int struck = 0, pierced = 0;
		// SHOTS AT THE PARTY (code-review C1, C18): burst bolts that went off on
		// contact with the party (ResolveMonsterProjectileHit's area branch), the
		// monster bolts a Wind Ward turned there, and what a gust's repel did to the
		// shots it met (RepelAhead) - weakened and flown on, flung back, or left with
		// nothing so it fell (`repelSpent`, not counted in `repelTurned`).
		// tools\AllocTest.ps1 -Burst must show each one happened in its window.
		int partyBursts = 0, wardTurns = 0;
		int repelWeakened = 0, repelTurned = 0, repelSpent = 0;
		// MONSTER melee swings (MonsterAttack, hit or miss): what says a monster
		// got where it fights from. `taken` cannot - a caster's bolts feed it too.
		// tools\AITest.py's formation checks rest on it (code-review C57, C58).
		int monsterSwings = 0;
		// MONSTER ranged shots (MonsterRangedAttack, which only a kiter fires):
		// what says a caster was KITING, so a check that a brute got past one
		// cannot pass because the caster never stirred (code-review C57).
		int monsterShots = 0;
		// The square the LAST throw came down on - where it landed, or where a
		// shattering one burst (LandThrown); -1 = none since the reset. A burst
		// leaves nothing on the floor to find it by.
		int landX = -1, landZ = -1;
		// The square the LAST carrier that stopped without striking stopped in
		// (ResolveProjectileExpiry) - a wall's own square when it broke on one;
		// -1 = none since the reset. What says a shot stopped AT a wall rather
		// than past it (code-review C48).
		int expireX = -1, expireZ = -1;
		// THE PARTY'S FUMBLES, past the count above: the ones that went SEVERE,
		// and the items a fumble knocked out of a hand onto the floor (a drop or
		// a fling - not the cursor drop `drops` counts). tools\AllocTest.ps1
		// -Swing must show a severe one put a held item down in its window
		// (code-review C10).
		int severeFumbles = 0, fumbleDrops = 0;
		// COSMETIC DRAWS: clip variations PickClip chose by drawing from the
		// cosmetic stream - one per state entered that authors several clips.
		// tools\CombatTest.py must show a sweep drew them, and that its combat
		// numbers still matched the same sweep without them (code-review C73).
		int clipDraws = 0;
	};

	// ========================================================================
	// THE HARNESS SEAM (docs/eval-harness.md) — every piece of state the eval
	// harness needs the world to hold that a PLAYER never asks for, in ONE
	// place with ONE name.
	//
	// It is GATHERED rather than compiled out, and that is a decision (Michael,
	// 2026-08-15, reviewing exactly this). The harness's entire value is that it
	// measures the SHIPPING binary — the same rule tools/RollTest's CMakeLists
	// states for the roll engine, "the real thing straight in, not a copy of
	// it". Behind `#ifdef` these fields would only exist in a build nobody
	// ships, the suites and /check-pipeline would be measuring that build, and
	// the project would carry a fourth configuration to rot unwatched beside
	// release and release-profile. What it costs instead is a few fields and a
	// handful of predictable branches.
	//
	// The point of the struct is that a touch site in the simulation reads
	// `m_harness.frozen` and says what it is, where a bare `m_freezeMonsters`
	// read like world state somebody forgot to explain.
	//
	// DELIBERATELY NOT IN HERE: lockstep AI. It looks like harness machinery
	// and is not — SetResting turns it on, because rest runs the world at 60x
	// and lockstep is what makes the monsters think honestly through a
	// fast-forward. It would have to exist if the harness never had.
	// ========================================================================
	struct Harness {
		Tally tally;
		// Every member swings whenever a hand is off cooldown and something is
		// in reach. A harness behaviour, not a game one — the player clicks a
		// hand slot — but without it a measured encounter is the party standing
		// still being hit, which is half a fight and reads as a whole one.
		bool autoAttack = false;
		// A HELD autoattack (`autoattack hold`): off until `alloctest`'s first
		// ARMED frame turns it on, as it releases a held autocast, so the swings
		// a measurement takes start inside its window (tools\AllocTest.ps1
		// -OnHitTypo and -Swing).
		bool autoAttackHeld = false;
		// THE LOADED DIE (`fumble`): the next swing that reaches a target from
		// member `member`'s hand `hand` (-1 = any) fumbles without a roll, on the
		// face that makes it SEVERE or a plain one. A severe face comes up about
		// once in a hundred swings, so without this the consequence table - a
		// weapon or a lit torch knocked to the floor - is reachable by no script
		// and no measurement on purpose (code-review C10). Spent by the swing it
		// loads.
		struct LoadedFumble {
			bool armed = false;
			bool severe = false;
			int member = -1, hand = -1;
		} loadedFumble;
		// Monster ACTION (movement and attacks) stops while everything that
		// HAPPENS TO them keeps running — animation, effects, blasts, damage. A
		// geometry probe needs its instruments to hold still: monsters parked on
		// known cells to read a blast's falloff otherwise walk off those cells
		// mid-measurement and report where they ended up instead.
		bool frozen = false;
		// A HELD freeze (`freeze hold`): frozen, AND their AI plans are dropped,
		// so not one monster notices the party - until `alloctest`'s first ARMED
		// frame releases both (AutoCast::held's rule). That puts a fight's FIRST
		// notice, first formation pass and first blow inside a measurement, where
		// a warm-up would have swallowed them (tools\AllocTest.ps1 -Melee).
		bool frozenHeld = false;
		// Queued walking steps (`forward`). They CANNOT simply be applied in a
		// loop: Party::Act starts a tween and refuses a new move while one is in
		// flight, so nine calls in a single frame perform ONE step and silently
		// drop eight — which reads as a party that will not advance. They are
		// fed one at a time as each completes.
		int pendingSteps = 0;
		// Casting ON A CLOCK (`autocast`): a round-robin of (member, spell) that
		// fires one entry every `every` sim seconds. The console's own frame is
		// never a guarded one, so a cast typed there puts its LAUNCH, and with a
		// short flight its IMPACT, outside any steady-state window; this is what
		// lets tools\AllocTest.ps1 -Impact put both inside one. The harness PAYS
		// the mana (the caster is topped up before each cast), so a measurement
		// is never limited by the pool. Fixed capacity and the spell id held
		// inline, so a tick allocates nothing of its own (the rule it measures).
		//
		// An entry may instead be a SHOT AT THE PARTY (`autocast bolt`), which no
		// party cast makes: `spell`'s bolt launched from (x, z) down roster slot
		// `slot`'s lane as a monster casts it (ShootSpellBolt, the `bolt` command),
		// and with `repel` > 0 met on the same tick by a gust's repel of exactly that
		// power, credited to `member` (RepelAhead, the `castsvc repel` service). An
		// exact power is the point: only a gust of the shot's own strength leaves it
		// nothing, and a real gust's power is whatever the caster's skill and stats
		// make it. tools\AllocTest.ps1 -Burst puts all of that inside a guarded window.
		struct AutoCast {
			static constexpr int kMaxEntries = 6;
			struct Entry {
				int member = 0;
				char spell[32] = {};
				u8 len = 0;
				bool bolt = false;   // a shot at the party, not a member's cast
				int x = 0, z = 0;    // bolt: the square it flies from
				int slot = -1;       // bolt: the lane (a roster slot), -1 = the middle
				float repel = 0.0f;  // bolt: a repel of this power at launch, 0 = none
				// What each attempt came to (CastSpell's verdict), so a rotation
				// that stopped producing bolts says so in `autocast`.
				int cast = 0, failed = 0;
				std::string_view Spell() const { return {spell, len}; }
			};
			std::array<Entry, kMaxEntries> entries{};
			int count = 0, next = 0;
			float every = 0.0f; // seconds between casts
			float timer = 0.0f; // until the next one
			// Parked until something releases it: `alloctest`'s first ARMED
			// frame does, so a barrage can begin exactly when a measurement
			// does (`autocast hold`). A wall-clock delay could not promise
			// that - the guard's warm-up is counted in frames.
			bool held = false;
		} autoCast;
		// Every Update runs its dt as ONE step, however long (`frames ...
		// whole`): the world's fixed ticks bypassed, which is what every
		// resting frame did before code-review C64. Kept so a check can put a
		// 60x dt in front of what must cope with one on its own - a flight's
		// half-square steps (ProjectileSystem::Update, C48).
		bool wholeSteps = false;
		// PIT FALLS BEGUN: the steps onto a pit that latched a plunge (onStep),
		// the step that assigns into the kept m_fall. Not in the tally, so the
		// window's reset leaves it be: Game::Update differences it across one
		// frame, and counts the
		// falls that began in a MEASURED one - armed to its end, inside an
		// `alloctest` window - as the verdict's `falls=`, AllocTest -Exit's proof
		// that the step it exists for was checked (code-review C210).
		u32 fallsBegun = 0;
		// LEVER PRESSES (PressButton, whatever the lever is wired to) and the ones
		// that FLIPPED A NICHE (ToggleNichesNamed found the name). Counted like
		// fallsBegun - differenced across a frame by Game::Update, kept only in a
		// measured one - as the verdict's `levers=` and `niches=`: AllocTest
		// -Lever's proof that a press wired to no niche and a reveal were both
		// checked (code-review C211).
		u32 leverPresses = 0;
		u32 nicheFlips = 0;
		// The presses whose reveal REBUILT chunks in play - a chunk it reaches
		// held no look to swap in (past kNicheLookNames, or buckets that differ):
		// the old drain, build and upload. `niche looks` prints it, and EditorTest
		// phase 55 wants none, including where a niche reaches a chunk with no
		// walls at all (the review of code-review C211's first cut).
		u32 nicheRebuilds = 0;
	};
	Harness& GetHarness() { return m_harness; }
	const Harness& GetHarness() const { return m_harness; }

	// UN-WIPE THE PARTY (the `heal` command). `m_partyWiped` latches so
	// onPartyWipe fires once — but it also gates every monster attack, so once
	// it is set the world never fights again and only ResetForNewGame clears it.
	// A ladder healing between rungs therefore ran rung 2 onward against
	// monsters that had permanently stopped swinging, and reported forty-five
	// simulated seconds of nothing as a result.
	//
	// IT ALSO PUTS EVERY MONSTER BACK ON COOLDOWN, and that is the subtle half.
	// An attack fires on `attackCd <= 0`, and a cooldown only ever counts DOWN
	// with dt — it never needs dt to FIRE. While the party is wiped the app sits
	// on the title screen and the world stops updating entirely, so any monster
	// that was adjacent and ready at the moment of the wipe stays ready for as
	// long as the party is down, and swings on the very FIRST frame this gate
	// re-opens. Measured: a skeleton took 11.57 off a member between `heal` and
	// the next line of a script, at `timescale 0`, with zero simulated seconds
	// elapsed — and `tiers.eval` read that as the veteran rung's starting health
	// while claiming "any difference is the seeding and nothing else"
	// (docs/eval-audit.md F16).
	//
	// A swing owed from a stretch of time the world was not running is not a
	// swing the party should take on standing up, so they are re-armed as if
	// they had just swung. GUARDED on the latch actually being set: `heal` is
	// also used mid-fight, and silently resetting cooldowns there would perturb
	// the very encounter somebody is measuring.
	void ClearWipeLatch();

	// Scale the MOST RECENTLY SPAWNED monster's hp and damage (the eval
	// harness's `spawn ... <strength>`). Applied after AddMonster rather than
	// passed through it, so the editor's placement path keeps its signature
	// and nothing but the harness can reach this.
	void ScaleLastMonster(float strength) {
		if (m_monsters.empty() || strength <= 0.0f) return;
		Monster& m = m_monsters.back();
		m.strength = strength;
		m.hp = m.MaxHp(); // spawned at full, and full has just changed
	}
	// Stand the MOST RECENTLY SPAWNED monster up at once (`spawn ... up`): no
	// rise. A kit skeleton otherwise lies 9.5-14 s getting up and does nothing
	// while it does (spawnrise.eval), so a measured fight placed beside the
	// party is over before the monster has swung - respond.eval's defence arms
	// read `taken` 0 on both sides. The same two fields a reload clears for a
	// monster that was already met (ApplyState's alreadyUp).
	void StandLastMonster() {
		if (m_monsters.empty()) return;
		Monster& m = m_monsters.back();
		m.spawnReq = false;
		m.spawnAnim = 0.0f;
	}
	// Give the MOST RECENTLY SPAWNED monster a piercing edge (`spawn ...
	// pierce`): its criticals go under armour as a `crit = pierce` kind's do. No
	// authored monster has one, and the armour lesson's rule for such a blow
	// (TrainDefense, code-review C11) cannot be measured without one.
	void PierceLastMonster() {
		if (m_monsters.empty()) return;
		m_monsters.back().piercing = true;
	}
	// Re-parse ONE item kind's swing `on_hit` from `spec` (the `onhit` command),
	// in memory only: the catalog is not touched, so nothing is saved and a world
	// reload puts the authored list back. A thrown item keeps the payload built
	// at load. It exists so a TYPO'D proc - an id naming no effect - can be swung
	// inside an allocation window (tools\AllocTest.ps1 -OnHitTypo): its warning
	// must excuse itself. False when the project has no such item.
	bool SetItemOnHit(const std::string& type, std::string_view spec);

	// DETONATE A NAMED SPELL'S BLAST at a cell, with no caster, no mana, no
	// skill roll and no bolt flight — the eval harness's way of asking a
	// geometry question directly (`blast <spell> <x> <z>`).
	//
	// It reads the spell's AUTHORED rules rather than taking numbers of its own,
	// so what a measurement describes is the content that ships. False if the id
	// names no spell, or names one that is not an area effect at all — reported,
	// because a blast that did not happen would otherwise read as a blast that
	// did nothing, and those are opposite answers.
	bool DetonateSpell(std::string_view spellId, int cx, int cz);
	// LAUNCH A NAMED SPELL'S BOLT AT THE PARTY from (x, z), as a monster casts it
	// (Spell::MonsterBolt, accuracy kHarnessBoltAccuracy) but with no monster:
	// down the row or column it shares with the party, in the QUADRANT LANE of
	// roster slot `slot` (AimAtLane, the monsters' own aim; an empty slot of a
	// short roster is a lane nobody stands in), or down the middle for slot < 0.
	// The harness's way of putting a bolt on a lane a real caster would not
	// choose - a shot flying past the party to break on the wall behind it
	// (`bolt <spell> <x> <z> [slot]`). False, warned, for an unknown spell, one
	// with no bolt, or a cell off the party's row and column.
	bool ShootSpellBolt(std::string_view spellId, int x, int z, int slot);
	static constexpr float kHarnessBoltAccuracy = 75.0f; // a skel_mage's
	// A PLAIN MONSTER SHOT from no monster (`shot <x> <z> <dir>`): the ember
	// bolt a skirmisher looses (EmberShot), launched from the centre of (x, z)
	// along `dir` at the party's side, with the caller's damage and accuracy -
	// a shot whose flight a check can place to the square (code-review C48).
	// False off the map or inside rock.
	bool LaunchShot(int x, int z, Direction dir, float damage, float accuracy);

	// (The four pieces of harness STATE those used to be are fields on
	// `Harness` above; the operations that need the world — an arena, a spawn,
	// a detonation — stay methods, because they are things done TO the world
	// rather than switches held on it.)

	enum class ArenaShape : u8 { Open, Corridor, DeadEnd, TJunction, Room };
	// Where the arena ended up. Derivable from the map size (it is centred), so
	// a script can hardcode the cells; reported so a log reader can check them.
	struct ArenaInfo {
		int x0 = 0, z0 = 0, x1 = 0, z1 = 0; // inclusive bounds
		int cx = 0, cz = 0;                 // the cell that matters for the shape
		// Where the PARTY is placed. Same as the centre for every shape except
		// Room, whose whole point is that the two are FAR APART: the monster
		// waits in the room and the party walks the corridor to reach it, so
		// the approach is part of what gets measured.
		int sx = 0, sz = 0;
	};
	bool BuildArena(ArenaShape shape, int w, int h, ArenaInfo& out);
	static bool ArenaShapeFromName(std::string_view name, ArenaShape& out);
	// Drive the monster AI from sim time instead of wall-clock; see
	// ai::AsyncDirector::SetLockstep for what that does and does not promise.
	// Clears the bucket accumulators so switching it on does not immediately
	// fire every bucket with a debt of however long the game had been running.
	void SetLockstepAI(bool on) {
		m_director.SetLockstep(on);
		for (float& c : m_bucketClock) c = 0.0f;
	}
	bool LockstepAI() const { return m_director.Lockstep(); }
	// What lockstep's inline compute has done since it last came on (`lockstep
	// stats`, which AllocTest -Rest reads).
	const ai::AsyncDirector::InlineStats& LockstepStats() const { return m_director.Inline(); }
	// How many plan batches a bucket has published (`aiwait` watches it climb).
	uint64_t AIPlanSeq(int bucket) const { return m_director.PlanSeq(bucket); }
	// The live combat tuning (balance.cat + attacks.cat knobs, Balance.h). The
	// editor's Balance dialog edits it in place and Save()s it via the project.
	Balance& GetBalance() { return m_balance; }
	const Balance& GetBalance() const { return m_balance; }
	// The status-effect registry — the one place an effect id resolves to its
	// kind. The save loader (Game::ApplyState) reads it to rebuild a member's
	// effects; everything else already holds kind pointers.
	const fx::EffectBook& Effects() const { return m_effects; }
	// The damage-type vocabulary, for anything that has to NAME a type it was
	// handed (the projectile inspector, the type editor).
	const DamageTypeBook& DamageTypes() const { return m_damageTypes; }
	// What a monster kind can do to the party, resolved into plain numbers for
	// the threat score (Game/Threat.h): its melee blow and, for a ranged
	// archetype, the shot it throws - built the way MonsterAttack and
	// MonsterRangedAttack build theirs (powers, the spell's bolt, on-hit
	// effects), but from the catalog entry alone, so scoring a pool loads no
	// models.
	threat::Profile ThreatProfile(const CatalogEntry& monster) const;
	// A monster kind's POWER (Game/Power.h; DungeonWorld_Census.cpp): its
	// derived threat, or the entry's authored `power` override. Everything
	// that ranks monsters asks here - the generator's pools, the palette's
	// band pips, the overview - so an override moves all of them at once.
	// Cached per edit revision (a type save calls NoteEdit); a Balance change
	// moves threat without an edit, so its apply calls InvalidatePowers.
	double MonsterPower(const CatalogEntry& monster) const;
	double DerivedPower(const CatalogEntry& monster) const;
	// 1..power::kBands for a kind of THIS project (0 = no such kind): which
	// fifth of the project's range of monster powers it falls in.
	int MonsterBand(const std::string& id) const;
	power::Range MonsterPowerRange() const;
	void InvalidatePowers() const {
		m_powers.valid = false;
		m_census.valid = false; // it counts monsters BY BAND
	}

	// THE CENSUS (DungeonWorld_Census.cpp; the editor's overview panel,
	// tool-refinement Phase 3): what every level of the project HOLDS, as
	// a save would write it: records everywhere, except the ACTIVE level's
	// monsters, which are its live list (an editor-placed one has no record
	// until a save; ActiveEntText writes the live list). One row per project
	// level in manifest order;
	// the panel sums rows for a dungeon or the world. Walks the levels the way
	// Validate does (the active one live, a stashed one from its stash, the
	// rest READ-ONLY - never stash to read), and is cached per edit revision,
	// so the panel can ask every frame.
	struct LevelCensus {
		std::string stem;
		std::string dungeon; // the dungeon holding it ("" = none)
		int monsters = 0;
		std::array<int, power::kBands> bands{}; // monsters per power band
		std::string strongest;                  // its most powerful monster kind
		double strongestPower = -1.0;
		int items = 0;
		int questItems = 0; // items whose type carries quest / flag / reveals
		// Where each of those lies, in record order (the palette's Quest items
		// section says where an item is, and goes there).
		struct Placed {
			std::string type;
			int x = 0, z = 0;
		};
		std::vector<Placed> questPlaced;
		int doors = 0;
		int lockedDoors = 0; // doors wanting a key
		int stairs = 0;      // ways on and off it (a ceiling hole is scenery)
		int buttons = 0;
		int squares = 0;     // walkable squares: how much has been BUILT (Phase 7)
	};
	const std::vector<LevelCensus>& Census();
	// A QUEST ITEM's type: one carrying `quest`, `flag` or `reveals` - a hook
	// that fires when it is lifted (Game::OnItemFound).
	static bool IsQuestItem(const CatalogEntry* e);

	// Armor (docs/damage-system.md): the class governing a member (the
	// HEAVIEST piece worn) and what it costs them on the defense roll.
	ArmorClass WornArmorClass(const Character& member); // non-const: ItemKindFor caches
	float ArmorPenalty(const Character& member, ArmorClass c) const;
	// The sheet's defense breakdown (Combat.h). Assembled here because only
	// the world can resolve items, knobs and the live formula.
	DefenseReadout DefenseFor(const Character& member);
	// The same, AS IF `itemId` were worn in its own slot — what the backpack
	// tooltip compares against. Computed by swapping the piece into the member's
	// OWN slot for the length of the call and back, so the answer comes from
	// the live formula rather than a second implementation of it that could
	// disagree. (It used to copy the whole Character, every frame the tooltip
	// was up; the swap goes through m_defenseScratch and allocates nothing.)
	DefenseReadout DefenseWith(const Character& member, const std::string& itemId);
	// What fx::Deal sees of a member's armour: PartyTarget's own Soak and Resist,
	// for a readout (`char`) that must not re-derive them.
	float PipelineSoak(const Character& member);
	float PipelineResist(const Character& member, DamageType type);
	// The skill a hand PARRIES with - its weapon's class (HandWeapon,
	// WeaponSkill), else `unarmed`, which a key in the hand is too (code-review
	// C39). What PartyTarget::Evasion reads, and what `char` prints beside each
	// hand.
	std::string_view ParrySkill(const Character& member, int hand);

	// Trains `avoid` on an evaded blow or the worn armor on a blunted one —
	// call once per RESOLVED attack against a member. A bolt the wind ward
	// turned was never rolled, and teaches nothing.
	void TrainDefense(Character& member, const fx::DamageEvent& ev);
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
	// `outcome`, when given, says WHY a cast failed - the spellbook keeps its
	// built spell when the caster merely lacked the mana.
	bool CastSpell(size_t member, std::span<const SpellSymbol> sequence,
				   int hand = -1, MagicSystem::CastOutcome* outcome = nullptr);
	// Same cast, referencing the recipe by catalog id (the hand-slot Magic menu
	// stores "cast:<id>" defaults). All the same gates apply — the member must
	// know the recipe's symbols and afford its mana. False on an unknown id.
	bool CastSpellById(size_t member, std::string_view id, int hand = -1);
	// The world in front of the party, as the cast services hand it to a spell
	// (DungeonWorld_Ahead.cpp; Spell/Spell.h CastServices says what each does).
	// Public so the dev console's `castsvc` can drive each one alone.
	FireAhead FireAheadOfParty() const;
	// Where that fire is: its square, and its wall (-1 = a brazier). False if
	// there is none.
	bool FireAheadCell(int& x, int& z, int& wall) const;
	// That fire when it is a WALL TORCH and the click (mx,my) lands on it.
	bool SconceUnderCursor(float mx, float my, float w, float h, int& x, int& z,
						   int& wall) const;
	bool SetFireAhead(bool burning);
	bool FlareFireAhead();
	// The haze that fire's smoke effects add to its square right now (0 = none):
	// the harness's view of a douse thinning away.
	float FireAheadHaze() const;
	// How far that fire is flared right now (0 = steady), and the ids of the
	// items lying in a square, space-separated: two more harness views, of a
	// gust on a fire and of a conjured item landing at the party's feet.
	float FireAheadFlare() const;
	std::string ItemIdsAt(int x, int z) const;
	// Every LIT item lying in the level, one string each - "<id> at <x>,<z>
	// charge <seconds|full>" - for the console's `torch floor`: the harness's
	// view of a torch burning where it fell, and of a thrown one keeping what it
	// had (code-review C447). Empty = none.
	std::vector<std::string> FloorTorchReport() const;
	// The `flooritems` readout: each item lying in square (x, z) - every square
	// for x < 0 - as `<x>,<z>: <id> slot <s> charge <c>`, and each thrown item in
	// the air as `<id> charge <c> at <x>,<z> over <x>,<z>`: where it is, in
	// squares, and the square it is over - the one a save made now writes it in
	// (SaveFlyingCargo), NOT where its flight will end. Harness views of what a
	// fumble knocked down and what a save holds; they build strings. A row of
	// the conjured pool (Item::conjured) ends ` conjured`.
	std::vector<std::string> FloorItemRows(int x, int z) const;
	std::vector<std::string> FlyingCargoRows() const;
	// The conjure service's drop (CastServices::dropAtFeet, `castsvc drop`):
	// a CONJURED item at the party's feet, through ConjureDrop's pool.
	void DropAtPartyFeet(std::string_view itemId);
	bool ShoveAhead(int cells);
	ProjectileSystem::Repelled RepelAhead(float power, int casterIndex);
	void BlastAroundParty(const ProjectilePayload& payload, SpellSymbol school,
						  int casterIndex);
	// A puff of `school`'s element just ahead of a cast's origin (its lane at the
	// eye) along `dir` - flame, dust, a breath of air, a splash of water - and a
	// brief shadowless glow.
	void HandPuff(SpellSymbol school, const Vec3& origin, const Vec3& dir);
	// A light spell's Hagalaz flare (DungeonWorld_SpellLight.cpp): a flash round
	// the party, the monsters within a few steps DAZZLED, the school's light once.
	void LightFlare(SpellSymbol school, float power, int casterIndex);
	// EARTH's light SET DOWN (Stonelight, Phase 6f): a glowing stone in the
	// party's square at `power` for `seconds` - part of this level's saved state -
	// that maps every square it reaches. A stone already in that square is
	// replaced; past the pool (kLightStones a level) the one nearest its end goes.
	void PlaceLightStone(float power, float seconds);
	// The stones on this level, one line each (the `lightstones` readout), and
	// clearing them all.
	std::vector<std::string> DescribeLightStones() const;
	void ClearLightStones() {
		for (const LightStone& s : m_lightStones)
			if (s.timeLeft > 0.0f) NoteCellCaster(s.x, s.z); // gone from the cubes too
		m_lightStones = {};
	}
	// The monster tracks on this level (6g): how many still show, and the
	// freshest few (the `tracks` readout); and wiping them.
	std::vector<std::string> DescribeTracks() const;
	void ClearTracks();
	// A fresh monster track on (x, z) going `dir` (the `tracks add` command).
	void AddTrack(int x, int z, Direction dir) { RecordTrack(x, z, dir, TrackMaker::Monster); }
	// What stamina regenerates at right now beyond its own rate: 1, or more in a
	// Tidelight (it SOOTHES) - for the `regen` readout.
	float StaminaRegenScale() const { return StaminaSoothe(); }
	// The whole spell registry (the Magic menu filters it by known symbols).
	std::span<const std::unique_ptr<Spell>> SpellDefs() const {
		return m_magic.Book().Defs();
	}
	const Spell* FindSpell(std::string_view id) const { return m_magic.FindSpell(id); }

	// Fired once when the last standing member goes down (Game ends the run).
	std::function<void()> onPartyWipe;

	// --- item pickup / drop (mouse-driven) ----------------------------------
	// Tries to pick up a floor item under the screen point (mx,my) in a w×h
	// viewport: an item is hit when the click ray, sampled at that item's own
	// visible height, lands in the quarter (the Medium 2x2 slot grid) the item
	// rests in — gated by reach (the cell is the party cell or orthogonally
	// adjacent) + seen. The top item (last in render order) wins. It is removed
	// from the floor and its catalog id returned (Game puts it on the cursor);
	// null if nothing pickable is under the cursor. The id is the kind's own, so
	// it outlives the call and the lift copies nothing (it runs in a guarded
	// frame). Pure query+remove - no satchel/knowledge side effects.
	const std::string* TryPickItem(float mx, float my, float w, float h,
								   float* charge = nullptr); // the lifted item's charge
	// The same pick WITHOUT the lift: the type of the floor item under the
	// cursor (the item details dialog's right-click), or null. The id lives in
	// the item's kind, so the pointer outlives the call.
	const std::string* ItemTypeUnder(float mx, float my, float w, float h) const;
	// The pick itself, shared by both: the index into m_items, or -1.
	int PickItemIndex(float mx, float my, float w, float h) const;
	// Drops a held item (catalog id): into an open niche the click lands in, or
	// onto the floor where the ray meets it, when a thing can rest on that square
	// (ItemCanRest - not a pit, a stairwell or a shut door's), in reach and seen
	// - snapped to the quarter slot nearest the hit point.
	// THROW OR DROP (ui-updates Phase 10, Grimrock's screen-height rule): a click
	// that meets no reachable floor - above the floor's horizon, on a wall,
	// beyond reach, over a hole - drops NOTHING and returns false, and the caller
	// throws.
	bool DropItemAt(const std::string& typeId, float mx, float my, float w, float h,
					float charge = -1.0f); // the item's charge goes down with it
	// The harness's hand on that click (the `drop` dev command): DropItemAt aimed
	// at the centre of square (x,z) on the floor, projected through the camera
	// as a pointer there would be, so it is refused by exactly what refuses a
	// player's drop. The item comes from nowhere, as `throw <item>`'s does.
	// `quarter` 0..3 (row * 2 + col, north-west first, SlotGrid.h) aims at that
	// quarter's centre instead: at the square's centre all four quarters tie,
	// and float noise picks one, where a harness that will click the item after
	// needs to know which.
	bool DropItemOnSquare(const std::string& typeId, int x, int z, int quarter = -1);
	// THROWING (DungeonWorld_Throw.cpp): a member throws an item (catalog id)
	// straight ahead down their quadrant lane - `member` < 0 = the party LEADER
	// (the cursor's throw), else that roster slot (a hand's `throw` use). False =
	// nobody threw (the thrower is down, or still recovering from their last
	// throw - throw_interval): the item stays where it was.
	bool ThrowItem(const std::string& typeId, int member = -1, float charge = -1.0f);
	// TORCH COMMANDS (DungeonWorld_Light.cpp; the hand menu's rows): what can be
	// done to the flame of an item in a hand - put a lit torch out, or light a
	// MAGICAL one, which takes no spell's fire and costs its holder mana
	// (balance.cat torch_light_mana per power_level). Each acts on member
	// `member`'s hand `hand`; false = nothing done (and, for want of mana, said).
	enum class TorchAct { None, PutOut, Light };
	TorchAct TorchActFor(const std::string& typeId);
	bool PutOutTorch(size_t member, int hand);
	bool KindleTorch(size_t member, int hand);
	// The thrower's wait, ticked down in Update (indexed by roster slot).
	float ThrowCooldown(size_t member) const {
		return member < m_throwCooldown.size() ? m_throwCooldown[member] : 0.0f;
	}
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
	// The runtimeId of the first monster at (cx,cz), or 0 - for editor selection and
	// INSPECTION ONLY. It counts the dead: a corpse lies in the list, undrawn once
	// its fall ends, and is still something to inspect or delete. Never a rule of
	// play or placement - a door asked it whether its doorway was clear and a
	// corpse jammed it for good (code-review C65); that is DoorwayOccupied.
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
	// cells, `turbidity` 0..1, `flameColor` the light + flame tint (kNoFlameColor =
	// the kind's). Both return false if no such sconce.
	bool TorchSettings(int cx, int cz, Direction wall, bool& lit, float& brightness,
					   float& turbidity, Vec3& flameColor) const;
	bool SetTorchSettings(int cx, int cz, Direction wall, bool lit, float brightness,
						  float turbidity, const Vec3& flameColor);
	// A floor brazier on (cx,cz)? Plus its per-instance light/smoke settings (live
	// on Set). Both return false if no brazier is there.
	bool BrazierAt(int cx, int cz) const;
	bool BrazierSettings(int cx, int cz, bool& lit, float& brightness, float& turbidity,
						 Vec3& flameColor) const;
	bool SetBrazierSettings(int cx, int cz, bool lit, float brightness, float turbidity,
							const Vec3& flameColor);
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
	// The per-TYPE map facing-arrow flag (catalog facing_arrow, default 1) of a
	// DECORATIONS type (the decoration inspector's checkbox; doors and the rest
	// draw no arrow): no-load queries for the marker/browse paths, and the live
	// half of the inspector checkbox's type edit (Game writes the catalog field +
	// saves).
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
		// The model's size IN METRES (kUnit x modelscale), which is what the
		// preview camera frames - the model itself is authored in units, so a
		// bare modelscale drew a 1.9 m skeleton at 0.77 m. Capped so a big
		// creature still fits the pane (MonsterPreviewFor).
		float scale = 1.0f;
		float modelYaw = 0.0f; // render-time facing fixup, so the preview matches in-world
		Vec3 pivot{}; // the rig root's rest point (MonsterKind::rigRest): the preview's centre
		// What a still preview plays: the kind's first Idle clip, else the
		// model's first clip (which for the skeleton kit is a lie-on-the-floor
		// spawn, so it must not be the default when an idle exists).
		std::string idleClip;
	};
	MonsterPreviewData MonsterPreviewFor(const std::string& type); // force-loads the kind
	// How far a ONE-SHOT clip (a death, a rise) may carry a monster's rig root
	// sideways, in model units (a square at modelscale 1): every monster animator,
	// and the previews of one, run with Animator::LockRootTravel(this). The
	// skeleton kit's deaths travel up to 0.6, which laid a body half a square
	// into a wall or the party's square. Its hips sit 0.34 below the crown and
	// ~0.42 above the toes, so a body whose hips end within 0.08 of where the
	// fall began lies inside its own square's 0.5 half-width either way round.
	static constexpr float kMonsterRootReach = 0.08f;
	// THE monster Animator: every one made for a creature's model - a spawn, a
	// kind's map-icon pose, the editor's two previews, the asset picker's tile
	// and preview - comes from here, already holding its root travel to
	// kMonsterRootReach. The rule above used to be each call site's to remember,
	// and the asset picker's looping preview forgot (code-review C395).
	static anim::Animator MonsterAnimator(const assets::SkeletonData* skeleton,
										  const std::vector<assets::AnimationClipData>* clips);
	static anim::Animator MonsterAnimator(const assets::ModelData& model) {
		return MonsterAnimator(&model.skeleton, &model.clips);
	}
	// The box `model`'s vertices fill when the scene shader poses them with
	// `palette` (CPU skinning, the shader's weighted sum; an empty palette = as
	// modelled). `nodeBaked`: each mesh's node transform first
	// (assets::NodeTransform - none for a skinned mesh), as
	// BuildMultiMaterialModel bakes it into the uploaded vertices - pass whether
	// the geometry drawn came from there. False for an empty model.
	static bool PosedBounds(const assets::ModelData& model, std::span<const Mat4> palette,
							bool nodeBaked, Vec3& lo, Vec3& hi);
	// Whether a monster type's model file exists (so the editor can guard the
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
	// The same by item TYPE, into a caller-owned buffer, for the play-mode item
	// details dialog (docs/ui-updates-plan.md P3). It opens on a click in a
	// guarded frame, so it fills rather than returns a vector: returns how many
	// submeshes were written (0 = nothing to show, or an unknown type). `pose` is
	// how to stand it up for its turntable spin (see FillItemPreview).
	size_t ItemPreviewForType(const std::string& type, std::span<gfx::PreviewSubmesh> out,
							  Vec3& fitMin, Vec3& fitMax, Mat4& pose);
	// What the details dialog says about an item type (Game/ItemDetails.h).
	// False for a type no catalog defines.
	bool ItemDetailsFor(const std::string& type, ItemDetails& out);
	// Whether the model cache holds `file` - a kind built on it has opened it
	// (`modelfile`, which says which file a type's loader really opened).
	bool ModelFileLoaded(const std::string& file) const { return m_modelCache.contains(file); }
	// An item type's MODEL as it draws (`modelfile`; code-review C301): the set
	// its entry's `texture` dresses it in ("" = none), how many parts the model
	// has and how many wear that set, and for the first of those the albedo it
	// was HANDED - `drawn` by the last rendered frame's draws (Sub::drewAlbedo,
	// stamped by DrawPart: a floor item, a thrown one, its icon bake), `previewed`
	// by FillItemPreview (the details dialog's parts) - each one word: "WxH" (its
	// set's albedo as it is now, at that size), "none" (no albedo: the flat look
	// C301 was about), "stale" (an albedo that is not its set's now), and for
	// `drawn` "undrawn" (no draw on the last frame - a headless run never draws).
	// Builds the kind, like ItemPreviewForType; nullopt for a type no catalog
	// defines or one with no model of its own (the tablet).
	struct ItemModelLook {
		std::string set;
		size_t parts = 0, wears = 0;
		std::string drawn = "undrawn", previewed = "none";
	};
	std::optional<ItemModelLook> DescribeItemModel(const std::string& type);

	// --- level transitions (P6 multi-level) ---------------------------------
	// Swaps the active level to `stem` and resets all per-level state (map,
	// entities, fog, monster/decoration/fire instances, surface chunks, shadow
	// cache), keeping the shared asset caches (kinds, prop textures) and the
	// party object. The heavy rebuild is NOT done here: the caller re-stages the
	// world's load tasks (AppendLoadTasks) behind a loading screen, then calls
	// PlacePartyAt for the arrival cell. Drains the GPU first. Relies on m_map /
	// m_entities being move-assignable into the existing objects so Party's map
	// reference stays valid.
	// `stashCurrent` saves the level being left's DYNAMIC state into the
	// in-memory per-level store (so returning restores its fog/progress); pass
	// false when that state is a throwaway baseline (e.g. loading a save onto a
	// different level). Its EDITOR layers are another matter: whatever the caller
	// passes, the map is stashed when it differs from its file (ActiveMapEdited)
	// and the records when m_entsDirty says so - unsaved editor work is never a
	// throwaway (code-review C298), and a level only visited is not stashed at
	// all, so savemap does not rewrite it (C308). StashEditedLayers.
	void BeginLevelLoad(const std::string& stem, bool stashCurrent = true);
	// Places the party at a cell + facing (the stair arrival point), revealing it.
	void PlacePartyAt(int x, int z, Direction facing);
	// Restores the active level's saved dynamic state (fog + entity diffs) from
	// the per-level store, if it was visited before, then drops that entry (the
	// live state is now authoritative). Call after a level's load completes (the
	// entity diffs need the monsters built). A no-op for a first visit.
	void ApplyActiveSnapshot();
	// PARKS the active level: the party has walked OUT of it (to the world map)
	// while it stays loaded underneath. Its state is stashed NOW, exactly as a
	// stair would stash it, because whatever replaces it next — a doorway, a
	// random encounter, a load - does so without stashing its dynamic state
	// (those paths replace a throwaway baseline, and cannot tell one from a level
	// the party left). Its editor layers go by the stair's rule too: only what
	// differs from the files. Idempotent; any level load or install ends it, and
	// so do a save loaded and a game begun on this same level, in place - which
	// drop what it stashed of the editor layers, the live ones being the level
	// again (Unpark).
	void ParkActive();
	bool Parked() const { return m_parked; }
	// The LAST level parked and where the party stood in it - kept when an ambush
	// replaces it (an encounter is never parked), so a save on the world map
	// after one still names a real level (code-review C299; Game::SaveGame). A
	// new game or a load forgets it; "" = nothing parked since.
	struct ParkedPose {
		std::string stem;
		int x = 0, z = 0, facing = 0;
	};
	const ParkedPose& LastPark() const { return m_lastPark; }

	// A pending level transition: the destination level + arrival cell/facing,
	// raised when the party steps onto a stair (see the .map "stairs" records).
	struct LevelTransition {
		std::string level;
		int x = 0, z = 0;
		// Unset for a stair: the party faces whatever the stair it lands on
		// faces (ArrivalFacingAt), which is only known once that level is
		// loaded. A pit fall sets it - you land looking the way you fell.
		std::optional<Direction> facing;
		// This stair leaves the DUNGEON rather than changing level
		// (stairs.cat `exit = 1`). A flag rather than a reserved level name
		// like "world": a name in the same namespace as real stems is one
		// rename away from colliding with a level someone authored.
		//
		// `level` then carries the WORLD LOCATION this exit surfaces at, since
		// a dungeon may have several ways out and the back stairs do not come
		// up at the front gate. It reuses the record's existing `dest=` field
		// rather than inventing a second one: on an exit stair, the
		// destination simply is a location instead of a level. Empty means
		// "wherever the party came in", which is what a single-exit dungeon
		// wants and what needs no authoring at all.
		bool toWorld = false;
	};
	// A pit fall is in progress (the step glide onto the pit, then the camera
	// drop). Movement is swallowed while it runs — the keyboard path gates in
	// Update, the HUD arrow path gates in Game's onMoveAction callback.
	bool Falling() const { return m_falling; }

	// Returns and clears a transition raised since the last call (the party
	// stepped onto a stair this frame); nullopt otherwise. Game polls this after
	// the world Update and drives the actual swap (BeginLevelTransition), so the
	// map never changes mid-Update.
	std::optional<LevelTransition> ConsumeLevelTransition();
	size_t MonsterCount() const { return m_monsters.size(); }
	// One human-readable line per monster group (id, count, kinds, cells#slot) for
	// the dev console `groups` command — the Phase-3 group model's reader.
	std::vector<std::string> GroupsReport() const;
	// A fingerprint of the active level's surface geometry, rebuilt from the map
	// exactly as a full bake builds it: one FNV-1a hash per surface over every
	// chunk's variant, chunk index and vertex/index bytes. For the `geomhash`
	// command - a change that must not move a single vertex (a refactor of the
	// variant resolve, a new cell state no cell uses yet) is checked by the hash
	// coming out identical before and after.
	//
	// `layout` / `liveLayout` answer a different question: do the chunks
	// actually UPLOADED match what a fresh bake would upload? Each hashes the
	// sorted (surface, chunk, variant, index count) set - `layout` from the
	// fresh build, `liveLayout` from the live chunks. The live meshes keep no
	// CPU copy, so this is shape not bytes, but a chunk an edit forgot to
	// rebuild keeps its old variant buckets and shows up as the two disagreeing.
	struct GeometryPrint {
		u64 walls = 0, floors = 0, ceilings = 0;
		u64 layout = 0, liveLayout = 0;
		size_t vertices = 0;
	};
	GeometryPrint GeometryFingerprint() const;

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
	// `includeLive` = false leaves the LIVE level out: the party is on the world
	// map, so the level under it is either PARKED (already in the store, and
	// written from there) or a baseline it never entered, which has no state.
	void CaptureState(SaveData& out, bool includeLive = true) const;
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
	// A wall on the level's START square is refused (RaisesStart), here and in
	// EditCellRemote: the writer emits 'P' whatever the square is, so a painted
	// start reverted on reload, and a resize could then crop it (code-review
	// C348). The editor's brushes ask first, to say so.
	void EditCell(int x, int z, Cell cell);
	// True when setting square (x,z) of `map` to `cell` would close its start.
	static bool RaisesStart(const DungeonMap& map, int x, int z, Cell cell) {
		return cell == Cell::Wall && x == map.StartX() && z == map.StartZ();
	}

	// A themes.cat entry's member per surface (its `wall` / `floor` / `ceiling`
	// ids, empty = none). The one reader of that format, for the map parser's
	// FixtureTypes and for the brush that paints a theme.
	static ThemeMembers ThemeMembersOf(const CatalogEntry& entry);

	// Which surface a variant edit targets (DungeonMap's Surface - spelled out,
	// since inside this class a bare `Surface` names the chunk-list struct).
	using SurfaceSel = game::Surface;
	// Pins a cell's wall/floor/ceiling texture variant to a palette index
	// (the variant index into the level's surface palette), then rebuilds like
	// EditCell. A wall variant lives on the SOLID cell, floor/ceiling on the
	// walkable one; the wrong cell type is a no-op.
	void EditVariant(int x, int z, SurfaceSel sel, int variant);
	// A surface type's SWATCH - the palette's rows, a theme's member rows, and
	// the editor map's textured cell fill: a small LINEAR thumbnail of its
	// texture set, else null (the flat colour / ink) until one has loaded. NOT
	// the level's loaded albedo, which it used to borrow: that is sRGB for the
	// scene, and the sprite batch drew it far darker than the material looks
	// (code-review C158; AssetUtil.h LoadTextureThumb says why). Any id at all,
	// so a browsed level's foreign palette and the Catalogue view show too.
	// Draw-safe: it never loads - a set not tried yet is ASKED FOR, and the
	// next Update's LoadWantedSwatches loads it.
	const gfx::Texture* SurfaceSwatchForId(SurfaceSel sel, const std::string& id) const;
	// Loads that thumbnail now (once per set, found or not) - the asset
	// picker's loader, trimmed to swatch size - for a list about to show the
	// type. It uploads, which drains the GPU: call from Update, never
	// mid-frame. True when it went to disk (so a caller can pace itself), false
	// when there was nothing to do.
	bool LoadSurfaceThumb(SurfaceSel sel, const std::string& id);
	// Loads up to `max` of the swatches a draw asked for (SurfaceSwatchForId).
	// Game::Update calls it every frame, ahead of every editor dialog, so a
	// swatch shows a frame or two after it is first drawn wherever it is drawn.
	size_t LoadWantedSwatches(size_t max);
	// The set and stem a swatch texture was loaded from (empty when it is not
	// one) - the swatch brightness survey (`editor palette swatches`).
	std::pair<std::string, std::string> SwatchSource(const gfx::Texture* swatch) const;

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
	// The same for a surface THEME (themes.cat `id`): each member joins
	// its surface's palette on level `stem` (skipping any whose assets are
	// missing), the level gains a slot for the theme, and the return is
	// the VARIANT a cell stores to reference it (DungeonMap::ThemeVariant) - or
	// -1 when the project defines no such theme.
	int EnsureThemeVariant(const std::string& stem, const std::string& id);
	// After a theme's DEFINITION changed (the type editor's Save): every
	// level whose squares use it takes the new members - each enrolled in that
	// level's palette first - so it repaints with nothing re-painted. Levels not
	// in memory are asked through their read-only copies, so only the ones that
	// USE it get stashed (and rewritten by the next savemap). The active level's
	// geometry is re-stamped when the editor closes, as after an undo.
	void RefreshTheme(const std::string& id);
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
	// longer exists. A rename also retypes what the levels' HELD dynamic states
	// carry whole - a placed monster or a drop with no record, a smashed prop -
	// which the respawn after it lays back. The ACTIVE level's live placements
	// count only once the caller has synced them into records
	// (SyncActiveRecords); it re-spawns live objects afterwards
	// (HoldActiveState / RestoreHeldState) and saves (`savemap`) to persist.
	TypeUsage SweepTypeRefs(const std::string& catalogKey, const std::string& id,
							const std::string* newId = nullptr);
	// --- generator support ----------------------------------------------------
	// Could a stair stand on (x,z) of `stem`? Walkable, and nothing already
	// there that a stair cannot share a square with. Public so the generator's
	// seam can find a cell that suits BOTH levels of a link before authoring it.
	// Reads the level (LevelForReading), as every cross-level query here does.
	bool CellFreeForStair(const std::string& stem, int x, int z);
	// Where on `stem` a stair DOWN to a new floor should go: a square a stair
	// can stand on, as many steps from the level's start as its floor reaches
	// - the way on is found at the far end, the generator's own rule for its
	// exit. {-1,-1} when the level has no such square. The new floor is then
	// BUILT AROUND this square rather than the square searched for afterwards,
	// because a stair needs the same (x,z) on both levels and two independent
	// layouts rarely share a free one (docs/level-building.md P1).
	std::pair<int, int> FarthestStairCell(const std::string& stem);
	// Any level's static map, for READING - the live one for the active level,
	// else its stash, else its file read-only (LevelForReading; it used to make
	// a stash, which the next savemap rewrote: C307). The generator copies a
	// chosen level's surface palette from it.
	const DungeonMap& MapOf(const std::string& stem) { return *LevelForReading(stem).map; }

	// Replace a level's CONTENT wholesale — the generator's regenerate.
	//
	// The active level is replaced IN PLACE (move-assign, respawn, defer the
	// surface rebake) rather than by a level transition, because a transition
	// clears the undo history and the whole promise of a destructive regenerate
	// is that Ctrl+Z brings the old level back. Any other level replaces its
	// stash, which is the ordinary remote-edit path. Bracket the call in
	// Begin/CommitUndoStep and it is one undo step like any edit.
	// Takes the two files' PATHS rather than parsed objects so the fixture-type
	// table (a private detail of this class) stays here, and the caller only has
	// to know how to write the text.
	bool InstallLevelFromFiles(const std::string& stem, const std::string& mapPath,
							   const std::string& entPath);
	// The same install, from TEXT that was never a file: a generated random
	// encounter (docs/world-map.md). `stem` names it for error messages and
	// for CurrentLevel; it is never a path and never written. Unlike a
	// regenerate it CLEARS the undo history first: the level it replaces is
	// not this one, whatever the stem says (code-review C297).
	bool InstallLevelFromText(const std::string& stem, std::string_view mapText,
							  std::string_view entText);

	// --- validation (the editor's playability check) --------------------------
	// Runs Game/Validate.h over EVERY level in the project - the active one from
	// its live state, an edited one from its edit stash (unsaved edits count),
	// and any other from a READ-ONLY copy of its files (m_readOnlyLevels).
	// Whole-project on purpose: a key may legitimately live a floor away from
	// its door, so a per-level check would report a correct dungeon as broken.
	//
	// It must NEVER create an edit stash. It used to (EnsureMapStash), and a
	// stashed level is one `savemap` rewrites - so pressing Check made the next
	// save rewrite every level in the project, untouched ones included, and put
	// all of them in every undo snapshot. Live validation runs this after every
	// edit, which would have made that true of every session.
	//
	// NOT const: the read-only copies are parsed on first use and cached.
	// `world` is the OPTIONAL world tier, passed in rather than held: the world
	// sits ABOVE this class (docs/world-map.md) and DungeonWorld is the
	// simulation of one level, so it gathers the world's view for the checker
	// without owning it. Default = a project with no overworld.
	std::vector<validate::Issue> Validate(const validate::WorldView& world = {});

	// The static map of `stem` AS IT IS, for READING: the live map when it is
	// the active level, else its edit stash, else a read-only copy of its files -
	// Validate's walk, and like Validate it never creates a stash. Null when the
	// level is neither and a file of it is missing (the parse would assert).
	// `levelcheck` reads every level's surface palette through it. NOT const for
	// Validate's reason; the map stays put until that level is next re-parsed.
	const DungeonMap* LevelMapAsItIs(const std::string& stem);

	// Rebuilds the live dynamic objects from the current records — the tail of
	// an undo restore, reused by a type Save and a type Rename between
	// HoldActiveState and RestoreHeldState (on its own it puts the level back to
	// its records and loses everything else: code-review C311).
	// Surfaces are untouched (a rename doesn't move geometry) EXCEPT features,
	// wall and surface alike, whose tiles are stamped into the chunks:
	// `geometryToo` re-files their meshes under the catalog's ids as they are
	// NOW (LoadFeatureMeshes - a renamed type's mesh was still filed under its
	// old id, so the next bake drew a plain block in its place) and sets the
	// geometry dirty flag so the editor's FlushGeometry re-stamps on the way out.
	void RespawnFromRecords(bool geometryToo = false);
	// The catalogs whose types are stamped INTO the surface chunks rather than
	// placed on them - what `geometryToo` above is for.
	static bool StampedIntoSurfaces(std::string_view catalogKey) {
		return catalogKey == "wallfeatures" || catalogKey == "surfacefeatures";
	}
	// THE RECORDS BROUGHT LEVEL WITH THE LIVE LEVEL (code-review C311). On the
	// active level the LIVE lists are the truth for monsters and props: a
	// placement adds an instance and nothing else, an inspector edits the
	// instance (interim, until placement is record-first - P9). This writes them
	// back: the props into the map's decoration records, every monster into a
	// .ent record (LiveMonsterRecord - one the editor placed gets a record, and
	// with it an id; a record whose monster is gone is dropped), the records
	// marked edited when one changed. A no-op while the level is PARKED: what
	// entering it again reads is the park's stash, not the live lists.
	void SyncActiveRecords();
	// A respawn from the records that KEEPS what the editor and play did to the
	// level, for a type Save and a type Rename (not an undo, which wants the
	// records it restores and nothing else): HoldActiveState syncs the records
	// and stashes the dynamic state (StashActive), the caller retypes and
	// respawns, and RestoreHeldState lays that state back on the respawned
	// objects (ApplyActiveSnapshot) - the dead stay dead, an opened door open, a
	// dropped item where it lies. Both are no-ops while the level is parked.
	void HoldActiveState();
	void RestoreHeldState();

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
	// `share` joins a square monsters of the same size already hold, in its next
	// free slot (FreeSlotInCell), as a level's records may stand a bone swarm's
	// four in one square; the editor's brush keeps one monster a cell. Only the
	// console's `spawn ... share` asks: tools\CombatTest.py's MELEE check needs
	// two and four swarms in one square (code-review C33).
	bool AddMonster(const std::string& type, int x, int z, Direction facing,
					bool share = false);
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
	// (x,z) along `axis` (0 = X, 1 = Z) and re-stamps the two flanking faces. The
	// editor derives the axis from the wall face pointed at, so a free-standing
	// block can be bored either way. False if that axis does not open into floor
	// at both ends, or the block is bored already. (Its auto-axis twin went with
	// code-review C310: nothing called it.)
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
	// `facing` is the doorway axis. The no-argument form derives it (the dev
	// console and any non-editor caller); the editor passes the one its ghost
	// already showed, so the door lands on the previewed axis rather than on a
	// second derivation that merely happens to agree.
	bool AddDoor(const std::string& type, int x, int z);
	bool AddDoor(const std::string& type, int x, int z, Direction facing);
	// Click interaction on the door directly AHEAD of the party (arm's reach).
	//
	// THE CLICK MUST LAND ON THE OPENER, not merely on the door: a chain is
	// pulled and a pad is pressed, and a door that worked from a click anywhere
	// in its general direction would make the hand-hold decorative. So the
	// screen ray is tested against the opener itself and a miss falls THROUGH
	// (returning false) to the button check, exactly as if the door were not
	// there — silence, because a miss is not an error.
	//
	// The one case that still answers a click anywhere is a door with NO opener:
	// there is nothing to hit, so the refusal message is the only way to say why
	// nothing happens. A monster standing in the doorway jams a closing door. A
	// keyed (key=) closed door opens only while some member carries that item
	// (not consumed; wired buttons bypass both the lock and the opener —
	// mechanisms need neither a key nor a hand).
	bool ToggleDoorAhead(float mx, float my, float w, float h);
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
	// `slot` is the Medium quarter (0..3) it should occupy, or -1 to fall back to
	// the nearest-free-to-centre pick. A given slot is PERSISTED as a `slot=`
	// record param: without that the quarter would be re-derived from the cell
	// centre on the next load and the editor's placement would silently move,
	// which would make previewing the quarter a promise the reload breaks.
	bool AddItem(const std::string& type, int x, int z, int slot = -1);
	// Click interaction: presses the button on the party's OWN cell mounted on
	// the wall the party faces. False if there isn't one.
	bool PressButtonFacing();
	// A wall torch, off its bracket and back (DungeonWorld_Fires.cpp). The party
	// faces it from its square and the click lands ON it. Taking leaves the bare
	// bracket and puts the torch - lit if it burned - in the leader's free hand,
	// else on `cursor` (which must be empty). Mounting puts the item `itemId` (a
	// torch, lit or not) into an EMPTY bracket. True if it happened. The bracket
	// REMEMBERS the torch and its `charge` (WallSconce::torch), so a magical or
	// half-burnt torch comes back off it as it went in.
	bool TakeTorchAhead(float mx, float my, float w, float h, HeldItem& cursor);
	bool MountTorchAhead(const std::string& itemId, float mx, float my, float w, float h,
						 float charge = kNoCharge);
	// The same acts on a named sconce, with no click (the dev console's `torch`).
	bool TakeTorchAt(int x, int z, int wall, HeldItem& cursor);
	bool MountTorchAt(int x, int z, int wall, const std::string& itemId,
					  float charge = kNoCharge);
	// The torch in the sconce on (x,z)/`wall` as it would come off it (its unlit
	// id), or "" when there is none (no sconce, an empty bracket).
	std::string_view SconceTorch(int x, int z, int wall);
	// Empties or refills the sconce on (x,z)/`wall` (the map record + the live
	// fire); `burning` = the refilled torch is lit. False for a kind with no
	// bare bracket to show (fixtures.cat `empty_model`) or no change.
	bool SetSconceEmpty(int x, int z, int wall, bool empty, bool burning = false);
	// Button instance surface for the inspector: presence + wiring, and the
	// live/record edit (in-memory until savemap). `target` is the door/niche
	// name it toggles; `needs` the flag it waits on (flag=); `sets` + `op` what
	// a press does to a flag (sets= / clears= / toggles=).
	struct ButtonEdit {
		std::string target;
		std::string needs;
		std::string sets;
		FlagOp op = FlagOp::None;
	};
	bool ButtonSettings(int x, int z, ButtonEdit& out) const;
	void SetButtonSettings(int x, int z, const ButtonEdit& in);
	// Distinct non-empty door names on the ACTIVE level, for the inspector's
	// Target dropdown (buttons only reach doors on their own level).
	std::vector<std::string> DoorNames() const;
	// Distinct non-empty niche names on the ACTIVE level — the button inspector's
	// Target dropdown lists these alongside door names (a button reveals either).
	std::vector<std::string> NicheNames() const;
	// A button targeting `name` flips every niche with that name open/closed and
	// swaps in the walls pre-built for it (the secret-niche reveal; see
	// "Pre-built niche walls" below). False if none matched. Allocates nothing:
	// it runs in the press's frame, which the allocation guard arms (C211).
	bool ToggleNichesNamed(std::string_view name);
	// What the pre-built niche walls hold, one line per chunk a lever-named niche
	// reaches: its names, the look on show, how many are held (`niche looks`).
	std::vector<std::string> NicheLooksReport() const;
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
	// A niche's POCKET as a click target (code-review C258): a ball this far
	// above NicheItemPos and this wide, in UNITS (x kUnit at the test) - what
	// lies in it is picked by it, and a held item is dropped into it.
	static constexpr float kNichePocketRise = 0.14f;
	static constexpr float kNichePocketRadius = 0.16f;
	// Opens or shuts the (x,z)/wall niche in play, re-stamping its wall - a
	// lever's reveal without the lever (the `niche` dev command). False if
	// there is no niche there.
	bool SetNicheOpen(int x, int z, Direction wall, bool open);
	// THE PICK PROBE (`pickprobe`, code-review C258 / C359): every click target
	// in reach - each floor item at its drawn height across its quarter, a niche
	// item, the door ahead's opener and the wall torch ahead at their drawn
	// middles - shot at through the screen with the real click tests, one line
	// each, then a verdict line `pickprobe RESULT=PASS|FAIL|NONE ...`.
	std::vector<std::string> ProbePicks() const;
	// Places an item into the (x,z)/wall niche (record-backed, piles at the
	// pocket). False if there is no niche there. The editor's item brush lands
	// here when its placement resolves INTO a niche (Placement::niche), and the
	// `niche ... put` dev command; AddNicheItemRemote is a browsed level's twin.
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

	// Door instance surface for the editor's inspector. DoorSettings reports the
	// door on (x,z) (false = none); SetDoorSettings applies the edit to its .ent
	// record's params (in-memory until savemap, like every editor edit) and the
	// live door. `open` is the AUTHORED state (initialOpen, the record's open=):
	// when an edit changes it the live leaf follows if it can - but a wrecked leaf
	// stays open (the record still takes the close), and a close is REFUSED
	// outright, record and all, while anyone stands in the doorway, since the
	// editor's Save writes a monster where it stands and a shut record would shut
	// the door on it at the next load; both say why (code-review C356). Read
	// DoorSettings back for what was kept. `name` is what a button's target=
	// points at (ToggleDoorsNamed).
	//
	// The OPENER fields are three-state on purpose and the empty one is not the
	// same as "none": empty INHERITS the door type's `opener`, "none" is this
	// placement overriding it to have no hand-hold at all. Collapse them and the
	// type's default becomes unsayable the moment an instance is edited once.
	struct DoorEdit {
		bool open = false;
		std::string key;         // items.cat id required by hand ("" = none)
		std::string flag;        // flags.cat id it waits on ("" = none)
		std::string name;        // button-target id ("" = unwired)
		std::string opener;      // "" = inherit type, "none", or a doors.cat id
		std::string openerSide;  // "" = inherit type, else "left" / "right"
		// Motion shaping, same three-state rule: "" inherits, else an
		// EaseShape name. The leaf's pair and the opener's pair are separate
		// because they are separate motions — a slab that grinds shut can hang
		// off a chain that snaps back.
		std::string easeIn, easeOut;
		std::string openerEaseIn, openerEaseOut;
		// How long the leaf's full throw takes. The same three-state rule,
		// spelled the way a NUMBER can spell it: 0 inherits the type's
		// `open_seconds`, because a door that takes no time to open is not a
		// thing anyone wants, so zero is free to mean something else.
		//
		// One number for both directions. A separate close time is easy to add
		// and nothing yet wants one — no door in the game shuts differently from
		// how it opens, and the pair of easing curves already carries the
		// asymmetry that a stone slab actually needs.
		float seconds = 0.0f;
		// REPORTED by DoorSettings, never applied: the leaf as it stands in play
		// and whether it is wrecked - what an `open` that did not take left.
		bool live = false;
		bool broken = false;
	};
	bool DoorSettings(int x, int z, DoorEdit& out) const;
	void SetDoorSettings(int x, int z, const DoorEdit& in);
	// The doors.cat id of the door on (x,z) ("" = none). The inspector needs it
	// to look up what the TYPE would give, which is what its Default rows name.
	std::string DoorTypeAt(int x, int z) const;
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
	// The stair inspector's seam (active level): the stair on (x,z), false if
	// none; and turning it - the record AND its prop, so the flight turns in
	// the 3D view at once. The paired half on the other level is untouched.
	bool StairSettings(int x, int z, StairLink& out) const;
	bool SetStairFacing(int x, int z, Direction facing);
	// The flag this half waits on (flag=, "" = none); the far half is its own.
	bool SetStairFlag(int x, int z, const std::string& flag);
	// Repoints an EXIT at a world-map location ("-" = nowhere yet). Refuses a
	// paired stair: its dest is a level and its pair's position, which the
	// inspector deliberately does not let one half change.
	bool SetExitDest(int x, int z, const std::string& location);
	// Where the party faces on ARRIVING at (x,z) of the active level by any way
	// in - a stair, a doorway, the game's opening, `play`: the facing of the
	// stair standing there (StairLink::facing), else south. A save load and a
	// pit fall bring their own facing and do not ask.
	Direction ArrivalFacingAt(int x, int z) const;
	// The stair prop's mesh(es), for the inspector's preview pane.
	std::vector<gfx::PreviewSubmesh> StairPreviewSubs(int x, int z) const;

	// --- moving placed things (the editor's drag-and-drop; DungeonWorld_Move.cpp)
	// Play-test #2 (Michael, 2026-09-28): with no brush armed, a left-drag picks
	// up the TOP thing on a square and drops it on another; dragging again digs
	// through what was under it. A move changes the object IN PLACE - its
	// patrol, its door name, its lever wiring all come along - where an erase
	// and a re-place would have lost them. Active level only: a browsed level
	// has no live instances to move (the inspectors' rule).
	struct MoveTarget {
		// The stacking order, top first: what stands on a square before what is
		// fixed to it, and the square's own fabric (a stair, a floor recess) last.
		enum class Kind { None, Monster, Item, Decoration, Button, Door, Brazier,
						  Sconce, Stair, Feature };
		Kind kind = Kind::None;
		int x = 0, z = 0;       // where it was picked up
		u32 runtimeId = 0;      // Monster
		int index = -1;         // Item / Decoration / Button: the live vector slot
		int id = -1;            // Door: its .ent record id
		Direction wall = Direction::North; // Sconce: the wall it hangs on
		bool ceiling = false;   // Feature: a ceiling feature, not a floor one
		std::string label;      // display name, for the message line
	};
	// What a drag starting on (x,z) would pick up; Kind::None when nothing on
	// the square can move (a niche or a window is carved into the wall and stays).
	MoveTarget TopMovableAt(int x, int z) const;
	// Moves it to (tx,tz). Every kind applies the rules its placement does (a
	// door needs a doorway, a lever a wall, a stair a free square on BOTH floors)
	// and a refusal says why on the message line. A STAIR takes its paired half
	// with it, and any way IN that landed on it - a world-map doorway's arrival
	// square, the game's opening - follows it (both undoable: see
	// SetOpeningForUndo). The caller brackets it as one undo step.
	bool MoveObject(const MoveTarget& t, int tx, int tz);
	// The project's OPENING (where a new game starts), borrowed like the world so
	// a stair move that carries it along is undone with everything else. Null =
	// no opening to keep in step.
	void SetOpeningForUndo(std::string* level, int* x, int* z) {
		m_openLevel = level;
		m_openX = x;
		m_openZ = z;
	}
	// True once a move has changed the opening since the last call - the owner
	// then writes project.ini on the next save, which is where it lives.
	bool ConsumeOpeningMoved() { return std::exchange(m_openingMoved, false); }

	// --- resizing a level (the editor's edge drag; DungeonWorld_Resize.cpp) ---
	// Play-test #3 (Michael, 2026-09-28): drag the map's EDGES. The new level is
	// the window [x0,x1) x [z0,z1) in the level's CURRENT coordinates: past the
	// old edges grows (new squares are rock), inside trims. A trim that would cut
	// off floor - and so anything standing on it - or a window bored through the
	// rock is REFUSED, and the message line says what is in the way ("nothing is
	// ever silently deleted"). Moving the left or top edge renumbers every
	// square, so everything that names one follows: the paired half of each
	// stair on the neighbouring floor (refused if it cannot), every other stair
	// pointing in, the doorways that land here, the opening, and the party.
	// Works on a browsed level too (it replaces the stash). The caller brackets
	// it as one undo step.
	bool ResizeLevel(const std::string& stem, int x0, int z0, int x1, int z1);

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
	// An item INTO the (x,z)/wall niche of a browsed level: a record with
	// `niche=<wall>`, as AddNicheItem writes on the active one (code-review
	// C351). False if the stash's map has no niche on that face.
	bool AddNicheItemRemote(const std::string& stem, const std::string& type, int x,
							int z, Direction wall);
	// A window bored through solid block (x,z) of a browsed level, along `axis`
	// - AddBore's twin, on the level's stash. The bore brush used to call
	// AddBore whatever level was viewed, boring the ACTIVE level's block at the
	// same square (code-review C310).
	bool AddBoreRemote(const std::string& stem, const std::string& type, int x, int z,
					   int axis);
	// The erase ladder for a remote cell, mirroring the live tool: stair (pair
	// removed too) → one monster/door/button/item record → one decoration
	// record → fixture → niche → bore → surface feature → reset the cell's
	// surface variants, messaging what it did. Returns whether anything changed
	// (the reset finds nothing to reset on a plain square).
	bool EraseRemote(const std::string& stem, int x, int z);

	// Saves every level with unsaved edits: the active one (SaveLevel) plus
	// each level with a stashed map or .ent (WriteStashedLevel). Returns the
	// stems written.
	std::vector<std::string> SaveAllLevels();
	// The text `savemap` WOULD write for level `stem`, WITHOUT writing it - a
	// world copy puts it in the new world's folder, so the unsaved edits come
	// across and the world they were made in is not saved behind your back. A
	// layer this world holds no edit of comes back EMPTY: its file on disk is
	// the truth, and the caller copies that. '\n'-joined, like the writers.
	void LevelTextFor(const std::string& stem, std::string& mapText,
					  std::string& entText) const;
	// A dynamic layer as the .ent text the writers produce (one record a line):
	// how an edit made to a parsed copy goes back in (Game::PopulateViewedLevel).
	static std::string EntTextOf(const std::string& stem, const DungeonEntities& ents) {
		return StashedEntText(stem, ents);
	}

	// Renames a level's world-side state: moves the .map/.ent files, rekeys
	// the three per-level stashes (+ the active stem), and repoints every
	// stair dest= across all levels, writing each level that has one at once
	// (a level with none is only READ, never stashed: C307). Drops the undo
	// history (its snapshots are keyed by the old stem). The MANIFEST is the owner's:
	// Game updates Project::levels + saves it after this returns true.
	// Existing save files keep the old stem and won't load — dev-cycle cost.
	bool RenameLevel(const std::string& oldStem, const std::string& newStem);

	// --- deleting a dungeon's levels (W10) -------------------------------------
	// A stair on a level that SURVIVES, leading into one that would not. The
	// reason a dungeon delete refuses: the stair would be left pointing at
	// nothing, and a stair is the one reference no world-map view shows.
	struct StairInto {
		std::string fromLevel;
		int x = 0, z = 0;
		std::string destLevel;
	};
	// Walks every level NOT in `dying` (each as it reads - LevelForReading, so
	// asking stashes nothing: C307) for a traversable
	// stair whose dest names a level in `dying`. EXIT stairs are skipped: their
	// dest is a world LOCATION, and a location that shares a stem's spelling is
	// not a way into that level.
	std::vector<StairInto> StairsInto(const std::vector<std::string>& dying);
	// Forgets a level: every in-memory trace (its map/ent stashes and its
	// dynamic state, so the next savemap cannot write it back) and then its two
	// files. NEVER the active level — its truth is on screen. The MANIFEST and
	// the dungeon's `levels` are the owner's (Game::DeleteDungeon), as for a
	// rename, and so is dropping the undo history, which holds these levels.
	// False (logged) when a file could not be removed.
	bool DeleteLevel(const std::string& stem);

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
	// only meaningful while that level is live. And on a new game, a load or an
	// ambush, all of which can leave the level's NAME standing (C297).
	void BeginUndoStep();
	void CommitUndoStep(bool changed);
	bool CanUndo() const { return !m_undoStack.empty(); }
	bool CanRedo() const { return !m_redoStack.empty(); }
	void Undo();
	void Redo();
	// Drops both stacks. A level transition does this (a step's snapshot is
	// only meaningful while its level is live), and so does a type RENAME:
	// every held snapshot names the type by its old id. So do a new game and a
	// load (ResetForNewGame), on the same level too - the session the steps
	// were taken in is over - and an ambush (InstallLevelFromText), which puts
	// a generated level where the steps' level was (code-review C297).
	void ClearUndoHistory();
	// A counter that moves whenever the editor changes something - the signal
	// live validation re-runs on. Bumped by every kept undo step, undo/redo, a
	// history clear (level transitions, a new game or a load, renames, deletes),
	// and by the edits that
	// take NO undo step but still change what the checker reads: the instance
	// inspectors' apply (door key/name, button target, stair facing and exit)
	// and Game's type-field writes and level creation. Over-bumping only costs a
	// re-run; a missed bump is a stale red box, so when in doubt, NoteEdit.
	u64 EditRevision() const { return m_editRevision; }
	void NoteEdit() { ++m_editRevision; }
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
	// across a relaunch. Returns false if either file could not be written. Not
	// const: a map written is a map that now matches its file (m_mapAsFiled).
	bool SaveLevel();

	// --- dev console hooks ---------------------------------------------------
	// "kind @ x,z" for each live monster.
	std::vector<std::string> MonsterList() const;
	// "kind @ x,z  id n  spawn x,z  anchor x,z  range r" for each live monster
	// (dev console `leash`): where its leash is measured from beside the square
	// it was made on - one placed in the editor or by `spawn` (id -1) used to be
	// anchored on 0,0.
	std::vector<std::string> LeashList() const;
	// --- the one-pipeline check (Game/DamageLedger.h) ------------------------
	// Arming, strictness and the counters live on the ledger itself; the console
	// reaches them through here. Anything that REPLACES party or world state
	// wholesale (a load, a save restore, a respawn, a `heal`) must call
	// RebaseDamageLedger afterwards — the values it overwrote no longer exist to
	// be reconciled, and without a fresh baseline the next checkpoint reports the
	// replacement itself as a violation. So must anything that ERASES from the
	// middle of the monster, prop or door list (every editor removal, a moved
	// door or stair): the ledger knows a value by its address, and each one after
	// the erased slides onto the address before it - judged against the erased
	// one's baseline, a violation naming it, an abort under `pipelineguard
	// strict` (code-review C355).
	ledger::Ledger& DamageLedger() { return m_damageLedger; }
	void RebaseDamageLedger();
	// The `pipeline` command's lines: the RESULT= verdict, then how much health
	// moved by each sanctioned route.
	std::vector<std::string> DamageLedgerReport() const;
	// Presses the button in cell (x,z) (false if none), returning its new state
	// via `out`: the doors and niches it names and its flag op, as a press does.
	// `asParty` also honours its flag= wait, as the party's hand would; without
	// it the press is forced (the `press` dev command's old meaning).
	bool ToggleButtonAt(int x, int z, bool& out, bool asParty = false);
	// The party's hand on the door at (x,z), as a click on its opener would be
	// once it hit (flag wait, key, toggle) - for the harness, which has no
	// pointer to aim. False if there is no door; `open` is its state after.
	bool HandOnDoorAt(int x, int z, bool& open);
	// "id @ x,z = on|off" for each live button (dev console `buttons`).
	std::vector<std::string> ButtonList() const;
	// "id type @ x,z open|shut authored=open|shut [broken] [name=n]" for each live
	// door (dev console `doors`): its state in play beside the record's, which an
	// inspector edit or a wrecked leaf can set apart.
	std::vector<std::string> DoorList() const;
	// Camera vertical FOV in degrees (clamped); UpdateCamera applies it.
	void SetFov(float degrees);
	float Fov() const { return m_fovDegrees; }
	// Toggle the shadow pass (off = lights still lit, just unshadowed).
	// How fast a wandering fire may re-render its shadow cube, in hertz, and how
	// many such re-renders one frame may spend. Live, because the right rate is a
	// judgement made by looking at fire, not a number to be argued about.
	void SetShadowFlicker(float hz, int perFrameBudget = -1) {
		m_shadows.SetFlickerHz(hz, perFrameBudget);
	}
	float ShadowFlickerHz() const { return m_shadows.FlickerHz(); }
	int ShadowFlickerBudget() const { return m_shadows.FlickerBudget(); }

	void SetShadowsEnabled(bool on) { m_shadowsEnabled = on; }
	bool ShadowsEnabled() const { return m_shadowsEnabled; }
	// The shadow cache's readout (dev console `shadows status`): one line of
	// totals, then one per slot - the light holding it this frame (kind:index),
	// its re-renders and why (ShadowScheduler::Reason), the pass of its last
	// render and its light's lag (in squares). AllocTest -Lights reads it to see
	// a door's move and a walking Firelight re-render their cubes.
	std::vector<std::string> DescribeShadows() const;
	// The door on (x,z) as the shadow cache must see it (`shadows door`): its
	// pose and the first shadow pass that drew it (Door::posePass); "" = no door.
	// A slot whose last render came before that pass still shows an older pose.
	std::string DescribeDoorShadow(int x, int z) const;
	// The harness's mutations of the cache (ShadowScheduler::Ignore).
	void SetShadowIgnore(bool notes, bool moves) { m_shadows.Ignore(notes, moves); }

	// --- what the load paths leave resident (code-review C154, C222, C471) -----
	// The texture readout (dev console `textures`): a head line - the tier, the
	// SRV gauge, how many surface and prop sets are loaded and how many are
	// STALE (loaded for a tier that is not the current one: a quality swap must
	// leave none) - then a line per surface set and per prop set with the tier it
	// was asked for and the size its albedo actually loaded at, a prop's normal
	// map reading `flat` when the set has none. Eval.ps1 -SelfTest reads it
	// across Low -> Ultra -> Low.
	std::vector<std::string> DescribeTextures() const;
	// Loads a texture set exactly as a prop asks for one (LoadPropTextures, so it
	// stays cached and swaps with the rest) and returns its readout line, or a
	// line saying it is not installed. The harness's way to load a set no level
	// names: `textures load <set>`.
	std::string ProbeTextureSet(const std::string& set);
	// The model cache's readout (dev console `modelcache`): a head line - files,
	// how many built a multi-material model, how many still PIN CPU image bytes,
	// and those bytes - then a line per pinning file.
	std::vector<std::string> DescribeModelCache() const;
	// The END OF A LOAD (Game::RunLoadTasks, the frame the last task lands):
	// drops the CPU images every cached model still holds. A multi-material
	// model released its own the moment it was uploaded (ModelMulti); what is left
	// here is a file only ever drawn single-mesh, whose embedded images nothing
	// reads at all. A later ModelMulti of such a file re-reads it (and says so).
	// Returns the bytes released.
	u64 ReleaseModelImages();
	// A decoration kind's model as the world holds it (dev console `decokind`,
	// code-review C253): whether it is multi-material, its bounds and their
	// origin radius in UNITS (node transforms baked - what was drawn), the cull
	// radius in metres, and `node_space_radius`, the farthest vertex BEFORE the
	// nodes are baked - what a multi-material cull sphere used to be read from
	// (viking_dagger's ~115 times too big). Loads the kind if it is not loaded;
	// empty when the decorations catalog has no such type (a missing type would
	// fall back to <type>.gltf, and a missing model aborts).
	std::string DescribeDecorationKind(const std::string& type);
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
	// The level's TAGS (same dialog, persisted as the .map `tags`
	// record; same active-vs-stash routing). Nothing in the running world reads
	// them — see the definition.
	void SetLevelTags(const std::string& stem, std::vector<std::string> tags);
	// The level's UI material override (same dialog, the .map `uistone`
	// record; same routing). Empty clears it, so the dungeon's applies. Game
	// re-resolves the chrome after a save (Game::RefreshPlaceStone).
	void SetLevelUiStone(const std::string& stem, std::string name);

	// HUD log feedback (bump lines, monster announcements, palette flavor).
	// Set before play starts; the party/monster callbacks route through it.
	//
	// BORROWS its text (docs/message-allocation.md): callers hand over a
	// loc::Line built on their own stack, so nothing along this path owns,
	// copies or allocates a message until the log copies it into its ring.
	std::function<void(std::string_view)> onMessage;
	// Like onMessage, for lines ABOUT a party member (casting, learning,
	// being struck, going down): carries the member's identity color so the
	// log tints the line in it. MemberMessage routes here, falling back to
	// plain onMessage when unwired.
	std::function<void(std::string_view, const Vec4&)> onMemberMessage;

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
		// WHAT the texture arrays hold: the set names in variant order and the
		// resolution tier they were asked for. A level change whose palette
		// resolves to the same sets at the same tier keeps them instead of reading
		// every map off disk again (AppendLoadTasks). LoadSurfaceMaterial is the
		// only thing that appends and ResetTextures the only thing that clears, so
		// the record cannot disagree with the arrays.
		std::vector<std::string> loadedSets;
		std::string loadedRes;
		bool Holds(std::span<const std::string> sets, std::string_view res) const {
			return !loadedSets.empty() && std::ranges::equal(loadedSets, sets) &&
				   loadedRes == res;
		}
		// Drops the texture variants (keeps the chunks) before a (re)load of the
		// set - the staged loader and the quality hot-swap both reuse the Surface.
		void ResetTextures() {
			albedo.clear();
			normal.clear();
			mr.clear();
			heightScale.clear();
			factors.clear();
			uAspect.clear();
			loadedSets.clear();
			loadedRes.clear();
		}
	};

	// A textured material set shared by props (defined in full below); forward-
	// declared here so monster kinds can point at one before the definition.
	struct PropTextures;

	struct MultiMaterialModel; // defined below (shared with decorations/items)

	// Per-kind monster assets (shared) and per-instance state. Kinds are
	// entity type names from the .ent file ("skeleton" loads skeleton.gltf).
	struct MonsterKind {
		// Shared through the model cache (several kinds use one file - the six
		// skeleton variants); holding it keeps it alive for the Animators that
		// point into its skeleton and clips.
		std::shared_ptr<const assets::ModelData> model;
		std::shared_ptr<gfx::Mesh> mesh; // meshes[0], shared likewise
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
		// Both in d100 POINTS, authored directly: a monster has no skills or
		// stats to run through the curves, so its competence IS the number.
		float accuracy = 60.0f;
		float evasion = 10.0f;
		// THE STANCE (docs/damage-system.md), the monster half of the party's
		// slider: how much of `accuracy` goes into pressing the attack, the
		// rest held back to guard with. 1 = all-out. Authored PER KIND
		// (monsters.cat `offense`) — an archetype describes how a creature
		// moves and perceives, not how boldly it fights, and the two vary
		// independently: a wild caster and a cautious one share an archetype.
		//
		// This couples the two sides from ONE number, which is the point: a
		// cautious monster is genuinely worse at hitting BECAUSE it is harder
		// to hit, rather than being handed good numbers on both.
		float offense = 1.0f;
		float armor = 0.0f; // flat soak (docs/combat.md part 4)
		// The defender side (docs/combat.md part 4): per-type resists
		// (catalog `resists = pierce 0.5, bash -0.5`; a cell of 1.0 =
		// authored immunity) and what this monster's melee deals AS
		// (`dmgtype`, default bash). Ranged/spell attacks type by school.
		ResistTable resists;
		// The ATTACKER half of the type axis (`powers = fire 0.5`), which is a
		// monster's whole answer to the skill a character trains: it has no skills,
		// so this is where "this thing is dangerous WITH FIRE specifically" lives.
		ResistTable powers;
		DamageType damageType{}; // resolved from `dmgtype` at load
		// What a CRITICAL blow leaves behind, on top of on_hit — the same
		// authored form (`on_crit = bleed 2 10`), rolled only when the attack
		// roll went open-ended. Reuses the proc machinery entirely: a crit is
		// not a new kind of thing that happens, it is the same thing happening
		// because the dice said so.
		std::vector<fx::Proc> onCrit;
		// What a LANDED melee blow may leave behind (monsters.cat `on_hit =
		// poison 1 20, bleed 2 10 0.5`): effects named by ID, rolled and
		// landed by fx::ApplyProcs. The older one-per-line `poison =` /
		// `bleed =` fields still load, appended as the same procs.
		std::vector<fx::Proc> onHit;
		// The same list packed into a carrier's inline payload, ONCE at load, for
		// this kind's plain ranged shot (MonsterShoot): a shot fires mid-fight in
		// a guarded frame, and packing it there built the warning label string
		// per shot.
		ProjectilePayload shotPayload;
		// --- what the dice's extremes do (docs/damage-system.md) -------------
		// `crit = pierce`: a critical goes under armour instead of through it.
		bool critPierce = false;
		// What a FUMBLE costs THIS monster — procs landed on itself
		// (`on_fumble = bleed 1 4`) plus the named consequences
		// (`fumble = recover 2.5`, `fumble_severe = self_hit 0.4`). An empty
		// list takes the balance.cat default table; see Game/Mishap.h.
		std::vector<fx::Proc> onFumble;
		std::vector<mishap::Entry> fumble;
		std::vector<mishap::Entry> fumbleSevere;
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
		// monsters.cat `flammable` (lighting-updates Phase 6, Michael: "mummies
		// are a human torch waiting to happen"): ANY fire that lands sets it
		// burning, every time - not the usual chance (MonsterTarget::Wound).
		bool flammable = false;
		// (radially-symmetric models like the blob set faces=false to skip it)
		float fallbackRoughness = 0.9f; // flat-material roughness when no PBR set
		// Render-only orientation/size fixups for imported models that don't ship
		// in the engine's convention (e.g. a Mixamo-rigged asset facing the wrong
		// way). modelyaw is radians added to the facing rotation; modelscale is a
		// uniform visual scale about the model's foot. Both default to no-op.
		float modelYaw = 0.0f;
		float modelScale = 1.0f;
		// The rig's ROOT joint (-1 = no skeleton) and where it stands at rest, in
		// model units. A model is drawn CENTRED on that joint's rest XZ (see
		// MonsterModelWorld), because a bought Mixamo rig need not be centred on
		// its file's origin: the four skeleton-kit models stand ~(0.34, 0.43) off
		// it, so they were drawn half a unit beside their own square - and turned
		// round it with the facing - while everything placed AT the monster (its
		// burn plume and glow, hits, lanes) stayed on the square. The baker rigs
		// and the bought creatures root on the origin already, so for them this
		// is a no-op. The plume also rides the root joint's LIVE pose
		// (BurnOrigin), so it follows the body through a lunge or a fall.
		// What a clip may do to that joint horizontally is held down by the
		// Animator (LockRootTravel, kMonsterRootReach), so the root stays within
		// a lurch of its rest and the two above stay true while it walks.
		int rigRoot = -1;
		Vec3 rigRest{};
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
		// framed on the model's upper portion (UpdateMapIcons). Data-driven -
		// every kind gets one from its own model, no authored 2D art. Starts
		// transparent until the bake runs.
		std::unique_ptr<gfx::Texture> iconTarget;
		// The pose that icon shows: the kind's idle at its first frame - what the
		// asset picker's tile of the model and a resting monster in the world
		// show; the bind pose, a T-pose for the bought kit, is none of them - and
		// the box that pose fills (PosedBounds), which the head shot is framed
		// on (PoseMonsterIcon). A PERSISTENT Animator, one per kind: the
		// renderer knows a palette by its buffer's address for the whole frame,
		// and a throwaway one per bake could hand the next kind of the same size
		// its address and so its pose (code-review C189 / C183).
		anim::Animator iconPose;
		Vec3 iconLo{}, iconHi{};
		// What the last bake of that icon actually DREW, written by the bake
		// itself at the moment it drew (BakeMonsterIcon): the box it framed the
		// head shot on, and the clip and fingerprint (PaletteFingerprint) of the
		// palette it handed the renderer. The readout reports THESE, never the
		// inputs above, so a bake that drifts from its pose or its frame shows
		// (MonsterIconReport measures a fresh pose of the idle beside them).
		struct IconDrawn {
			bool baked = false;
			Vec3 lo{}, hi{};
			std::string_view clip; // a clip name in `model`, which the kind keeps
			size_t joints = 0;     // the palette's size (0 = drawn unskinned)
			double fingerprint = 0.0;
		} iconDrawn;
	};
	// A burning body's plume burns a little bigger than a brazier. Shared by the
	// plume itself (reserved at spawn) and the particle buffer that allows for one.
	static constexpr float kPlumeScale = 1.1f;
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
		// anchor (leashX/Z: MakeMonster sets the spawn cell, a record's leashfrom=
		// moves it) it will chase before breaking off and returning; 0 = unleashed.
		// patrol: a waypoint route walked when idle (P3b; empty = none).
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

		// The flame plume rising off a burning body - PRESENTATION, derived
		// from the list above every frame (DungeonWorld::Update): lit when an
		// effect that burns arrives, put out when it goes. Not state: the
		// effect is the truth, this is just what it looks like. HELD BY VALUE
		// and reserved at spawn (MakeMonster), then lit and put out IN PLACE:
		// it used to be a unique_ptr made on ignition, which allocated in a
		// settled frame every time any monster caught fire again. It was
		// pointer-held because a FireEffect carried a 5 KB mt19937; it no
		// longer does, so every monster can afford one.
		FireEffect plume;
		bool plumeLit = false;

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

		// PER-INSTANCE STRENGTH (the eval harness's `spawn ... <x N>`): scales
		// this creature's hit points and the damage it deals, leaving its
		// catalog entry alone. A ladder climbs by TYPE first — those measure
		// content that ships — and uses this to sweep finely BETWEEN authored
		// types, where a result points at a monster that does not exist and so
		// says where to author one rather than what to fix.
		float strength = 1.0f;
		// PER-INSTANCE PIERCING EDGE (the eval harness's `spawn ... pierce`):
		// this creature's criticals go under armour as though its kind said
		// `crit = pierce`. Read beside the kind's flag in MonsterAttack; not
		// saved, like `strength`.
		bool piercing = false;
		float MaxHp() const {
			return (kind ? kind->maxHp : 1.0f) * strength;
		}
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
	// COPYABLE ON PURPOSE: the GPU parts (textures, submesh geometry) are shared
	// pointers and the materials are values, so the model cache builds a file's
	// GPU resources once and every kind using that file gets its own COPY - the
	// same meshes and textures, but materials it may override (BakeCatalogMaterial)
	// without touching the other kinds' look.
	struct MultiMaterialModel {
		std::vector<std::shared_ptr<gfx::Texture>> textures; // one per model.images
		struct Sub {
			std::shared_ptr<gfx::Mesh> mesh;
			gfx::MaterialParams material;
			// The catalog SET this part wears (an item's `texture`, code-review
			// C301: WearModelSet), else null for the file's own material. Its maps
			// are LOOKED UP each time the part draws (PartMaterial) and never kept
			// in `material`: a quality swap frees and reloads every prop set's
			// maps (ReloadPropTextures), and a pointer kept from before would draw
			// freed textures.
			const PropTextures* set = nullptr;
			// What the part was last DRAWN with: the albedo its draw handed the
			// renderer, and the frame (DungeonWorld::m_drawFrame) it did so -
			// stamped by DrawPart at the draw itself, so `modelfile`'s drawn= reads
			// what a draw site really passed, never a material re-derived for the
			// readout. Mutable: a draw is const. Only compared, never dereferenced
			// (the texture may since have been freed).
			mutable const gfx::Texture* drewAlbedo = nullptr;
			mutable u64 drewFrame = 0;
		};
		std::vector<Sub> subs; // one per model.meshes
		Vec3 boundsMin{}, boundsMax{}; // world-space AABB of the baked geometry
		// The model's LONG axis (0 x, 1 y, 2 z - the AABB's biggest extent) and
		// which way along it the "handle" end lies (+1 / -1): the half holding
		// the widest cross-section, which on a blade is the guard's side. The item
		// details dialog stands a weapon on this axis, handle up.
		int longAxis = 1;
		float handleSign = 1.0f;
		// How an ITEM of this model lies on the floor (and how tall it then
		// stands) is the item kind's, not the model's: ItemKind::floorLay /
		// floorHeight, worked out from these bounds by LayOnFloor.
	};
	// they implicitly get the "memorize" command. Other categories so far reuse
	// the tablet mesh, tinted, as a placeholder (see ItemKindFor).
	struct ItemKind {
		std::string id;          // catalog id (the .ent record type)
		std::string nameKey;     // loc key for the display name ("item.rune_fire")
		std::string category;    // rune|weapon|armor|clothing|food|misc (free-form)
		std::string skill;       // weapon class this item trains/uses (catalog
								 // `skill`, docs/skills.md); "" = `unarmed`
		// Weapon stats (docs/combat.md Phase 1). A `damage` of 0 is NO WEAPON: the
		// hand holding it swings bare (DungeonWorld::HandWeapon), and a speed of 0
		// falls back to the attacker's unarmed pace (the unarmed_* knobs).
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
		// What consuming it restores (items.cat `nutrition` / `hydration`).
		// Both on every item: most food is partly one and partly the other, and
		// 0/0 means it feeds nobody, which is how a consume is refused.
		float nutrition = 0.0f;
		float hydration = 0.0f;
		// A POTION (transparency Phase 4): what drinking it restores at once
		// (items.cat `restore_health` / `restore_stamina` / `restore_mana`), and
		// the effects it treats (`cures = poison 0.5, bleed`): a share of each
		// one's bite taken away, 1 (the default) lifting it outright. Parsed at
		// load, so a drink allocates nothing.
		float restoreHealth = 0.0f, restoreStamina = 0.0f, restoreMana = 0.0f;
		struct Cure {
			std::string effect; // effects.cat id
			float share = 1.0f; // of its magnitude removed; >= 1 removes it
		};
		std::vector<Cure> cures;
		bool drinks = false; // `command` lists drink: the log says "drinks"
		// What a consume leaves in the hand (items.cat `drink_as`): a waterskin
		// drunk from steps down a fill level instead of being used up. Empty =
		// the item is gone (bread is eaten).
		std::string drinkAs;
		// LIGHT (items.cat): `burn_time` > 0 marks a LIT item - it is the party's
		// light while it is held, and burns for that many seconds of game time
		// (the item's CHARGE counts them down, ItemSlot::charge), then becomes
		// `spent_as` (a burnt-out stub). `unlit_as` is what it turns into when
		// it goes out (stowed, put down, doused) and `lit_as` the reverse.
		float burnTime = 0.0f;
		std::string litAs, unlitAs, spentAs;
		// A MAGICAL light (items.cat `power_level`, `flame_color`): the level
		// multiplies the burn, so `burnTime` above is already burn_time x (1 +
		// level); the colour is what its flame and its light are, instead of its
		// light profile's colour. flameTinted=false = an ordinary flame.
		float powerLevel = 0.0f;
		Vec3 flameColor{1.0f, 0.62f, 0.28f};
		bool flameTinted = false;
		// A container one fill level up (items.cat `fill_as`): what a Splash, or
		// any later filling, makes of it. Empty = it takes no water.
		std::string fillAs;
		bool Lit() const { return burnTime > 0.0f; }
		// The light it gives while lit in a hand (a lights.cat id; items.cat
		// `light`, default `torch` for a lit kind, none otherwise).
		std::string light;
		// What it sheds when thrown (a trails.cat id; items.cat `trail`, none
		// by default - a lit torch trails its own flame, see TorchFlame).
		std::string trail;
		// Worn armor's WEIGHT CLASS (armor.cat `class`): what it costs to
		// evade in, which skill it trains, and what STR it asks. The soak
		// itself stays per ITEM (`armor` below) — a breastplate and a mail
		// shirt are both heavy and do not blunt alike.
		ArmorClass armorClass = ArmorClass::None;
		// Which doll slot it is worn in (armor.cat/items.cat `wear`). The UI
		// keeps its own copy in ItemCategoryBank for hit-testing; the world
		// needs it too, to answer "what would this be worth if worn".
		WearSlot wearSlot = WearSlot::None;
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
		// The same, but only on a CRITICAL (`on_crit = bleed 2 10`).
		std::vector<fx::Proc> onCrit;
		// --- what the dice's extremes do (docs/damage-system.md) -------------
		// `crit = pierce`: a critical with this edge goes UNDER armour rather
		// than through it — soak is not subtracted. The only crit consequence.
		bool critPierce = false;
		// What a FUMBLE with this weapon costs its WIELDER: procs landed on the
		// attacker themselves (`on_fumble = bleed 1 4` — a blade that bites the
		// hand holding it), plus the named consequences a status effect cannot
		// express (`fumble = recover 2.5`, `fumble_severe = drop`). An empty
		// list takes the balance.cat default table; see Game/Mishap.h.
		std::vector<fx::Proc> onFumble;
		std::vector<mishap::Entry> fumble;
		std::vector<mishap::Entry> fumbleSevere;
		// The defender side of a WORN piece (part 4): per-type resist cells
		// plus a small flat soak, summed across the equipment slots.
		ResistTable resists;
		// THE ATTACKER SIDE, the mirror of the above (`powers = fire 0.4`): how much
		// harder — or, negative, more feebly — its bearer strikes with each damage
		// type. It sums across the WIELDED weapon and every WORN piece, exactly as
		// resists do, so a fire-forged gauntlet lends its element to whatever hand
		// swings. Characters get their type axis this way rather than innately:
		// their own is skill, per weapon class and per school.
		ResistTable powers;
		float armor = 0.0f;
		float weight = 0.0f;     // carry weight (kg); sums into a member's load
		std::vector<std::string> commands; // hand right-click command ids (data-driven)
		// THROWING (Phase 10): the attacks.cat attack it flies as ("" = the
		// default rule, DungeonWorld_Throw.cpp), what it leaves on what it
		// strikes or where it bursts (on_hit, or a throw_spell's payload), and
		// whether it shatters rather than landing (a flask).
		std::string throwAttack;
		ProjectilePayload throwPayload;
		DamageType throwBlastType{}; // what its blast deals (blast_type / the spell's school)
		bool throwBreaks = false;
		// A RUNE TABLET (items.cat `category = rune` + `symbol`) and its symbol
		// (runeSymbol, below): THE definition of a rune (code-review C347) -
		// Memorize, the icon bank and the starter kit all ask the kind
		// (ItemRune / RuneItemFor), never the shape of the id.
		bool isRune = false;
		// items.cat `upright`: it STANDS on the floor as authored (a bottle) and
		// its icon stands too, instead of being laid along its length.
		bool upright = false;
		// Uniform size trim (items.cat `scale`) over the model's authored unit
		// size — the DecorationKind knob, for floor/niche draws. 1 = as authored.
		float modelScale = 1.0f;
		// HOW IT LIES ON THE FLOOR, worked out once when the kind is built
		// (LayOnFloor; code-review C359): the floor draw's matrix for a spot at
		// the origin - its model, or the tablet, scaled to metres, laid on its
		// flattest side or standing for `upright`, grounded and centred - and the
		// height it then stands to, in metres. FloorItemPose moves the one to the
		// item's spot; half the other is its drawn middle, where a click ray is
		// measured.
		Mat4 floorLay = Mat4Identity();
		float floorHeight = 0.0f;
		SpellSymbol runeSymbol = SpellSymbol::Fire;
		Vec4 glow{1, 1, 1, 1};   // accent-glow tint (element colour / category tint)
		// Carved-stone tablet look: the shared tablet mesh (m_runeMesh) drawn with
		// this element's PBR set (rune_<elem>). null tex falls back to flat stone.
		const PropTextures* tex = nullptr;
		// Authored multi-material model (catalog `model`): when set, the item draws
		// as this on the floor and its baked 3D thumbnail is the icon/cursor. null =
		// the tablet+tint placeholder above.
		std::unique_ptr<MultiMaterialModel> model;
		// The set `model` wears (items.cat `texture`, code-review C301), "" = only
		// the file's own materials. Which parts wear it is WearModelSet's rule;
		// each such part carries the set itself (MultiMaterialModel::Sub::set).
		std::string modelSet;
		// Baked 3D icon render-target (one per type, owned here; reused by every
		// slot/grid/cursor instance via the icon bank). Null for placeholder items;
		// transparent until UpdateItemIcons renders into it.
		std::unique_ptr<gfx::Texture> iconTarget;
		// Catalog `icon_spin`: re-bake this icon every frame on a turntable spin
		// (vs the static shape-aware pose). The cursor + every slot animate with it.
		bool iconAnimated = false;
	};
	// Rewrites a held item as the kind its `becomes` field names (&litAs: light
	// it; &fillAs: fill it a level), keeping its charge; returns the new kind's
	// name key, or empty when that field is empty (the item does not take it).
	// The cast services' lightItem / fillItem (DungeonWorld_Ahead.cpp).
	std::string_view RenameHeldItem(ItemSlot& slot, std::string ItemKind::*becomes);
	struct Item {
		const ItemKind* kind = nullptr; // points into m_itemKinds (stable)
		int id = -1;                    // source Entity::id (>= 0 = .ent baseline)
		int x = 0, z = 0;
		bool collected = false; // picked up — hidden + saved so it stays gone
		// Sub-cell quarter (Medium 2x2 slot, 0..3) the tablet rests in — a dropped
		// item snaps to the quarter nearest the cursor; up to 4 share a cell. The
		// draw, the pick and its lights all take it through FloorItemPose
		// (SlotCenter(x,z,Medium,slot)). See SlotGrid.h.
		int slot = 0;
		// The wall NICHE this item sits in (Direction index; -1 = an ordinary floor
		// item). Niche items pile at the pocket centre (NicheItemPos), ignore `slot`,
		// and are hidden + unpickable while the niche is closed.
		int niche = -1;
		// The item's own charge (ItemSlot::charge - a torch's seconds left),
		// kept while it lies here and handed back when it is lifted. LAST, so
		// the positional inits above need not name it.
		float charge = -1.0f;
		// CONJURED by a spell (Rock's pebble at the feet) and never handled
		// since: one of the level's recycled pool (ConjureDrop), saved as the
		// `drop` line's 8th token. Lifting it makes it the party's; what is put
		// down again is an ordinary drop. After charge, for the same reason.
		bool conjured = false;
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
		// Flags (flags.cat ids, "" = none): `needs` is the flag= param - the
		// lever will not move until it is on; `sets` is what a press does to a
		// flag, `op` saying how (sets= / clears= / toggles=, one per lever).
		std::string needs, sets;
		FlagOp op = FlagOp::None;
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

	// How a door's leaf gets out of the way. Blocking is keyed to the CELL, not
	// to the leaf, so this is purely a render matrix — every motion costs the
	// same and none of them touches walkability.
	enum class DoorMotion {
		Slide, // sideways into the flanking wall (a pocket door)
		Rise,  // straight up into the ceiling block, which hides it (Dungeon
		       // Master's portcullis idiom — wants no floor space at all)
		Split, // two halves parting, each into its own jamb
	};

	// How a door is WORKED BY HAND. The opener is the affordance on the jamb —
	// Dungeon Master's square pad, Grimrock's hanging chain — and its absence is
	// meaningful: a door with no opener cannot be opened by the party at all,
	// only by a button wired to its name. That gate is the whole point of the
	// setting, since "opened by other means" is otherwise indistinguishable from
	// "opened by anyone who walks up to it".
	//
	// The STYLE picks the behaviour; the model, texture and placement come from
	// the opener's own catalog entry, so a new kind that moves like one of these
	// is data alone (the trim/frame pattern) and only a genuinely new MOVEMENT
	// costs an enumerator here.
	enum class OpenerStyle {
		Pad,   // a plate pressed into the jamb
		Chain, // a hanging chain, pulled down
	};

	// Where an opener hangs by DEFAULT, in the door's own UNIT model space: on
	// the frame's face, centred on the jamb. The jamb face runs from the opening
	// edge (0.34) to the cell edge (0.5), so its middle is 0.42.
	//
	// It is a default and not a rule, because "centred on the jamb" and "clear
	// of the jamb" are different answers and different openers want different
	// ones. A flat pad wants the middle. A CHAIN does not: the jamb stones stand
	// prouder (0.095) than a chain hangs (~0.091), so anything within their
	// reach vanishes behind them the moment you look at the door from an angle —
	// which is how a door is normally approached. Openers override with
	// `offset`; see doors.cat.
	//
	// The mortice is INTERNAL (mid-thickness, |z| < 0.032), so an opener on the
	// face never fouls the slot its leaf runs into, whichever jamb it picks.
	static constexpr float kOpenerX = 0.42f;
	static constexpr float kOpenerFaceZ = 0.076f;
	// How far a worked opener moves, and the two speeds it moves at. The pull is
	// FAST and the recovery is slow, but neither is instant: a step to full
	// travel in one frame reads as a glitch rather than a yank.
	//
	// The chain's drop is small on purpose. It EXTENDS out of its socket rather
	// than sliding as a whole (the socket is a separate, static model that hides
	// the links still inside it), so the travel has to stay inside the length of
	// chain the socket conceals — go further and the chain's top end walks out
	// of the bottom of its own anchor.
	static constexpr float kChainDrop = 0.045f;  // units, down the wall
	// The pad's face stands 2.4 cm above its surround, so this is bounded by the
	// mesh: 1.75 cm sinks it nearly flush and still leaves a lip. It was 3.5 cm
	// — deeper than the whole fitting — and the button disappeared into the
	// stone rather than being pressed.
	static constexpr float kPadPress = 0.007f;   // units, into the jamb
	static constexpr float kPullDownSeconds = 0.13f;
	static constexpr float kPullSeconds = 0.55f;
	// HAND HEIGHT, derived from the eye and not from the wall. The eye sits at
	// 0.62 of the wall, so a fraction of the WALL puts a "waist-high" prop well
	// below the viewer's waist and it reads as fallen — the trap the lever hit.
	static constexpr float kOpenerY = kEyeHeight / kUnit - 0.06f;

	// A door filling a doorway cell (side walls flank the travel axis). Closed
	// it blocks the party, monsters, and projectiles; the leaf gets out of the
	// way per `motion` as openT animates toward `open`. `facing` is the travel
	// direction through it; `name` is what buttons target (name= param).
	//
	// A SPLIT door's leaf model is authored as the LEFT HALF only, and the
	// render draws it twice — the second copy turned a half turn about Y, which
	// is a proper rotation (winding survives, so an authored mesh still
	// back-culls) and lands the half on the right of the opening.
	// Open-state diffs ride the save like button toggles (kind Door, activated).
	//
	// The motion knobs are resolved from the type's catalog entry at spawn, not
	// read per frame — a door type is fixed for the life of a placement, and the
	// render loop runs over every door every frame.
	// THE DAMAGEABLE HALF OF A PIECE OF DUNGEON (docs/damage-system.md). A door,
	// a barrel, a brazier — whatever can be broken carries one of these, and one
	// fx::ITarget adapter (BreakableTarget) serves them all, so the dungeon
	// reaches the damage pipeline through exactly the same door a combatant does.
	//
	// DAMAGEABILITY IS OPT-IN AND OFF BY DEFAULT (`breakable` in the catalog,
	// Michael's requirement): if props and doors were breakable unless told
	// otherwise, keys and switches would stop mattering the moment a party could
	// swing at a door. maxHp of 0 means "not a target at all" and every ask below
	// short-circuits on it.
	//
	// It carries an `effects` list like a combatant, which is not decoration: it
	// means a DoT works on the dungeon for free, so a burning door burns DOWN.
	// This is dynamic state and lives here rather than on the static .map records
	// it describes — the same split m_seen makes.
	struct Breakable {
		float hp = 0.0f;
		float maxHp = 0.0f; // 0 = indestructible; nothing else is consulted
		float soak = 0.0f;  // catalog `armor`
		ResistTable resists;
		std::vector<fx::Inst> effects;
		bool broken = false;

		bool Damageable() const { return maxHp > 0.0f; }
		bool Alive() const { return Damageable() && !broken; }
		// Back to how its type made it: whole, at full hp, nothing riding it - a
		// new game or a load over the same level (code-review C292). clear(), not
		// `= {}`, so the list keeps the room fx::ReserveEffects gave it.
		void Mend() {
			hp = maxHp;
			broken = false;
			effects.clear();
		}
	};

	// A FIXTURE's damage state, kept beside the map rather than on it. Sconces and
	// braziers are STATIC .map records (WallSconce / FloorBrazier in DungeonMap), so
	// unlike a decoration they have no instance struct of their own to carry a
	// Breakable — and hanging dynamic state on a static record is exactly the split
	// this project keeps (the same reason m_seen is not baked into DungeonMap).
	//
	// Keyed by cell + WALL, because several sconces may share a cell on different
	// walls; `wall` is -1 for a floor-standing brazier, which has none.
	struct FixtureBreak {
		int x = 0, z = 0;
		int wall = -1; // Direction as int; -1 = floor-standing
		std::string type;
		Breakable brk;
	};

	struct Door {
		int id = -1;                         // source Entity::id (.ent record)
		std::string type;                    // doors.cat id, for naming + breakage
		int x = 0, z = 0;
		Direction facing = Direction::South; // travel axis (panel spans the other)
		std::string name;                    // button-target id ("" = unwired)
		std::string key;                     // item id that unlocks it ("" = none)
		// A flags.cat id the door waits on (flag= param, "" = none): the party's
		// hand cannot open it until the flag is on - the key rule's shape, and
		// like a lock a wired button still moves it.
		std::string flag;
		bool open = false;
		bool initialOpen = false;            // authored state (open= param)
		float openT = 0.0f;                  // open anim, 0 closed .. 1 open
		DoorMotion motion = DoorMotion::Slide;
		float travel = 0.75f;                // how far the leaf moves, UNITS
		float openSeconds = 0.7f;            // how long the full throw takes
		// The SHAPE of the leaf's travel, both ends chosen separately. This
		// replaced a hardcoded smoothstep: every door moved the same way, which
		// is the one thing a stone slab and a wooden door should not share.
		EaseSpan ease;
		const DecorationKind* panel = nullptr;
		const DecorationKind* frame = nullptr;
		// An optional second moving part drawn with the panel's matrix. The
		// import path binds ONE texture set per model, so a wooden door's iron
		// straps have to be their own mesh to be iron — the same split the lever
		// makes between its stone plate and its wooden handle.
		const DecorationKind* trim = nullptr;
		// The hand-hold. Null = none, and none means the party CANNOT work this
		// door — see OpenerStyle. Resolved from the type, overridable per
		// placement by the record's opener= param.
		const DecorationKind* opener = nullptr;
		// The opener's STATIC part, drawn at the same place but never moved: the
		// socket a chain hangs out of. It is what makes the chain read as
		// EXTENDING rather than sliding — the links still up inside the socket
		// are hidden by it, so working the chain feeds more of it into the room
		// instead of translating the whole thing down the wall.
		const DecorationKind* openerMount = nullptr;
		OpenerStyle openerStyle = OpenerStyle::Pad;
		float openerX = -kOpenerX; // which jamb, signed: negative = left
		float pullT = 0.0f;        // 0 at rest .. 1 fully worked
		bool pullRising = false;   // true while it is being pulled, false coming back
		EaseSpan openerEase;       // the hand-hold's own shaping, from its entry
		// The first shadow pass to draw the door's latest pose: stamped where the
		// leaf or hand-hold moves in play (the travel, a smash) and never by its
		// caster note, so `shadows door` can tell a cube that kept the pose the
		// door LANDED in from one re-rendered only as it began to move.
		u64 posePass = 0;
		// Can it be broken down? OFF unless doors.cat says `breakable = 1`
		// (Michael's requirement — otherwise a party would simply chop through
		// every locked door and keys and switches would stop mattering). A broken
		// door's way is open FOR GOOD: it cannot be shut again, which is the
		// difference between smashing one and opening it.
		Breakable brk;
	};

	// Static architecture decorations from the .map layer (column, archway,
	// fountain, statue, barrel, ...). One shared model+mesh per type; instances
	// are placed and oriented once at load and never move or animate, so they
	// carry no per-frame state and stay out of the save (static = .map only).
	// A textured material set shared by props (loaded once per set name). Mirrors
	// Surface but single-variant: albedo (sRGB) + normal/height + ORM, linear.
	// Kinds hold a POINTER to one, so the entry itself never moves or dies: a
	// quality swap replaces its maps in place (ReloadPropTextures).
	struct PropTextures {
		std::unique_ptr<gfx::Texture> albedo, normal, mr;
		float heightScale = 0.0f;
		// The tier the maps were asked for (GameSettings::TextureSuffix) - what a
		// swap compares, as Surface::loadedRes is. A prop set installed only at
		// 2k reads "4k" at Ultra and holds the 2k fallback; that is the tier
		// asked for, not the size loaded (the `textures` readout shows both).
		std::string res;
		bool flatNormal = false; // no `_n` map: the flat placeholder (C471)
	};
	struct DecorationKind {
		std::shared_ptr<const assets::ModelData> model; // via the model cache
		std::shared_ptr<gfx::Mesh> mesh;                 // meshes[0], likewise
		Vec4 color{1, 1, 1, 1};
		const PropTextures* tex = nullptr; // points into m_propTextures (stable)
		// Authored multi-material models render their own glTF textures instead of
		// a single bound set; non-null replaces the mesh/tex/color path above.
		std::unique_ptr<MultiMaterialModel> multi;
		std::string id;            // catalog id (the record type), for the writer
		// Which catalog the id is in (its cache, m_decorationKinds[catalog]), the
		// model file it loaded and the texture set it wears ("" for a multi-
		// material model, which wears its own) - what `propkinds` reports.
		PropCatalog catalog = PropCatalog::Decorations;
		std::string file, set;
		bool authored = false;     // imported model: consistently wound -> back-cull
		bool solidDefault = true;  // floor-standing blocks the party (passages don't)
		float alphaCutoff = 0.0f;  // > 0: alpha-test cutout (masked set, e.g. a gate)
		bool transparent = false;  // decorations.cat `transparent`: drawn as glass
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
		// Breakability, OFF unless decorations.cat says `breakable = 1` — the
		// gate, with `hp`/`armor`/`resists` for how tough it is. Copied into each
		// instance's Breakable at placement.
		bool breakable = false;
		float hp = 0.0f;
		float soak = 0.0f;
		ResistTable resists;
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
		// ORIGIN - for a multi-material model the farthest corner of its baked
		// bounds (OriginRadius) - carried through kUnit and modelScale
		// (DecorationKindFor).
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
		// Breakable if its type opted in (decorations.cat `breakable = 1`).
		Breakable brk;

		// A smashed prop is GONE for every purpose — not drawn, not blocking, not on
		// the map — but its record STAYS in the list. Erasing it would dangle the
		// reference its damage adapter holds, and the save has to be able to name
		// what was broken after the fact, which an erased record cannot do.
		bool Gone() const { return brk.broken; }
		// Does it stop the party? Smashing a crate in a doorway clears the way,
		// which is most of the point of being able to smash it.
		bool Blocks() const { return solid && !brk.broken; }
	};

	// Fires: wall sconces (at 'T' cells, mounted on the adjacent wall) and
	// floor braziers ('F' cells). Each carries a flickering point light at
	// its flame origin and a FireEffect particle simulation.
	struct FixtureKind; // defined with the fixture members below

	struct Fire {
		const FixtureKind* kind = nullptr; // resolved catalog id (mesh/tex/flame)
		bool brazier = false;    // floor-standing (light params branch on this)
		bool lit = true;         // false: prop still drawn, but no light/flame/smoke
		// Which map record this fire is - the FixtureBreak key (cell + wall, -1
		// for a brazier) - so breaking a fixture can put ITS fire out, and
		// SetFireBurning (a spell, a save) can find it.
		int x = 0, z = 0, wall = -1;
		float lightRadius = 7.0f; // point-light reach in metres (sconce brightness * cell)
		// Its placement's own flame colour (light + flames); kNoFlameColor = the
		// kind's light profile decides.
		Vec3 flameColor = kNoFlameColor;
		Mat4 world;        // prop transform
		Vec3 flamePos;     // particle + light origin
		float phase = 0;   // flicker phase
		FireEffect effect;
		// A wall torch whose torch was taken: the bare bracket draws, nothing burns.
		bool empty = false;
		// A FLARE in progress, 1 = just fanned .. 0 = none, decaying in Update:
		// the light swells and the flames leap while it lasts (FlareFire).
		float flare = 0.0f;
		// What is happening TO this fire, through the effects system like any
		// combatant's list: the smoke it leaves when it goes out (its kind's
		// on_douse) lands here, and the haze over its square is read off it
		// (fx::EffectKind::Haze). Reserved at build, so a douse allocates nothing.
		std::vector<fx::Inst> effects;
	};
	// Lights (and the flame + smoke of) or puts out the sconce on (x,z)/`wall`,
	// or the brazier on (x,z) when `wall` < 0: the map record's burning state,
	// the live fire, and the haze it feeds, together. A kind that can never
	// burn (fixtures.cat `flame = 0`) is never lit. Going out lands the kind's
	// on_douse effects on the fire (its smoke) - unless `smoke` is false, for a
	// state RESTORED (a save, a broken fixture coming back dark), which went out
	// long ago. True if anything changed.
	bool SetFireBurning(int x, int z, int wall, bool burning, bool smoke = true);
	// Fans a burning fire (a gust): it flares for a moment. False if there is no
	// such fire or it is out.
	bool FlareFire(int x, int z, int wall);
	Fire* FindFire(int x, int z, int wall);
	// Every live fire to its map record's burning state (after the records were
	// set wholesale - a new game, a reset).
	void SyncFiresFromMap();
	// Flares dying away, dust puffs settling (DungeonWorld_Fires.cpp).
	void UpdateFireTransients(float dt);

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
		// The set has no `_n` map, so `normal` is the FLAT placeholder
		// (LoadNormalMapFile): no relief, no parallax. One warning said so.
		bool flatNormal = false;
	};
	// Loads a PBR set by base name at the current quality tier, falling back to
	// the always-present 2k set. `required` (surfaces) never comes back without
	// an albedo: a set missing even at 2k gets the magenta checker placeholder
	// (LoadTextureFile) and a warning - a provisioning gap, not a crash.
	// Otherwise (props) a missing set returns a null albedo so the caller keeps
	// its flat material. A missing `_n` map is a flat normal and one warning
	// (code-review C471), never the magenta checker read as a normal. The
	// single source of the res->2k fallback.
	PbrMaps LoadPbrSet(const std::string& name, bool required);

	void LoadDungeonBlocks();      // loads the worn block set for the quality tier
	void LoadFeatureMeshes();      // wall/surface feature meshes (file-cached)
	void LoadSurfaceMaterial(Surface& surface, const std::string& name,
							 float heightScale);
	void LoadTextureSet(const SurfaceDef& def); // resets, then loads the set
	void LoadAllSurfaceTextures(); // reloads every set (quality hot-swap)
	// The full surface bake: builds every chunk's geometry, then DRAINS THE GPU
	// and replaces the old chunk meshes. The drain is its own (code-review C193):
	// a caller need not remember one, and `arena` and the eval `reset` did not.
	void BuildDungeonMeshes();
	void LoadMonsters();
	void LoadItems(); // instantiates EntityKind::Item records (runes) from .ent
	void LoadButtons(); // instantiates EntityKind::Button records from .ent
	void LoadDoors();   // instantiates EntityKind::Door records from .ent
	// Builds one live Door from its record (kinds lazily loaded via the doors
	// catalog: the type's entry = the panel, [door_frame] = the shared frame).
	void SpawnDoor(const Entity& record);
	// The type's opener with the placement's override applied. Shared by
	// SpawnDoor and the inspector's edit so both land on the same resolution.
	void ResolveDoorOpener(Door& door, const std::string& type,
						   const std::string& openerParam,
						   const std::string& sideParam);
	// Where an opener hangs in WORLD space, for `face` = +1 / -1 (the two sides
	// of the door): its grip, or `local`, a point in the opener model's own
	// space (the far face's copy turned a half turn, as the draw turns it).
	// Declared down here rather than beside ToggleDoorAhead because a member's
	// SIGNATURE can only name nested types already declared, and Door is defined
	// further down the class.
	Vec3 OpenerPos(const Door& door, float face, const Vec3& local = {}) const;
	// Whether `ray` takes hold of the door's opener on either face: a ball round
	// the middle of what is drawn (Camera::Ray::HitsSphere). ToggleDoorAhead's
	// test, which the pick probe (`pickprobe`) asks too.
	bool OpenerUnderRay(const Door& door, const gfx::Camera::Ray& ray) const;
	Door* DoorAt(int x, int z);
	const Door* DoorAt(int x, int z) const;
	// (DoorwayFacing moved to DungeonMap — it only ever read the map, and the
	// placement resolver needs it without dragging the whole world in.)
	// Whether anyone stands in the doorway at (x,z): the party, or a LIVING
	// monster whose body covers the square (a Huge's 2x2 included). The one test
	// for every way a door can come to stand shut there - ToggleDoor's close, the
	// inspector's close, AddDoor and the editor's door move (code-review C357; the
	// close checked monsters alone, and a lever in a doorway shut its own door on
	// the party). A corpse is nobody (C65).
	bool DoorwayOccupied(int x, int z) const;
	// Toggles one door (with the doorway-occupied jam check + message/anim) /
	// every door whose name matches a button's target.
	bool ToggleDoor(Door& door);
	// The party's HAND on a door, once it has been reached: the flag wait, then
	// the key, then the toggle. False when it was refused (or jammed).
	bool HandOnDoor(Door& door);
	void ToggleDoorsNamed(const std::string& name);
	// A lever's whole press: flip it, toggle the doors and niches it names, and
	// apply its flag op. Its flag= wait is the CALLER's to honour (the party's
	// hand does; a forced dev press does not).
	void PressButton(Button& b);
	// What `flag=` / `sets=` / `clears=` / `toggles=` on a button record say, read
	// into the live lever (spawn and the inspector's apply share it).
	void ReadButtonFlags(const Entity& record, Button& b);
	// Sets / clears / toggles a flag in the borrowed store (no-op without one).
	void ApplyFlagOp(FlagOp op, std::string_view id);
	// Lazily loads (and caches) the shared behaviour for an item type, resolved
	// through the items catalog (category=rune → symbol + element glow colour).
	ItemKind& ItemKindFor(const std::string& type);
	// Fills `kind` from the item catalogs' entry for `type` - ItemKindFor's build,
	// and a rebuild's. An icon target `kind` already holds is KEPT when the item
	// still bakes one (the icon bank and every slot draw that texture), dropped
	// when it no longer does.
	void BuildItemKind(const std::string& type, ItemKind& kind);
	// Re-reads an item type's kind IN PLACE after a type-editor save (code-review
	// C302): the kind is never erased, because thrown items, the Earth light's
	// stone and the icon bank hold pointers to it. The old model and textures
	// GO, though: the caller drains the GPU first (ReloadTypeKind does) and
	// closes whatever borrowed them - the item details dialog's preview (Game's
	// type save closes it, as a quality swap does).
	void RebuildItemKind(const std::string& type);
	// THE WORN PIECES (code-review C12): every doll slot that DEFENDS - the hands
	// excepted - handed to `fn` as its item kind. The ONE hand rule for every
	// defensive sum (WornArmorClass, PartyTarget::Soak and ::Resist, DefenseFor),
	// which used to disagree: the class skipped the hands while soak and resists
	// counted them. Defined in DungeonWorld_Combat.cpp, its only user.
	template <class Fn> void ForEachWornPiece(const Character& member, Fn&& fn);
	// items.cat `liquid_color`: generates the liquid inside the kind's glass and
	// appends it to its model as one more part (DungeonWorld_Load.cpp).
	void AddLiquid(ItemKind& kind, const CatalogEntry& def, const std::string& modelFile);
	// AddLiquid's core, shared with the asset picker (LoadPoolModelLook) so a
	// potion shows filled there exactly as in play: the liquid part generated
	// from `file`'s see-through part with `def`'s liquid_color / liquid_fill.
	// False when the colour is absent or the model has no glass.
	struct LiquidPart {
		std::shared_ptr<gfx::Mesh> mesh;
		gfx::MaterialParams material;
		bool fromInnerWall = true;
	};
	static bool BuildLiquid(gfx::GraphicsDevice& device, const assets::ModelData& file,
							const CatalogEntry& def, LiquidPart& out);
	// A FILLED bottle's glass goes clear (see AddLiquid).
	static void ClearGlassForLiquid(gfx::MaterialParams& glass);
	// Lays a RUNTIME drop (negative id) on the floor: into the slot of a
	// runtime drop that was picked back up (it is dead - the save skips it)
	// when there is one, else onto the end. With ReserveDropRoom's headroom, a
	// drop allocates nothing, and a pick-and-drop loop never grows the list.
	void PlaceDrop(const Item& item);
	// Lays a CONJURED item (Item::conjured) in square (cx, cz): the level keeps
	// at most kConjuredDrops of them lying uncollected, and past that the OLDEST
	// (the highest runtime id) is taken up and laid here instead (Michael,
	// code-review C227) - so practising Rock with full hands neither litters a
	// level without end nor outgrows the drop headroom inside a guarded frame.
	// Only a conjured item is ever recycled; a real one on the floor never is.
	void ConjureDrop(const std::string& typeId, int cx, int cz);
	static constexpr int kConjuredDrops = 16;
	// Tops up m_items' spare capacity for drops, at load time: kDropRoom for
	// real drops, and the conjured pool's kConjuredDrops on top of it.
	void ReserveDropRoom();
	// A kind's preview submeshes into `out` (its authored model, else the carved
	// tablet) plus the model-space AABB to frame them by; returns the count.
	// `pose`, when given, receives how the details dialog stands it up before its
	// turntable spin (a weapon on its long axis handle up, a flat thing face-on,
	// the rest as authored - PreviewPose in DungeonWorld_Load.cpp).
	size_t FillItemPreview(const ItemKind& kind, std::span<gfx::PreviewSubmesh> out,
						   Vec3& fitMin, Vec3& fitMax, Mat4* pose = nullptr) const;
	// Renders a soft round halo (sprites) + one model's submeshes into an icon
	// render-target (fit to bounds, flat face to camera, 3/4 view). Shared depth
	// target; the bake list redirects the OM.
	void BakeIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
				  const MultiMaterialModel& model, const gfx::Texture& target,
				  bool animated, float spin, bool torch = false, bool upright = false);
	// The carved tablet's material for a HELD view (icon, details dialog):
	// the rune's set, darker stone, the groove glowing in its school's colour.
	void RuneTabletMaterial(gfx::MaterialParams& mat, const ItemKind& kind) const;
	// A rune's icon: its carved tablet in its own texture set (RuneTabletIconWorld).
	void BakeRuneIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
					  const ItemKind& kind, const gfx::Texture& target);
	// Both bakes' bracket: Begin redirects the OM at `target`, clears it, lays
	// the halo and opens the scene with the icon camera + studio rig; the caller
	// draws; End hands `target` back as a shader resource.
	void BeginItemIconBake(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
						   const gfx::Texture& target);
	void EndItemIconBake(ID3D12GraphicsCommandList* list, const gfx::Texture& target);
	// Draws every submesh of an authored multi-material model at `world`, each with
	// its own glTF material - or the catalog set it wears, as PartMaterial looks it
	// up. Shared by decorations, floor items, and the icon bake.
	void DrawMultiMaterial(ID3D12GraphicsCommandList* list,
						   const MultiMaterialModel& model, const Mat4& world);
	// THE ONE DRAW OF A MODEL'S PART: hands the renderer `mat` and stamps the
	// part with what it was handed (Sub::drewAlbedo / drewFrame), which is what
	// `modelfile` reports as drawn= (code-review C301). DrawMultiMaterial and the
	// light stones draw their parts through it; a part drawn any other way would
	// read "undrawn".
	void DrawPart(ID3D12GraphicsCommandList* list, const MultiMaterialModel::Sub& sub,
				  const Mat4& world, const gfx::MaterialParams& mat);
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
	// Formation pass (Phase 5): assign each aware ENGAGING monster a target attack
	// cell (an orthogonal neighbour of the party a monster can stand on -
	// MonsterCanStand), spreading them around the party (surround) before
	// doubling up a side; overflow, kiters and fleers hold their own cell, and
	// unaware or idle monsters target the party cell. Sets Monster.targetX/
	// targetZ; called before BuildAISnapshot.
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
	// A stair prop's transform: on its cell, turned OPPOSITE its facing (the
	// meshes' +Z is the way you travel on them; the facing is the way you step
	// off). Shared by placement and the stair inspector's turn.
	Mat4 StairPropWorld(const DecorationKind& kind, int x, int z, Direction facing) const;
	// Lazily loads (and caches) the shared assets for a monster / prop type
	// (model + mesh + PBR set), a prop resolved through its `catalog`'s entry and
	// cached under that catalog (code-review C302). Shared by the initial load and
	// live editor placement.
	MonsterKind& MonsterKindFor(const std::string& type);
	DecorationKind& DecorationKindFor(const std::string& type, PropCatalog catalog);
	// The project catalog a PropCatalog names.
	const Catalog& PropCatalogOf(PropCatalog catalog) const;
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
	// THE QUALITY SWAP'S HALF FOR PROPS (code-review C154): every cached prop set
	// whose tier is not the settings' current one is reloaded IN PLACE - the
	// entry, and so every kind's pointer to it, stays; its three maps are freed
	// and loaded again at the new tier. Before this only the surfaces swapped, so
	// Low -> Ultra left 1k doors beside 4k walls and Ultra -> Low kept the props'
	// 4k VRAM. The caller DRAINS first (in-flight frames sample the old maps).
	// Returns how many sets it reloaded.
	int ReloadPropTextures();
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
public:
	// Routing info for DungeonMap's fixture-record parser, derived from the
	// project's fixtures catalog (wall-mount ids + the glyph default ids) and,
	// for `theme` records, its themes. Passed at every DungeonMap
	// construction - including Game's, when a new world parses a level of a
	// project that is not the running one.
	static FixtureTypes FixtureTypesOf(const Project& project);
private:
	// Builds an authored model's own GPU resources (one texture per embedded glTF
	// image, one submesh per primitive with its material) for the multi-material
	// decoration path. `model` must still HOLD its images: a model whose images
	// were released (assets::ReleaseImages) is refused by an assert, since it
	// would build every texture from an empty image.
	static std::unique_ptr<MultiMaterialModel> BuildMultiMaterialModel(
		gfx::GraphicsDevice& device, const assets::ModelData& model);
	// Bakes the entry's metallic=/roughness=/color= overrides into an authored
	// model's per-submesh materials (the multi-material twin of the DecorationKind
	// ApplyPropMaterial overload).
	static void BakeCatalogMaterial(MultiMaterialModel& model,
									const CatalogEntry* def);
	// WHICH PARTS OF A MODEL A CATALOG SET DRESSES (code-review C301): every part
	// of a single-primitive .gltf - the set REPLACES the file's material, as the
	// world's single-mesh prop draw binds it - else only the parts the file leaves
	// untextured. The one rule an item's `texture` (WearModelSet) and the asset
	// picker's look (LoadPoolModelLook) both follow; `file` is the model's file
	// or path.
	static bool SetDressesWholeModel(size_t parts, std::string_view file);
	// Dresses `model` in prop set `set` by that rule: each part it reaches is
	// marked (Sub::set) and given the factors a prop draw gives the set. Null =
	// nothing to wear. The maps themselves are looked up as each part draws.
	static void WearModelSet(MultiMaterialModel& model, const PropTextures* set,
							 std::string_view file);
	// A part's material as it draws NOW: its own, with the maps of the set it
	// wears (Sub::set) looked up afresh - never kept, since a quality swap frees
	// and reloads them. Every draw of an item's parts goes through this
	// (DrawMultiMaterial, the item preview, a light stone).
	static gfx::MaterialParams PartMaterial(const MultiMaterialModel::Sub& sub);
	// A decoration's CULL SPHERE about its model's own origin, in model UNITS
	// (DungeonWorld_Models.cpp). Props are authored grounded (min y = 0) and
	// XZ-centred, so the origin sits at the base centre and a sphere about it
	// covers the whole mesh however a placement turns it - correct by
	// construction, never a false cull. OriginRadius: the farthest corner of a
	// box, the multi-material path's (MultiMaterialModel::boundsMin/Max, its
	// node transforms baked - code-review C253). ModelOriginRadius: the farthest
	// RAW vertex, right for a single-mesh model drawn as it is in the file
	// (meshes[0], an identity node) and wrong for anything carrying a node.
	static float OriginRadius(const Vec3& lo, const Vec3& hi);
	static float ModelOriginRadius(const assets::ModelData& model);
	// THE MODEL CACHE (DungeonWorld_Models.cpp). The kind caches are keyed by
	// CATALOG ID, and many ids share one file - six monster kinds on
	// skeleton.gltf, five armours on leather_armor.glb, each enchanted blade on
	// its plain twin's mesh - so every one of those used to parse the file and
	// upload its own GPU copy. These key by FILE NAME ("skeleton.gltf") instead:
	// the parse, meshes[0]'s GPU mesh and the multi-material GPU build each happen
	// once per file, and a kind holds shared pointers into them.
	std::shared_ptr<const assets::ModelData> ModelFile(const std::string& file);
	std::shared_ptr<gfx::Mesh> ModelMesh(const std::string& file); // meshes[0]
	// A per-kind COPY sharing the file's GPU meshes and textures, so the caller
	// may bake its own material overrides into it. The first call uploads the
	// file's embedded images and then RELEASES them from the cached ModelData
	// (code-review C222): every later copy shares the GPU textures instead.
	std::unique_ptr<MultiMaterialModel> ModelMulti(const std::string& file);
	// Drops a file so its next use reads it off disk again (a type the editor
	// just saved). Kinds still holding the old copy keep it until they reload.
	void ForgetModelFile(const std::string& file);
	void BuildFires();
	void BuildTurbidityMap();
	// The mid-frame haze change (a fixture breaking): rewrite the kept pixels and
	// let RenderScene copy them into the existing texture. Allocation-free.
	void RefreshTurbidity();
	void FillTurbidityPixels(); // m_map's turbidity -> m_turbidityPixels
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
	// Wakes an attacked monster: latches awareness (sticky) and engages it toward
	// the party THIS frame, independent of its neighbours. Called by
	// MonsterTarget::Noticed alone, so for EVERY attack that reaches the monster
	// (fx::Notice: anything but a DoT's tick), landed or not - a miss, a deflect,
	// a blow soaked or drunk to nothing as surely as a wound - and from any
	// source, a monster's blast or wild swing included (code-review C34).
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
	// The party's side of it: the live monster in square (tx,tz) that member
	// `member`'s melee swing meets, or null (code-review C33). The FRONT RANK of
	// the square's occupants first, then the one in the attacker's own lane, then
	// list order - so a lone or Large occupant is just the monster there.
	Monster* PickMeleeTarget(size_t member, int tx, int tz);
	// A standing member's facing-relative sub-cell position (the quadrant the
	// portraits read: front pair a quarter-cell toward the facing, rear away,
	// even indices the on-screen-LEFT column, facing::SlotSide). Shared by a
	// monster shot's lane test, the ranged lane aim and a crowding monster's
	// slot pick; PickMeleeVictim reads SlotSide itself.
	Vec3 PartyMemberSubPos(size_t member) const;
	// A cardinal bolt's launch point slid onto roster slot `slot`'s QUADRANT
	// LANE: its LATERAL coordinate (the one LaneOffset measures) moved onto that
	// slot's sub-cell position, the rest left alone. The ranged aim, one statement
	// for MonsterRangedAttack and the `bolt` instrument.
	Vec3 AimAtLane(Vec3 origin, const Vec3& dir, size_t slot) const;
	// Resolves a spell bolt reaching `impact.pos` with its strike profile: finds
	// a live monster in that cell, runs the strike (combat + log + slain), and
	// returns true if a monster was there (the bolt is consumed). A landed hit
	// with `impact.push` shoves the survivor that many cells along the bolt's
	// travel (walls/occupants stop it early). The moving-item engine
	// (m_projectiles) owns the bolt; this is the TargetSide::Monsters branch of
	// its impact hook.
	// Index into m_monsters of the first live monster in (cx,cz) that sits in this
	// bolt's LANE, or -1. Shared by the single-target strike and an area carrier's
	// detonation so both ask one question.
	int MonsterInLane(const ProjectileImpact& impact, int cx, int cz) const;
	bool ResolveSpellHit(const ProjectileImpact& impact);
	// TargetSide::Party branch of the moving-item engine's impact hook: a
	// monster bolt reaching the party's cell strikes a random standing member
	// IN ITS LANE (the quadrant mirror of the party's shots — nobody in the
	// lane and it flies straight past; the air ward may deflect whoever it
	// picks) and is consumed (true, hit or miss). False while it is short of
	// the party or slides through an empty lane.
	bool ResolveMonsterProjectileHit(const ProjectileImpact& impact);
	// The moving-item engine's EXPIRY hook: a carrier stopped without striking
	// anything (a wall, or out of reach). Fizzles audibly as it always did, and
	// lands its payload on every combatant of its target side in the square its
	// flight ENDED in (FlightEnd: the last open one, never the wall's own) -
	// CELL-WIDE, where a hit is lane-wide (see the definition for why).
	void ResolveProjectileExpiry(const ProjectileExpiry& expiry);
	// An OPEN square: walkable, with no shut door. What a blast may enter and
	// where a flight may end - one test for both (Detonate, FlightEnd).
	bool OpenSquare(int x, int z) const;
	// Where a flight that stopped ENDS: the last open square along it
	// (blast::LastOpenCell over OpenSquare) - a wall's or a shut door's square
	// never is, though a Wall expiry's position lies inside one (code-review C43,
	// C44). Leaves (cx, cz) as the caller set them when none is within reach.
	void FlightEnd(const ProjectileExpiry& expiry, int& cx, int& cz) const;
	// A bolt that broke against a SHUT, BREAKABLE door in (cx, cz) strikes it
	// (its damage, then its procs - a fire bolt may set it alight) and returns
	// true. False when there is no such door: an immune door is not a target.
	bool StrikeDoorWithBolt(int cx, int cz, const ProjectileExpiry& expiry);
	// The same for a THROWN item (DungeonWorld_Throw.cpp): its blow, an enchanted
	// weapon's element and its on_hit effects. The item still comes down in front.
	bool StrikeDoorWithThrow(int cx, int cz, const ProjectileExpiry& expiry);
	// A THROWN item's two ends (DungeonWorld_Throw.cpp). A strike: a monster in
	// the lane takes the blow through fx::Deal as a swing's (a carried blast
	// bursts instead), the thrower trains `throwing` on CONTACT - a landed blow, or
	// a bomb bursting on the monster (code-review C40) - and the item falls in
	// that cell either way. A landing: the item comes down in the last OPEN square
	// it flew through (ThrownLanding), so a thrown item is never lost - unless it
	// shatters (`throw_breaks`), when what it carried is let go there. A landing
	// trains nothing: only contact with a monster does (Michael).
	bool ResolveThrowHit(const ProjectileImpact& impact);
	void LandThrown(const ProjectileExpiry& expiry);
	// WHERE A THROWN ITEM COMES DOWN when its flight ends without a strike: the
	// last open square along it (blast::LastOpenCell), else the party's own square
	// - never inside the stone, where it could not be picked up again, and never
	// past the party's square. One that `shatters` bursts in any open square; one
	// that lands must be able to rest there (ItemCanRest - short of a pit,
	// code-review C74). The one statement of it for a landing and for a save,
	// which writes a thing still in the air WHOLE, as the floor item it would be
	// if it came down now, in the square it is over (SaveFlyingCargo, so
	// `shatters` false).
	void ThrownLanding(const ProjectileExpiry& expiry, bool shatters, int& cx, int& cz) const;
	// A SAVE'S VIEW OF A THROWN ITEM IN THE AIR (code-review C47): each one, as a
	// floor item with its charge in the square it is over (ThrownLanding at its
	// position now - not where the flight would have ended, which a save cannot
	// know), appended to the live level's snapshot `ls`. The flight itself is
	// left flying - landing it for the save resolved a shattering flask's payload
	// in the live world, so a fire flask burst in or beside the party's square
	// and the save held the damage.
	void SaveFlyingCargo(SaveData::LevelState& ls) const;
	// The training a throw earns on contact: `throwing`, and a creep of the stats
	// it was thrown with (a weapon's own, else Balance's ThrowStats).
	void TrainThrow(Character& thrower, const ItemKind& kind);
	// Every member's throw wait, by roster slot (throw_interval after a throw).
	std::array<float, 4> m_throwCooldown{};
	// Set off an AREA blast on a cell: Game/Blast.h propagates it (a wavefront over
	// ticks, deflecting and reflecting off walls, converging units multiplying) and
	// this plays the result out over time. Unrolled Bursts, and it catches EVERYONE
	// in its squares including the party — a blast has no side and no lane.
	// `payload` carries both the blast's shape and what it LEAVES — a transient
	// front's procs are how fire "catches", so a square the blast passes through
	// keeps burning on its own through the effects pipeline.
	// `spareCentre` leaves the detonation square itself untouched: the blast
	// starts there and spreads outward, but treats it as solid (a ward's burst
	// round the caster - the party's own square takes nothing).
	void Detonate(int cx, int cz, const ProjectilePayload& payload, DamageType type,
				  int attacker, bool spareCentre = false);
	// A blast PLAYING OUT. The propagation is computed once at detonation — the
	// geometry cannot change mid-blast — and its ticks land `rate` seconds apart,
	// which is what makes a fireball rush and a gas cloud creep.
	struct ActiveBlast {
		blast::Result result;
		float rate = 0.0f;
		DamageType type{};
		int attacker = -1;
		ProjectilePayload payload{}; // what each square it reaches is left with
		float elapsed = 0.0f;
		int next = 0; // index of the first hit not yet applied
		// How it LOOKS (ui-updates Phase 10 - blasts had no visual at all): a
		// puff of its element's colour in each square on each tick, a quick flare
		// for a passing front, a slow lingering cloud for a persistent gas.
		Vec4 color{0.7f, 0.65f, 0.6f, 1.0f};
		bool persistent = false;
		// A persistent gas HANGS once it has spread (`blast_linger` seconds),
		// biting again in every square it filled each `rate` (at least 0.3 s).
		float linger = 0.0f;
		float lingerClock = 0.0f;
		// Set once it has spread: its bites from then on say nothing (a line a
		// bite every half second floods the log); a kill or a break still speaks.
		bool lingering = false;
	};
	// One blast square landing, seen and felt: its puff, then ApplyBlastHit.
	void LandBlastHit(const blast::Hit& h, const ActiveBlast& a);
	// The live blasts, in the order they went off: a FIXED table (m_pendingBolts'
	// rule), because a detonation lands in a guarded frame and an ActiveBlast is
	// ~6.5 KB - the vector this was grew on the first blast of every session and
	// on every new peak of overlapping ones (code-review C49, C71). A live blast
	// is NEVER evicted to make room, or a lingering gas would silently stop
	// biting: with the table full, the new one lands whole at once (Detonate).
	static constexpr size_t kMaxActiveBlasts = 32;
	std::array<ActiveBlast, kMaxActiveBlasts> m_activeBlasts{};
	size_t m_activeBlastCount = 0;
	// Advance every live blast and apply whatever has come due. Called per frame.
	void UpdateBlasts(float dt);
	// Apply one tick's worth at one square: monsters, the party (friendly fire),
	// and whatever pieces of dungeon stand there.
	void ApplyBlastHit(const blast::Hit& hit, const ActiveBlast& active);
	// Walk every breakable piece of dungeon standing in a cell — THE one place
	// that knows which kinds those are, so a new one reaches a blast
	// (ApplyBlastHit) and the dev `smash` / `breakables <x> <z> <effect>` at once.
	// NOT a projectile: a bolt or a thrown item that stops against a SHUT DOOR
	// strikes that door alone, straight through DoorTarget (StrikeDoorWithBolt /
	// StrikeDoorWithThrow), so a new kind reaches those only if they learn it.
	// (Forward-declared: the adapter itself is defined further down beside the
	// two combatant ones, and a reference in a std::function needs only an
	// incomplete type.)
	class BreakableTarget;
	void ForEachBreakableAt(int x, int z,
							const std::function<void(BreakableTarget&)>& fn);
	// Fill an instance's Breakable from its type. ONE place, so a prop placed at
	// load and one placed by the editor are breakable on the same terms.
	static void SeedBreakable(Breakable& brk, const DecorationKind& kind);
	// Build the fixture damage side-table from the map's sconces and braziers.
	// Called once the fixtures are placed; only breakable kinds get an entry, so
	// the table is empty in a dungeon that authored none.
	void SeedFixtureBreakables();
	// Douse a broken fixture: its fire goes out through SetFireBurning (light,
	// flame and haze), and a smashed wall bracket drops the torch it held.
	void DouseFixture(const FixtureBreak& fb);
	std::vector<FixtureBreak> m_fixtureBreaks;

public:
	// Deal `amount` of bash damage to everything breakable in a cell; returns how
	// many were struck. The dev console's `smash`, and the seam a future weapon
	// swing at scenery would use.
	int SmashAt(int x, int z, float amount);
	// One line per damageable piece of dungeon (all of them, or one cell's): its
	// hp, whether it is broken, and every effect riding it with its time left -
	// the console's `breakables`, which is how a script watches a door burn.
	std::vector<std::string> BreakableReport(int x = -1, int z = -1) const;
	// Land an effect on every breakable in a cell, as a proc would (its kind's own
	// school). Returns how many took it, or -1 for an unknown effect id.
	int ApplyEffectToBreakables(int x, int z, std::string_view id, float magnitude,
								float seconds);

private:
	// Say what a blow did to a breakable, and what it broke — in that order.
	void NarrateBreak(const BreakableTarget& t, const fx::DamageEvent& ev);
	// A member's ATTACK-side type axis: the potency summed from the weapon in
	// `hand` (-1 = none, for a spell) and every worn piece — the mirror of the way
	// PartyTarget::Resist sums the defender's. Characters have no innate cell; what
	// they carry is what they get, because their own axis is skill.
	ResistTable PartyPowers(const Character& member, int hand);
	// The weapon a hand swings and parries with: what it holds, or nullptr for a
	// bare hand - and for a held thing that is no weapon (no `damage` of its own:
	// a key, a tablet), which is held, not wielded (code-review C39). The same
	// `damage > 0` test as ThrowItem's and the details dialog's. Non-const:
	// ItemKindFor caches.
	const ItemKind* HandWeapon(const Character& member, int hand);
	// The class a weapon swings, trains and parries with: its `skill`, else
	// `unarmed` - for a bare hand (nullptr) and for a weapon naming no class.
	static std::string_view WeaponSkill(const ItemKind* weapon);
	// The party attack formula - damage, attack bonus, type, crit pierce and the
	// over-exertion fumble band - shared by a swing (PartyAttack) and a throw
	// (ThrowItem), so the two can never drift apart.
	AttackProfile PartyAttackProfile(const Character& attacker, const ItemKind* weapon,
									 const AttackSpec& spec, int level, float statAvg,
									 float base) const;
	// The potency of whoever LAUNCHED a carrier: a party member's worn gear (by
	// roster index) or a monster's own table (by runtimeId). Applied where the
	// carrier's DamageEvent is built, which is the only place both are known —
	// MagicSystem is walled off from equipment and cannot do it at cast time.
	ResistTable AttackerPowers(int attacker, u32 shooter);

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
	// Shoves m_monsters[index] up to `cells` squares along (dx, dz), stopping at
	// the first square it cannot enter. Returns how many it moved. The air bolt's
	// push and the Puff of Wind both go through it.
	int ShoveMonster(size_t index, int dx, int dz, int cells);
	// Greedy local step shared by the kite/flee executors: among this monster's own
	// cell and its four free orthogonal neighbours, step to the one MINIMISING
	// `score(x,z)` (its own cell is the baseline, so it holds when nothing beats it).
	// `selfIndex` is its index in m_monsters (excluded from the slot test).
	void GreedyStep(Monster& monster, int selfIndex,
					const std::function<int(int cx, int cz)>& score);
	// Launches a monster bolt from `monster` toward the party through the shared
	// moving-item engine (TargetSide::Party); sets the attack cooldown + swing gesture.
	void MonsterRangedAttack(Monster& monster);
	// The plain ember bolt a monster with no spell looses at the party: from
	// `origin` along `dir`, flying `range` metres, striking with `atk`. Shared
	// by MonsterRangedAttack and the harness's LaunchShot, so a check flies the
	// shot the game does.
	static ProjectileSpec EmberShot(const Vec3& origin, const Vec3& dir, float range,
									const AttackProfile& atk);
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

	// Which roster slot a member is, or -1 if it is not in the roster at all.
	// Pointer arithmetic into the one vector both sides already share, so it
	// needs no extra bookkeeping and cannot drift from the roster's order.
	int MemberIndex(const Character& who) const {
		if (!m_roster || m_roster->empty()) return -1;
		const ptrdiff_t i = &who - m_roster->data();
		return (i >= 0 && i < static_cast<ptrdiff_t>(m_roster->size()))
				   ? static_cast<int>(i)
				   : -1;
	}

	// WHAT A FUMBLE COSTS THE PERSON WHO THREW IT (docs/damage-system.md "When
	// it goes wrong"). `face` is the die face the fumble was judged on — at
	// fumble_severe_face or below the severe table fires as well.
	//
	// Two functions rather than one taking an abstraction, deliberately: the six
	// consequences act on inventories, stamina bars and neighbours, and the two
	// sides of this game share none of those. It is the same split as
	// PartyTarget/MonsterTarget, and it keeps the part that IS shared — WHICH
	// entries fire — in the pure, tested mishap:: layer where it belongs.
	//
	// A consequence with nothing to act on is a NO-OP, never an error: `drop`
	// with an empty hand, `stumble` on a monster that has no stamina bar. That
	// is what lets one default table serve a knight, a bare fist and a claw.
	void PartyFumble(Character& attacker, size_t hand, const ItemKind* weapon,
					 const AttackProfile& atk, int face);
	void MonsterFumble(Monster& monster, const AttackProfile& atk, int face);
	// The harness's LOADED DIE (Harness::LoadedFumble): when it is armed for this
	// member's hand, turns `ev` into a fumble on a severe face (1) or a plain one
	// (the top of this attack's band), spends it and returns true - the caller
	// then skips the roll. False, touching nothing, otherwise.
	bool TakeLoadedFumble(size_t member, size_t hand, const AttackProfile& atk,
						  fx::DamageEvent& ev);
	// The consequence table a source actually uses: its own when it authored
	// one, else the balance.cat default. Resolved per fumble so a Balance dialog
	// change lands on the next swing rather than on the next level load.
	// A VIEW, copying nothing: onto `own`, or onto `fallback` (the caller's
	// inline default table, filled here), since a fumble is a steady-state event.
	// `own` is a span too, so bare hands pass an empty one rather than a vector.
	std::span<const mishap::Entry> FumbleTable(std::span<const mishap::Entry> own,
											   bool severe,
											   mishap::DefaultTable& fallback) const;
	// Lay an item on the floor of a cell as a RUNTIME drop (negative id, saved
	// as a `drop` diff) — NOT an .ent record, which is what an editor placement
	// authors. Shared by the cursor drop and by a fumbled weapon. `charge` is
	// what the item has left: a caller dropping an item it HOLDS passes the
	// slot's, since -1 reads as untouched and the floor refills a torch from it.
	void DropItemInCell(const std::string& typeId, int cx, int cz, float charge = -1.0f);
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
		// Sized by the CEILING, not the live count: this is a stack array on a
		// per-frame path, so it must have a compile-time size.
		std::array<float, kMaxDamageTypes> bite{};
		for (fx::Inst& e : effects) {
			// AN EFFECT BITES FOR THE TIME IT ACTUALLY HAD, not for the whole
			// step. At frame dt the two are the same to within a rounding error
			// and this went unnoticed for the life of the system; at the
			// SIXTY-SECOND slices a world-map journey settles in, a four-second
			// bleed was dealing sixty seconds of damage and killing outright.
			//
			// The bug was always here — a DoT with 0.2s left on a 0.5s frame
			// over-applied by more than half — and it took a caller with a
			// coarse dt to make it visible. That is worth remembering about
			// anything else that multiplies a rate by dt without asking how
			// much of dt it was entitled to.
			const float had = std::min(dt, std::max(0.0f, e.timeLeft));
			e.timeLeft -= dt;
			if (e.IsDot())
				bite[e.kind->DamageTypeOf(e).index] += e.magnitude * had;
			if (e.timeLeft <= 0.0f) onExpire(e);
		}
		std::erase_if(effects,
					  [](const fx::Inst& e) { return e.timeLeft <= 0.0f; });
		for (size_t i = 0; i < bite.size(); ++i) {
			if (bite[i] <= 0.0f) continue;
			fx::DamageEvent ev = fx::DamageEvent::Tick(
				DamageType{static_cast<u8>(i)}, bite[i], DotSource(effects));
			fx::Deal(ev, target, m_balance.Strike(), m_combatRng);
		}
	}
	// Who to credit a DoT tick's damage to: the first sourced DoT on the list
	// (they are nearly always one, and threat is a coarse signal — the point
	// is that a hit-and-run torch keeps the grudge alive).
	static int DotSource(const std::vector<fx::Inst>& effects);
	// Where a burning body's flames rise from (torso height above visualPos,
	// moved with the rig's root joint as the body animates): the per-frame
	// plume origin and the glow agree because both ask here.
	static Vec3 BurnOrigin(const Monster& monster);
	// A monster's model space -> world: centred on its rig root (MonsterKind::
	// rigRest), scaled, faced, stood on visualPos. The ONE statement of where a
	// monster's model is, for the draw and for anything attached to the body.
	static Mat4 MonsterModelWorld(const Monster& monster);
	// How the flames READ per school — the FireEffect palette is authored
	// orange, so fire burns untinted and the other three recolour it (a water
	// burn is the freezing kind: the plume runs cold blue).
	static Vec3 BurnTint(SpellSymbol school);
	// The same for one burn: its OWN colour when it carries one (fx::Inst::tint,
	// a magical torch's flame), else its school's. BurnTintFor is the plume's
	// multiplier, BurnGlow the light's colour - the plume and its light ask
	// these, so a blue torch's burn is blue in both.
	static Vec3 BurnTintFor(const fx::Inst& burning);
	static Vec3 BurnGlow(const fx::Inst& burning);
	// The colour a held item lends what it sets alight (a LIT item's
	// `flame_color`), or null for an ordinary one.
	static const Vec3* FlameTintOf(const ItemKind& kind);
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
	void MemberMessage(const Character& member, std::string_view line) const;
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
	// THE RESOURCE PRACTICES, awarded by THROUGHPUT (docs/health-and-healing.md):
	// `points` is stamina spent / mana spent / health regained, scaled by that
	// pool's own xp knob. One expensive spell therefore trains attunement more
	// than three cheap ones — "the more it channels through you" meant literally.
	//
	// THE WHOLE REASON THIS IS NOT JUST A GrantSkillXp CALL AT THREE SITES: these
	// three skills must creep NO stat. Every other skill in the game drips its
	// associated stats forward, and each of these three feeds a pool that its
	// aptitude ALSO feeds — so the ordinary award would close a loop on itself
	// (spend stamina, creep vitality, grow max stamina). Routing them through one
	// function that passes an empty stat list makes that a property of the code
	// rather than a rule three call sites have to keep remembering.
	void GrantResourceXp(Character& member, resource::Kind kind, float points);
	// --- supplies (docs/health-and-healing.md "Food and water") ---------------
	// One frame of a member's food and water: drain by time (scaled by
	// conditioning — its price), then raise or clear the starving/parched
	// effect. It does NOT deal the damage; the effects do, through the ordinary
	// DoT tick, which is the whole reason they are effects.
	void TickSupplies(Character& member, float dt);
	// One frame of the rest STATE: the reasons it ends by itself. A no-op when
	// not resting, so the ordinary frame pays a bool for it.
	void UpdateRest();
	// End rest because something happened, saying why. Safe to call when not
	// resting (it does nothing), which is what lets every attack that reaches a
	// member (PartyTarget::Noticed) call it unconditionally rather than testing
	// the flag at the call site.
	void BreakRest(const char* reason, const char* reasonKey);
	// How long a starving/parched instance is given each frame it is held open.
	// Nominal — long enough that the aging loop can never expire it between two
	// supply ticks, short enough that if this code ever stopped running the
	// effect would lift by itself rather than sticking forever.
	static constexpr float kDeprivationHold = 5.0f;
	// Drain both meters by a stamina SPEND (water more than food — sweat is
	// water). Called from SpendStamina, so every exertion in the game pays it.
	void DrainSuppliesByExertion(Character& member, float points);
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
	// strike, mitigate, absorb — is shared. After it comes Noticed, the other
	// per-side answer: to being ATTACKED at all, landed or not (a monster
	// wakes, a resting party is roused).
	class PartyTarget final : public fx::ITarget {
	public:
		PartyTarget(DungeonWorld& world, Character& member)
			: m_world(world), m_member(member) {}
		float Evasion(DamageType type) const override;
		float Soak() const override;
		float Resist(DamageType type) const override;
		std::vector<fx::Inst>& Effects() override { return m_member.effects; }
		void Wound(float amount, fx::DamageEvent& ev) override;
		void Absorb(float amount, fx::DamageEvent& ev) override;
		// An attack reached a member, landed or not: it ends a rest.
		void Noticed(const fx::DamageEvent& ev) override;
		loc::Line Name() const override { return loc::Line{m_member.name}; }
		void Say(std::string_view line) const override;
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
		float Evasion(DamageType type) const override;
		float Soak() const override;
		float Resist(DamageType type) const override;
		std::vector<fx::Inst>& Effects() override { return m_monster.effects; }
		void Wound(float amount, fx::DamageEvent& ev) override;
		void Absorb(float amount, fx::DamageEvent& ev) override;
		// An attack reached a living monster, landed or not: it wakes and turns
		// on the party (ProvokeMonster).
		void Noticed(const fx::DamageEvent& ev) override;
		loc::Line Name() const override;
		void Say(std::string_view line) const override;
		void SayApplied(const fx::EffectKind& kind) const override;

	private:
		DungeonWorld& m_world;
		Monster& m_monster;
	};

	// THE DUNGEON AS A TARGET — the third implementation of fx::ITarget, after the
	// two combatant kinds, and the one that is not a combatant at all. A door, a
	// barrel or a brazier reaches the damage pipeline through exactly the same
	// interface a monster does, so everything already built works on it for free:
	// resists, absorption past 1.0, DoTs (a burning door burns DOWN), a blast.
	//
	// ONE adapter serves every breakable kind. What differs between them is only
	// what BREAKING means — a door's way opens for good, a prop is removed, a
	// brazier goes dark — and that is a callback rather than a subclass, because
	// the damage side of a barrel and of a door are identical and only their
	// consequence differs.
	//
	// It does not dodge: Evasion is 0 whatever arrives. An inert thing has no
	// guard, which is the honest answer and also what makes a swing at scenery
	// feel different from a swing at something that is trying not to be hit.
	//
	// It holds VIEWS, never strings: the name is a lang-key prefix plus the type id
	// it borrows from the piece (resolved through loc::ViewKey), and the broken key
	// is a literal. A burning piece builds one of these every frame it burns
	// (TickBreakables), inside the frames the allocation guard watches, and the
	// "door." + type it used to concatenate allocated each time.
	class BreakableTarget final : public fx::ITarget {
	public:
		BreakableTarget(DungeonWorld& world, Breakable& brk, std::string_view namePrefix,
						std::string_view nameId, std::string_view brokenKey,
						std::function<void()> onBroken)
			: m_world(world), m_brk(brk), m_namePrefix(namePrefix), m_nameId(nameId),
			  m_brokenKey(brokenKey), m_onBroken(std::move(onBroken)) {}
		// The line for "this broke", said by the CALLER once it has narrated the
		// blow — the same rule a monster's death line follows, and for the same
		// reason: "the barrel is smashed" has to read AFTER the hit that smashed it,
		// and a callback fired from inside the pipeline runs before the caller has
		// said anything at all.
		std::string_view BrokenKey() const { return m_brokenKey; }
		float Evasion(DamageType) const override { return 0.0f; }
		float Soak() const override { return m_brk.soak; }
		float Resist(DamageType type) const override;
		std::vector<fx::Inst>& Effects() override { return m_brk.effects; }
		void Wound(float amount, fx::DamageEvent& ev) override;
		void Absorb(float amount, fx::DamageEvent& ev) override;
		// Nothing: a door does not wake, and only its wounds change it.
		void Noticed(const fx::DamageEvent&) override {}
		loc::Line Name() const override;
		void Say(std::string_view line) const override;
		void SayApplied(const fx::EffectKind& kind) const override;

	private:
		DungeonWorld& m_world;
		Breakable& m_brk;
		std::string_view m_namePrefix; // "door." / "decoration." / "fixture."
		std::string_view m_nameId;     // the piece's own type id, borrowed
		std::string_view m_brokenKey;
		std::function<void()> m_onBroken; // small captures only: stored inline
	};
	// The adapter for each breakable kind - ONE place per kind says what it is
	// called and what breaking it does, shared by ForEachBreakableAt (a blast,
	// `smash`), TickBreakables (an effect riding it) and, for a door, the bolt
	// and the throw that strike it, so they cannot drift.
	BreakableTarget DoorTarget(Door& d);
	BreakableTarget DecorationTarget(Decoration& p);
	BreakableTarget FixtureTarget(FixtureBreak& fb);
	// ONE frame of every breakable's effects: age them and let their DoTs bite,
	// through the same TickEffects a combatant uses - so a door left alight burns
	// DOWN, and a ward on a crate runs out. Only pieces carrying an effect build an
	// adapter, so a dungeon with nothing alight costs a walk of three lists.
	void TickBreakables(float dt);

	// The balance knobs an effect's own maths needs, in the shape the module
	// takes them (it never sees Balance.h).
	fx::Knobs EffectKnobs() const { return {m_balance.stoneskinResist}; }
	// What a reacting effect deals its reprisal through (a fire shield's burn
	// goes back out via fx::Deal like any other damage), so every React call
	// site hands over the same two things this world resolves damage with.
	fx::ReactCtx Reaction() { return {m_balance.Strike(), m_combatRng}; }
	// --- the one-pipeline check's world side (DungeonWorld_Ledger.cpp) --------
	// Observe every value the damage pipeline can reach. The ONE place targets
	// are enumerated: a future fourth kind of thing that can be hurt is covered
	// by adding it there, and if it is not there it is not checked.
	void SweepDamageLedger();
	// Verify the region since the last checkpoint and take a new baseline.
	// `phase` names that region and must outlive the call (a string literal).
	void CheckDamageLedger(const char* phase);
	std::string LedgerSubjectName(ledger::Key key) const;
	ledger::Ledger m_damageLedger;
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
	// WHERE A MONSTER MAY STAND (code-review C58, C74) - the one statement of
	// it, asked by the formation's side list, FreeSlotInCell and BuildAISnapshot.
	// A square holds a monster's body when it is floor (in bounds, walkable) with
	// no brazier, has a floor UNDER it (not a pit or a stairwell - FloorHoleAt),
	// and holds no solid decoration and no shut door. Occupancy (the party, other
	// monsters, slots) is not part of it; FreeSlotInCell adds that.
	// MonsterGroundAt is the MAP's half alone, which the AI snapshot caches by
	// map revision; the snapshot asks MonsterCanStand for the rest.
	bool MonsterGroundAt(int x, int z) const;
	bool MonsterCanStand(int x, int z) const;
	// Every square of a `size` body anchored at (x,z) - a Huge's 2x2 block - is
	// somewhere a monster can stand, and none of them is the party's.
	bool FootprintCanStand(int x, int z, SizeClass size) const;
	// WHERE A THING MAY COME TO REST on the floor (C74): walkable, a floor under
	// it (not a pit or a stairwell, where it would hang over the shaft) and no
	// shut door. Asked by the cursor drop (DropItemAt), a throw's landing
	// (LandThrown) and a fumble's fling, which refuse such a square rather than
	// send the thing down to the level below.
	bool ItemCanRest(int x, int z) const;
	// The index of a free sub-cell SLOT for a monster of `size` standing on (x,z),
	// or -1 if none (FootprintCanStand refuses it, full, or already held by a
	// different-size group). `self` (a monster array index, or -1) is excluded from
	// the occupancy scan. Slots are filled lowest-index-first. See Game/SlotGrid.h.
	int FreeSlotInCell(int x, int z, SizeClass size, int self) const;
	// A solid decoration standing on (cx,cz)? Blocks monsters exactly like the
	// party (the isOccupied lambda checks the same flag). Wall-mounted props
	// default non-solid, so only floor-standing blockers register.
	bool SolidDecorationAt(int cx, int cz) const;

	// True if a caster that moves EVERY frame is within the light's reach - a
	// monster (alive, or still playing its death), a thrown item in flight -
	// so its cube must re-render. Fed to m_shadows.ShouldRender as the world's
	// per-light verdict.
	bool MovingCasterNear(const gfx::PointLight& light) const;
	// A shadow caster CHANGED (code-review C178) - appeared, vanished, or moved
	// in a way no light's own state shows - so every cube reaching it must
	// re-render: notes to m_shadows, in a sphere as wide as what the shadow
	// pass draws there. A door's leaf each frame it travels (and the snap of a
	// smashed one), a lever thrown, a floor item lifted / set down / burnt out,
	// a monster's body gone, a smashed prop, a wall torch taken or put back, an
	// Earth stone set down or spent, a thrown item's flight ending. A NEW place
	// where something the shadow pass draws can change (SubmitSceneGeometry)
	// must note it too, or the shadow it left stays in the cube until the light
	// moves (`shadows status`; AllocTest -Lights opens a door and checks).
	void NoteDoorCaster(const Door& door);
	void NoteItemCaster(const Item& item);
	void NoteMonsterCaster(const Monster& monster);
	void NoteCellCaster(int x, int z);
	// The sphere round a door's cell that holds everything it draws (frame,
	// leaf at any travel, trim): the draw's cull and its caster note share it.
	static float DoorReach(const Door& door);
	// The draw's cull spheres for a monster and a thrown item, shared with
	// their caster tests.
	static constexpr float kMonsterCasterRadius = 0.65f * kUnit;
	static constexpr float kCargoCasterRadius = 0.35f * kUnit;

	// Reveals a cell and its eight neighbors in the fog-of-war set.
	void MarkSeen(int x, int z);

	// Captures the ACTIVE level's live dynamic state (revealed cells + monster
	// diff) as a SaveData::LevelState. Shared by StashActive and CaptureState.

	// The shared tail of both installs: replace a stashed level, or the ACTIVE
	// one in place (Party holds a reference to m_map, so the object persists
	// and only its data changes).
	bool InstallLevel(const std::string& stem, DungeonMap&& map,
					  DungeonEntities&& ents);
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
	// Rebuilds the single chunk region (chunkX, chunkZ) in place, and its
	// pre-built niche walls (PrebuildNicheLooks) from the map as it now stands.
	void RebuildChunkRegion(int chunkX, int chunkZ);
	// The region builder with the world's blocks, holes and feature meshes, as
	// the map stands: what RebuildChunkRegion uploads and a niche look is made of.
	DungeonGeometry BuildChunkGeometry(int chunkX, int chunkZ);

	// --- pre-built niche walls (code-review C211) ------------------------------
	// A named niche's open state decides its own wall panel and the pins of the
	// two panels beside it along its wall, so a lever's reveal used to rebuild
	// those chunks IN PLAY: a GPU drain, a region build and an upload, in a frame
	// the allocation guard arms - the only play-time geometry change there is.
	// Each chunk such a niche reaches now keeps its WALL chunks for every
	// combination of the lever names reaching it, built with the surfaces
	// (BuildDungeonMeshes) and again whenever an edit rebuilds the chunk
	// (RebuildChunkRegion), and a press SWAPS the matching look into
	// m_walls.chunks: unique_ptrs moved, nothing built, freed or waited on - the
	// look it replaces is kept for the next press. Keyed by NAME, not by niche,
	// because a press flips every niche of a name at once; a chunk reached by k
	// names holds 2^k looks, counted from the state it was built in. Floors and
	// ceilings never depend on a niche, so only the walls are held, and a chunk
	// with no walls in any state (reached through rock or open floor) holds none.
	static constexpr int kNicheLookNames = 4; // names per chunk: 16 looks at most
	struct NicheLooks {
		int chunk = -1;
		// The lever names that reach this chunk's walls, in niche order. Past
		// kNicheLookNames the chunk holds no looks (`prebuilt` false): a press
		// there rebuilds it the old way, which the build warns about and the
		// allocation guard reports - an authoring limit, never a silent cost.
		std::vector<std::string> names;
		bool prebuilt = false;
		// Bit i set = names[i] flipped an odd number of times since the build.
		u32 live = 0;
		// looks[state] = this chunk's wall SurfaceChunks in that state, in the
		// order m_walls.chunks holds the chunk's own (variant order: a niche
		// stamps into its wall's bucket, so every state fills the same buckets).
		// looks[live] is empty - those meshes are the live ones.
		std::array<std::vector<SurfaceChunk>, 1u << kNicheLookNames> looks;
	};
	std::vector<NicheLooks> m_nicheLooks;
	// The chunks a niche's state reaches: its own cell's, and those of the two
	// cells beside it along its wall (their panels pin against its face).
	// Returns how many distinct ones it wrote.
	int NicheChunksOf(const WallNiche& n, int (&out)[3]) const;
	// Every chunk's looks again (the full bake), or one chunk's (an edit's
	// rebuild). The caller has drained the GPU: the looks dropped may have been
	// on show in a frame still in flight.
	void PrebuildNicheLooks();
	void PrebuildNicheLooks(int chunk);
	// Puts look `state` on show for `nl`'s chunk and keeps the one it replaces.
	// False (nothing moved) when the chunk holds no such look.
	bool SwapNicheLook(NicheLooks& nl, u32 state);
	// While a chunk batch is open (m_chunkBatch > 0), RebuildChunksAround only
	// RECORDS the chunks it would rebuild; EndChunkBatch rebuilds each once.
	int m_chunkBatch = 0;
	std::vector<int> m_batchedChunks; // chunk indices, deduplicated at the end
public:
	// Batches the chunk rebuilds of a multi-cell edit (a rectangle, a flood, an
	// area fill): N painted cells used to cost N x (GPU drain + up to 5 chunk
	// builds); inside a batch they cost one drain and one build per distinct
	// chunk, at EndChunkBatch. Nests - only the outermost End rebuilds.
	void BeginChunkBatch() { ++m_chunkBatch; }
	void EndChunkBatch();
private:
	// Uploads every chunk in `geo` as ONE batch (gfx::CreateMeshes) and appends
	// them to the three surfaces. Shared by the full bake and the region rebuild.
	void AppendSurfaceChunks(DungeonGeometry& geo);

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
	// lights.cat, parsed (Game/LightProfile.h). Looked up BY ID every frame
	// (LightProfileFor - a handful of profiles, so a linear scan); a reload
	// replaces the vector, which is why nothing holds a pointer into it.
	std::vector<light::Profile> m_lightProfiles;
	// trails.cat, parsed (Game/Trail.h) - the same arrangement.
	std::vector<trail::Profile> m_trailProfiles;
	// Where each of this frame's lights came from, parallel to m_lights.points
	// up to the budget cut (filled beside the push; read by the `lights` dev
	// command). Reserved to the ceiling, so filling it allocates nothing.
	struct LightOrigin {
		const char* source = ""; // "fire", "torch", "burning", ...
		int profile = -1;        // index into m_lightProfiles; -1 = the fallback
		float fade = 1.0f;       // the budget fade it was drawn at (SelectLights)
	};
	std::vector<LightOrigin> m_lightOrigins;
	// A light's STABLE identity, frame to frame (gfx::PointLight::id): what
	// kind of source it is, and which one. The shadow-cube cache and the
	// budget fades both key on it. Never 0, and never the high bit (the
	// scheduler's index-keyed fallback).
	enum class LightKind : u32 {
		Torch = 1, Fire, Burning, Glow, Sight, Stress,
		Bolt,      // a flight in the air, keyed by its projectile id
		FloorTorch, // a lit torch lying on the floor, keyed by its m_items index
		Flash,     // the moment a lit bolt leaves where it ended, keyed by slot
		HandGlow,  // a hand spell's puff of light (HandPuff), keyed by its slot
		Worn,      // an item on the doll or in a hand giving light: member x slots + slot
		Spell,     // a Sowilo light on a member: member x 4 + school
		Stone,     // an Earth light set down (Stonelight), keyed by its m_lightStones slot
	};
	static u32 LightKey(LightKind kind, u32 index) {
		return (static_cast<u32>(kind) << 24) | (index & 0xFFFFFFu);
	}
	// Pushes one light from `profile` at `pos` (metres), its colour `color`
	// when the profile takes its source's, scaled by `brightness` (a torch's
	// charge) and with `radiusMetres` overriding the profile's reach when > 0
	// (a placed fire's own Brightness). Returns the pushed light, or NULL in two
	// cases, and a caller that adjusts the light afterwards must check for both:
	//   - the light is out (brightness 0), and
	//   - the frame's candidate list is already full (kLightCandidates): the
	//     light is REFUSED, and counted in m_lightRefusals by `source`. Refusal
	//     goes by push ORDER, not distance - whatever is pushed last is what a
	//     full list drops (`lightstress fill` fills it ahead of the fires).
	gfx::PointLight* PushLight(const light::Profile& profile, const char* source, u32 key,
							   const Vec3& pos, float time, float phase,
							   const Vec3& color = {1, 1, 1}, float brightness = 1.0f,
							   float radiusMetres = 0.0f);

	// --- the light budget (DungeonWorld_LightBudget.cpp, lighting-updates P3) --
	// Every light pushed this frame is a CANDIDATE; SelectLights decides which
	// are drawn: (1) one whose sphere reaches no pixel of the view is dropped
	// (Graphics/LightTiles.h's own rect, so the cull and the shader agree), (2)
	// so is one the party cannot reach on the grid - a sealed-off room's light
	// would only bleed through its walls, (3) the rest RANK by what they add to
	// the view and the top Max Lights are kept, a held torch always, and (4) a
	// light crossing that budget line FADES rather than switching.
	void SelectLights(const Vec3& eye, float time);
	// Re-walks the party's reach (a BFS over walkable squares) when the party's
	// square or the map's revision has changed since the last walk.
	void RefreshReach();
	// Whether the party can reach the square a light at `pos` stands in, within
	// what its reach and the view's depth could ever make visible.
	bool LightReachable(const Vec3& pos, float radius) const;
	std::vector<u16> m_reach;       // grid steps from the party; 0xFFFF = cannot reach
	std::vector<int> m_reachQueue;  // the BFS's queue, kept for its capacity
	int m_reachX = -1, m_reachZ = -1;
	u32 m_reachRevision = 0xFFFFFFFFu;
	// A light's budget fade, by key: 1 = fully in. A light kept by the budget
	// fades in, one it drops fades out (still drawn while there is room under
	// the hard ceiling), one merely off-screen keeps its value - so turning
	// round to a fire does not show it brightening.
	struct LightFade {
		u32 key = 0;
		float fade = 0.0f;
		float lastSeen = -1.0e9f;
		bool kept = false; // in the budget last frame (the ranking's incumbents)
	};
	// Room for every candidate (kLightCandidates) plus a margin for lights
	// briefly off screen, so a busy frame does not evict a fading one.
	std::array<LightFade, 320> m_lightFades{};
	LightFade& FadeFor(u32 key, float time, bool& fresh);
	void ClearLightFades() { m_lightFades.fill(LightFade{}); }
	float m_lastLightTime = -1.0f;
	// What SelectLights did with this frame's candidates (the `lights` readout).
	struct LightCull {
		u32 candidates = 0, offscreen = 0, unreachable = 0, budget = 0, fadingOut = 0;
	};
	LightCull m_lightCull;
	// The selection's scratch, all reserved at construction: a frame allocates
	// nothing however many lights it pushes, up to kLightCandidates.
	static constexpr size_t kLightCandidates = 256;
	struct LightCandidate {
		u32 index;
		float score;
		float fade;
		bool keep;
	};
	std::vector<LightCandidate> m_lightCandidates;
	std::vector<gfx::PointLight> m_lightScratch;
	std::vector<LightOrigin> m_lightOriginScratch;
	// `lightstress <n>`: n test lights scattered round the party (a measuring
	// load for the budget and the tiles; never saved). Pushed after the fires and
	// glows, or with m_stressFirst (`lightstress fill`) AHEAD of the fires, so a
	// load of kLightCandidates leaves every fire after it to find the list full.
	struct StressLight {
		Vec3 pos;
		Vec3 color;
	};
	std::vector<StressLight> m_stressLights;
	bool m_stressFirst = false;
	void AppendStressLights(float time);
	// The lights PushLight refused this frame because the candidate list was
	// full, by source ("fire", "stress", ...; the `lights` readout's ceiling
	// line). Fixed, so counting allocates nothing; a ninth source counts only in
	// the total.
	struct LightRefusal {
		const char* source = nullptr;
		u32 count = 0;
	};
	std::array<LightRefusal, 8> m_lightRefusals{};
	u32 m_lightRefusedTotal = 0;
	// The view's depth in metres (the camera's far plane), which also bounds
	// how far away on the grid a light can still matter (LightReachable).
	static constexpr float kFarPlane = 100.0f;
	// Shadow-slot budgeting + cube-cache scheduling (UpdateLights feeds it the
	// frame's lights; RenderShadowMaps asks it which cubes to redraw). See
	// ShadowScheduler.h.
	ShadowScheduler m_shadows;
	// The edit revision the cubes were last checked against: an editor edit
	// re-renders every cube once (RenderShadowMaps).
	u64 m_shadowEditRevision = 0;
	gfx::Atmosphere m_atmosphere; // per-cell air turbidity (dust)
	std::unique_ptr<gfx::Texture> m_turbidityMap;
	// Its pixels, kept: a fixture breaking mid-fight changes the haze, and the
	// in-place refresh (RefreshTurbidity) rewrites these and has the render copy
	// them into the existing texture - no new texture, no GPU drain, no heap.
	std::vector<u8> m_turbidityPixels;
	bool m_turbidityDirty = false;
	// The frame's dust puffs (gfx::Atmosphere::dustPuffs), DERIVED from the
	// haze effects the fires carry (a doused fire's smoke) - the strongest
	// kMaxDustPuffs of them - never stored, so they cannot drift from the
	// effect lists that are the truth.
	void GatherDustPuffs(gfx::Atmosphere& atmo) const;
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
	// Swatch thumbnails (LoadSurfaceThumb), by texture SET name - a set is a
	// pool asset, so one survives level, world and quality changes. A null
	// texture was tried and missing. Bounded by the surface catalogs (~21 KB
	// and one SRV slot apiece). `stem` is the file it came from.
	struct SwatchThumb {
		std::unique_ptr<gfx::Texture> texture;
		std::string stem;
	};
	std::unordered_map<std::string, SwatchThumb> m_surfaceThumbs;
	// Sets a draw asked for and found untried (SurfaceSwatchForId), loaded by
	// LoadWantedSwatches. Mutable: the asking is from const draw-time lookups.
	mutable std::vector<std::string> m_swatchWants;
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
	// WHICH worn blocks those are: the mesh tier and the three set lists they were
	// loaded for. LoadDungeonBlocks skips the reload when a level change asks for
	// exactly this again (the common case - levels share a palette). Unset =
	// nothing loaded, or ReloadDungeonBlocks asked for a fresh read because the
	// FILES may have changed under the same names (a restyle rebake).
	struct BlockSetKey {
		std::string tier;
		std::vector<std::string> walls, floors, ceilings;
		bool operator==(const BlockSetKey&) const = default;
	};
	std::optional<BlockSetKey> m_loadedBlocks;
	// Feature meshes by MODEL FILE (a feature type's `model`, ModelFileOf). Features are
	// project-wide, not per level, so each file is read once per world and the
	// per-type maps below point into this. Node-based on purpose: the maps hold
	// pointers, which a flat_map would invalidate on insert.
	std::unordered_map<std::string, assets::MeshData> m_featureMeshCache;
	// Niche panels by wallfeatures.cat type (each entry's `model`); the mesh
	// builder stamps the one matching a niche's type. NicheMeshFor resolves it.
	std::flat_map<std::string, const assets::MeshData*> m_nicheMeshes;
	const assets::MeshData* NicheMeshFor(const std::string& type) const;
	// See-through bore panels by wallfeatures.cat type (its `model`); stamped
	// on the two flanking faces of a bored wall block. BoreMeshFor resolves it.
	std::flat_map<std::string, const assets::MeshData*> m_boreMeshes;
	const assets::MeshData* BoreMeshFor(const std::string& type) const;
	// Surface-feature tiles by surfacefeatures.cat type (its `model`), split
	// by the type's `surface` so each resolver answers only for its own side -
	// which is what lets the builder ask "is there a floor feature here?" and
	// "is there a ceiling one?" independently, without knowing the catalog.
	std::flat_map<std::string, const assets::MeshData*> m_floorFeatureMeshes;
	std::flat_map<std::string, const assets::MeshData*> m_ceilingFeatureMeshes;
	const assets::MeshData* FloorFeatureMeshFor(const std::string& type) const;
	const assets::MeshData* CeilingFeatureMeshFor(const std::string& type) const;
	// True if the surfacefeatures.cat type mounts on the ceiling (`surface =
	// ceiling`). The editor routes a placement through this.
	bool FeatureIsCeiling(const std::string& type) const;


	std::flat_map<std::string, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	// Transparent, so ItemRune looks a kind up by a view without building a string.
	std::flat_map<std::string, std::unique_ptr<ItemKind>, std::less<>> m_itemKinds;
	// Baked 3D item-icon thumbnails: each model ItemKind owns its RT texture
	// (ItemKind::iconTarget), rendered once before the first scene via
	// BakeItemIconsIfNeeded. Shared depth target + halo for the bakes.
	// (kIconSize is public — a caller that supplies its own target must match it.)
	gfx::ComPtr<ID3D12Resource> m_iconDepth;
	gfx::ComPtr<ID3D12DescriptorHeap> m_iconDsvHeap;
	std::unique_ptr<gfx::Texture> m_iconHalo; // soft round disc, white w/ radial alpha
	bool m_itemIconsBaked = false;
	// Rendered frames so far (NewFrame counts them; 0 = none yet, as in a
	// headless run): what DrawPart stamps a part with, so a readout can tell a
	// draw on the last frame from an older one.
	u64 m_drawFrame = 0;
	// Monster head-shot + decoration whole-model map icons (each kind's
	// iconTarget), sharing the item bakes' depth/halo. A flag resets when a new
	// kind loads so it bakes next frame; the two fixture icons gate on their
	// texture existing instead (their meshes load once at boot).
	bool m_monsterIconsBaked = false;
	MonsterIconBake m_monsterIconBake; // the last monster pass (LastMonsterIconBake)
	bool m_decorationIconsBaked = false;
	// Creates the shared icon depth target + halo on first use (all bakers).
	void EnsureIconBakeTargets();
	// One kind's head-shot bake: the model in its icon pose, framed on its top.
	// Records what it drew in kind.iconDrawn.
	void BakeMonsterIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
						 MonsterKind& kind);
	// (Re)poses a kind's icon on its idle's first frame and measures that pose
	// (MonsterKind::iconPose / iconLo / iconHi). At load, and again when the
	// monster-config dialog changes which clips are its idle.
	static void PoseMonsterIcon(MonsterKind& kind);
	// A static model baked whole (fit by its bounds): decorations, fixtures, the
	// asset picker's tiles. One part for a plain mesh, one per primitive else.
	void BakeMeshIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
					  std::span<const gfx::PreviewSubmesh> parts, const Vec3& lo,
					  const Vec3& hi, const gfx::Texture& target,
					  std::span<const Mat4> palette = {}, float tilt = 0.3f, float yaw = 0.5f);
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
	// The party's own light (DungeonWorld_Light.cpp): the cursor's item
	// (borrowed, SetCursorItem), a scratch slot the cursor's torch burns
	// through, and the per-frame passes.
	HeldItem* m_cursorItem = nullptr;
	ItemSlot m_cursorScratch;
	// DropAtPartyFeet's id, assigned rather than constructed (a guarded frame),
	// into an item's room (kItemIdCapacity).
	std::string m_dropIdScratch = ItemIdBuffer();
	// The glows HandPuff leaves, each fading over its `life` (UpdateLights adds
	// the live ones). FIXED: a cast lands in a guarded frame; a fifth puff while
	// four still glow takes the oldest's place.
	struct HandGlow {
		Vec3 pos{};
		Vec3 color{};
		float timeLeft = 0.0f;
		float life = 0.0f;
		float intensity = 0.0f; // at the puff; fades to nothing over `life`
		bool flare = false;     // a light spell's flare (`spell_flare`), not a puff
	};
	std::array<HandGlow, 4> m_handGlows{};
	void TickHandGlows(float dt);
	// Bolts waiting their turn (a volley's later shots - the cast service
	// spawnBoltAfter, and a monster mage's volley): a FIXED queue, because a
	// cast lands in a guarded frame. Transient, like the bolts in flight: a
	// level change or a reset drops them.
	struct PendingBolt {
		ProjectileSpec spec;
		float delay = 0.0f;
	};
	std::array<PendingBolt, 32> m_pendingBolts{};
	size_t m_pendingBoltCount = 0;
	void SpawnBoltAfter(const ProjectileSpec& spec, float delay);
	void UpdatePendingBolts(float dt);
	// EVERY LAUNCH goes through Launch (lighting-updates Phase 4): it dresses the
	// spec, then flies it. DressFlight resolves a spec's light and trail - the
	// names it carries (a spell's own `light` / `trail`), else its cargo kind's,
	// else its school's (`bolt_<school>`, `trail_<school>`) - into the handle and
	// copied trail the engine carries, and drops the borrowed names. A spec
	// queued for later (SpawnBoltAfter) is dressed when it is queued.
	void DressFlight(ProjectileSpec& spec) const;
	void Launch(ProjectileSpec spec);
	// Each lit flight and each flash it leaves, as a light (UpdateLights).
	void AppendFlightLights(float time);
	// LIT TORCHES ON THE FLOOR (Michael, 2026-10-03: they stay lit, thrown or
	// set down): each burns its charge where it lies and becomes its stub when
	// spent. An authored record's torch becomes a drop the first time it
	// burns, so the save carries it whole (kind and charge).
	void TickFloorTorches(float dt);
	// One light per lit floor torch, at its burning end.
	void AppendFloorTorchLights(float time);
	// THE SOWILO LIGHTS (DungeonWorld_SpellLight.cpp): one per member's `light`
	// effect per school, from `spell_<school>`, sized by the cast's power.
	void AppendSpellLights(float time);
	const fx::LightEffect* SpellLightKind() const;
	// How much bigger than its profile a light of `power` is (the effect kind's
	// `scale_power` is size 1).
	float SpellLightScale(float power) const;
	// A flare's dazzle: the monster does nothing while it lasts.
	static bool IsDazzled(const Monster& monster);
	// What each school's light DOES beyond its colour, ticked every frame
	// (DungeonWorld_SpellLight.cpp). Fire: KINDLES unlit fires within a step of
	// the party, SCORCHES monsters beside it every `scorch_every` seconds.
	void TickSpellLights(float dt);
	// Fire's scorch on one monster: a small fire burst credited to `source`.
	void ScorchMonster(Monster& monster, float damage, int source);
	// (KindleNear, which lights the fires a light reaches, sits with Reach below.)
	// WATER (6d): the strongest Tidelight on the party (its power; 0 = none),
	// the factor stamina regenerates at in it, the clear bubble it cuts in the
	// haze (a negative dust puff, Render), and putting the party's fires out.
	float WaterLightPower() const;
	float StaminaSoothe() const;
	void AddClearBubble(gfx::Atmosphere& atmo) const;
	int QuenchParty();
	// AIR (6e): a shock at the nearest monster within `reachSquares` and the
	// party's sight (false = none there), and the flicker clock that runs fast
	// while the party is noticed (the WARNING), eased so it never jumps.
	bool CrackleNearest(float reachSquares, float damage, int source);
	void CrackleMonster(Monster& monster, float damage, int source);
	std::array<float, 4> m_crackleClock{};
	float m_airPulseClock = 0.0f;
	float m_airPulseRate = 1.0f;
	// Per-member scorch clocks, and the kindling check's (every quarter second).
	std::array<float, 4> m_scorchClock{};
	float m_kindleClock = 0.0f;
	// EARTH (6f): the stones set down on THIS level - a fixed pool, so a cast in
	// a guarded frame allocates nothing. timeLeft <= 0 is a free slot. Captured
	// into the level's LevelState when it is left or saved (SnapshotActive) and
	// put back when it is entered (ApplyActiveSnapshot); a level left behind is
	// not simulated, so its stones wait for the party with the rest of it.
	struct LightStone {
		int x = 0, z = 0;
		float power = 0.0f;
		float timeLeft = 0.0f;
		float duration = 0.0f;
		float moteClock = 0.0f;  // the next mote off it (not saved)
		float trackClock = 0.0f; // the next showing of the tracks round it (not saved)
	};
	static constexpr size_t kLightStones = 8;
	std::array<LightStone, kLightStones> m_lightStones{};
	// What a stone draws as (the light kind's `stone_item`, resolved once in
	// PreloadItemKinds; null = the light alone).
	const ItemKind* m_stoneKind = nullptr;
	// Counts the stones down and lets the odd mote rise off each.
	void TickLightStones(float dt);
	// One light per stone, from `spell_earth`, dimming over its last tenth.
	void AppendStoneLights(float time);
	// The stones themselves, as `stone_item`'s model glowing (Render).
	void DrawLightStones(ID3D12GraphicsCommandList* list, const ViewCull* cull);
	// How far, in squares, an Earth light of `power` reaches, and that reach in
	// whole walking steps (cut at kStoneSteps).
	float StoneReach(float power) const;
	int StoneSteps(float power) const;
	// WHAT A LIGHT SPELL REACHES from (x, z): every square within `steps` WALKING
	// steps, a step being into an OPEN square (OpenSquare: no rock, no shut door -
	// the test that stops a bolt and a blast), so neither a wall nor a closed door
	// lets it by. Walked in a fixed window round (x, z), so it allocates nothing
	// (`steps` is cut at kStoneSteps). THE ONE STATEMENT of a light's reach: an
	// Earth stone's (what it maps, the tracks it shows) and a flare's (what it
	// dazzles, what it kindles). The flare used the light budget's reach map,
	// which walks floor and so passed shut doors, and kindled by Manhattan
	// distance, through rock (code-review C17).
	static constexpr int kStoneSteps = 8;
	static constexpr int kStoneWindow = 2 * kStoneSteps + 1;
	struct Reach {
		int x = 0, z = 0; // walked from
		int count = 0;    // cells[0, count) were reached, nearest first
		std::array<int, kStoneWindow * kStoneWindow> cells{}; // map cell indices
		std::array<u8, kStoneWindow * kStoneWindow> steps{};  // per window square, 0xFF = not
		// Walking steps from (x, z) to (cx, cz), or -1 when it was not reached.
		int StepsTo(int cx, int cz) const;
	};
	void WalkReach(int x, int z, int steps, Reach& out) const;
	// FIRE: lights every unlit fire whose square `reach` reached (a brazier only
	// at `power` >= the light kind's kindle_brazier_power) - so never one through
	// rock or behind a shut door.
	int KindleNear(const Reach& reach, float power);
	// MAPS what it shows: those squares marked seen, with the walls round them.
	int MapStoneReach(int x, int z, float power);

	// MONSTER TRACKS (lighting-updates 6g): every monster step marks the square it
	// steps onto with the world's track clock, the way it was going and who made
	// it, fading over balance.cat `track_life`. One cell per square, sized with
	// the fog mask (FitTracksToMap), so writing one never allocates. Saved per
	// level as AGES (a `tracks` line), so a load restores how old each was; a
	// level left behind keeps its tracks as they were, like its stones.
	// FOR LATER (Michael): the PARTY leaving tracks, scent and noise that some
	// monsters can follow - `maker` is there so the party can write here too.
	enum class TrackMaker : u8 { None, Monster, Party };
	struct Track {
		double stamp = 0.0; // m_trackClock when it was made
		Direction dir = Direction::North;
		TrackMaker maker = TrackMaker::None;
	};
	std::vector<Track> m_tracks;
	double m_trackClock = 0.0; // simulated seconds, advanced in Update
	u32 m_trackSeed = 0x9E3779B9u;
	void FitTracksToMap();
	void RecordTrack(int x, int z, Direction dir, TrackMaker maker);
	// Seconds since the track was made; < 0 for none, or one already faded.
	float TrackAge(const Track& t) const;
	// Shows the tracks within an Earth light's reach from (x, z): a faint amber
	// mote on each, drifting the way its maker went, fewer and dimmer as the
	// track ages. `strength` 0..1 scales how many (a stone's dimming).
	void ShowTracks(int x, int z, float power, float strength);
	// --- a floor item's pose (code-review C180) ---------------------------------
	// Where a floor item lies and how it is laid there: the ONE answer the draw,
	// a rune's or an enchanted blade's floor glow, a lit torch's light and flame,
	// and the click pick all take, so none of them can drift from the others (the
	// glow used to stand at the foot of the wall for a rune in a SHUT niche, and
	// the flame's head ignored `upright`). `spot` is the rest point - the centre
	// of its quarter, or the pocket of the wall niche it sits in - `world` the
	// floor draw's matrix (the kind's floorLay moved there) and `midY` the height
	// of its drawn middle.
	struct ItemPose {
		Vec3 spot{};
		Mat4 world = Mat4Identity();
		float midY = 0.0f;
	};
	// False when the item is not on show - lifted, or in a shut niche - and then
	// nothing of it is drawn, lit, flamed or picked.
	bool FloorItemPose(const Item& item, ItemPose& out) const;
	// The rest point alone, shown or not: an item just lifted still needs its
	// spot for the shadow cache's note (NoteItemCaster).
	Vec3 FloorItemSpot(const Item& item) const;
	// Fills a freshly built kind's floorLay / floorHeight (ItemKindFor calls it
	// once the model, the tablet's bounds and the scale are known).
	void LayOnFloor(ItemKind& kind) const;
	// Where a lit floor item's flame burns: its model's head, as its pose lays it.
	Vec3 FloorTorchHead(const ItemKind& kind, const ItemPose& pose) const;
	// THE TORCH FLAMES: a lit torch on the floor or in flight burns with a
	// small fixture-style flame (FireEffect), from one fixed pool - reserved
	// at construction, so a torch catching or landing allocates nothing. In
	// flight the flame's particles keep their own course as the emitter moves
	// on, so a thrown torch trails its own fire. Past the pool, the furthest
	// go without a flame (they still light).
	static constexpr size_t kTorchFlames = 8;
	static constexpr float kTorchFlameScale = 0.3f;
	struct TorchFlame {
		u32 key = 0; // LightKey of what it burns on; 0 = free
		bool seen = false;
		FireEffect effect;
	};
	std::array<TorchFlame, kTorchFlames> m_torchFlames;
	// Lights, moves and ages the pool's flames for this frame's torches.
	void UpdateTorchFlames(float dt);
	// Burns every lit torch held (hands, cursor) by `dt`, and puts out any
	// stowed in a pack.
	void TickCarriedLight(float dt);
	// One light per lit torch held, at its member's side of the eye.
	void AppendCarriedLights(float time);
	// Burns one slot's torch; true if it burnt out (and became its stub).
	bool BurnTorch(ItemSlot& slot, float dt, const Character* holder);
	// 1 = a torch at full light, falling to a floor over its last tenth.
	static float TorchBrightness(const ItemKind& kind, float charge);
	// A torch, lit or not, whose lit kind has a `power_level`: no spell lights
	// it (Flame passes it over), only its own Light command.
	bool MagicalTorch(const ItemKind& kind);
	WorldState* m_flagStore = nullptr; // see SetFlagStore
	std::optional<WorldMap>* m_worldForUndo = nullptr; // borrowed; see SetWorldForUndo
	// The project's opening, borrowed (SetOpeningForUndo), and whether a move
	// has changed it since the owner last saved.
	std::string* m_openLevel = nullptr;
	int* m_openX = nullptr;
	int* m_openZ = nullptr;
	bool m_openingMoved = false;
	// MoveObject's stair half, and the ways in that follow a moved stair.
	bool MoveStair(const MoveTarget& t, int tx, int tz);
	void MoveArrivals(int fx, int fz, int tx, int tz);
	// Every way IN that lands on an explicit square of `stem` - a world-map
	// doorway, the project's opening - handed to `remap`, which moves it and
	// returns true when it did. Shared by a stair move and a level resize.
	void RemapArrivals(const std::string& stem, const std::function<bool(int&, int&)>& remap);
	// The active level's .ent text (live monsters + records); see SaveLevel.
	std::string ActiveEntText() const;
	// ...and its .map text (live decorations synced back into records).
	std::string ActiveMapText() const;
	// A STASHED level's .map / .ent text, as WriteStashedLevel writes them.
	static std::string StashedMapText(const std::string& stem, const DungeonMap& map);
	static std::string StashedEntText(const std::string& stem, const DungeonEntities& ents);
	std::mt19937 m_combatRng{0xC0FFEEu};
	// COSMETIC choices - which of a state's clips a monster plays (PickClip) -
	// draw from their own stream (code-review C73), so authoring one more attack
	// or hit clip cannot shift every later combat roll of a seeded sweep. Back to
	// kCosmeticSeed whenever a level's monsters are built (LoadMonsters), so a
	// recycled world picks the same clips a fresh load does - the `monsters`
	// readout prints the rising clip, and `reset` must equal a new game line for
	// line.
	static constexpr u32 kCosmeticSeed = 0xA11CE5u;
	std::mt19937 m_cosmeticRng{kCosmeticSeed};
	bool m_partyWiped = false; // latches onPartyWipe so it fires once
	bool m_partyNoticed = false; // PartyNoticed: set by UpdateMonsters, cleared by TickParty
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
	// The damage-type vocabulary (damagetypes.cat). Built FIRST of the three
	// registries, because Balance resolves its attack types against it and
	// every effect kind resolves the type it deals — and it must outlive both,
	// since a DamageType anywhere in the world is an index into it.
	DamageTypeBook m_damageTypes;
	// The type a COLLISION deals (a wall, a door, a pit landing). Resolved once
	// at load: the world's two blows are not attacks and have no verb to ask,
	// so this is the one place the engine still needs a type by name.
	DamageType m_bashType{};
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
	// DefenseWith's swap partner: holds the hovered item id while it sits in
	// the member's slot. Pre-sized so assigning an id never grows it.
	std::string m_defenseScratch = [] {
		std::string s;
		s.reserve(64);
		return s;
	}();
	// Last plan-batch sequence applied per bucket, so we adopt a batch only once.
	uint64_t m_lastPlanSeq[ai::Scheduler::kBucketCount] = {};
	// Per-bucket SIM-time accumulator for lockstep (TickLockstepAI). Unused
	// while the workers run themselves; reset when lockstep is switched on, so
	// enabling it does not immediately fire every bucket with a debt of
	// whatever wall-clock time happened to have passed.
	float m_bucketClock[ai::Scheduler::kBucketCount] = {};
	// THE FIXED TICKS (see kTick): world time owed but short of a tick, carried
	// to the next frame's ticks; zero whenever a frame is a whole step.
	float m_tickCarry = 0.0f;
	UpdateRun m_lastUpdate;
	Clock m_clock; // every Update, and what it simulated (WorldClock)
	// One tick of simulation - everything Update advances by dt except what
	// the frame shows (PresentFrame). Update calls it once, or once a tick.
	void Tick(float dt);
	// Runs `dt` as one step or as fixed ticks (see kTick); returns the world
	// seconds actually simulated.
	float AdvanceSimulation(float dt);
	// What the frame shows of the `dt` just simulated: camera, lights, and the
	// fire / plume / flight particles.
	void PresentFrame(float dt, float time);
	// EVERY eval-harness field the world holds, in one member (see `Harness`).
	// Four bools-and-counters that used to sit loose among the world's own state
	// reading like something nobody had got round to explaining.
	Harness m_harness;
	// REST. Transient by design — not saved, so a save made mid-rest loads
	// standing up, and a load or a new game ENDS one (ResetForNewGame, quietly;
	// C295). `m_restLockstep` remembers the AI mode rest replaced, because
	// the eval harness may already have lockstep on and rest must give it back
	// rather than assume it was off.
	bool m_resting = false;
	bool m_restLockstep = false;
	const char* m_restEndReason = ""; // a literal; see RestEndReason
	// For each standing member, swing any hand whose cooldown has run out.
	// Called from UpdateMonsters' cadence, no-op unless m_harness.autoAttack.
	void TickAutoAttack();
	// Fire the next Harness::autoCast entry when its clock runs out. Same
	// cadence as TickAutoAttack; no-op while the rotation is empty.
	void TickAutoCast(float dt);
	// Walkability grid shared into snapshots, rebuilt only when the map changes.
	std::shared_ptr<const std::vector<uint8_t>> m_walkableCache;
	u32 m_walkableRev = 0xFFFFFFFFu; // map Revision() the cache was built for
	// The grids the cache is drawn from - the snapshot pool's trick for the grid.
	// A map change in PLAY (a fixture broken and doused bumps the revision) used
	// to make_shared a fresh grid inside a guarded frame, because the old one may
	// still be in a worker's hands. Now a grid nobody can be reading (AIGridFree)
	// is refilled in place, from a pool filled at level load (ReserveAIPools).
	std::vector<std::shared_ptr<std::vector<uint8_t>>> m_walkablePool;
	// Snapshot pool so steady-state frames allocate nothing (CLAUDE.md memory
	// strategy): BuildAISnapshot reuses a snapshot nobody can be reading
	// (AISnapshotFree), zero-filling its flat grids and clear()ing its vectors in
	// place (capacity retained) instead of make_shared.
	std::vector<std::shared_ptr<ai::Snapshot>> m_snapshotPool;
	// The snapshot BuildAISnapshot published last: in use by definition, whatever
	// its mark says, until the next publish replaces it. Only compared, never
	// dereferenced (it may have left the pool; the director keeps it alive).
	const ai::Snapshot* m_publishedSnapshot = nullptr;
	// HOW DEEP both pools are filled (code-review C66): as many buffers as can be
	// in use at once - the snapshot on show and one per worker still reading an
	// older one - plus the one being built. The grids obey the same bound, since a
	// worker reads a grid only through a snapshot it holds.
	static constexpr size_t kAIPoolDepth = ai::Scheduler::kBucketCount + 2;
	// Room in each snapshot's monster list beyond the level's own monsters, for
	// ones added in play (a spawn, an editor placement), before the list grows.
	static constexpr size_t kAIAgentHeadroom = 32;
	// Room in each of the inline compute's plan batches beyond a bucket's own
	// monsters. Small, because each slot carries a whole map's worth of path,
	// and because adding a monster sizes for it at once (AddMonster).
	static constexpr size_t kAIPlanHeadroom = 2;
	// Whether each pool (0 snapshots, 1 grids) has warned of growing; once each.
	bool m_aiPoolWarned[2] = {};
	// Fills both pools for the current map and monsters, and sizes the inline
	// compute - lockstep's, which rest runs in guarded frames: its brain's BFS
	// scratch and its plan batches (code-review C62, C66). At level load,
	// wherever the map's size or the monster list is replaced, and when a
	// monster is added; idempotent.
	void ReserveAIPools();
	// Whether a pooled snapshot / walkability grid can be reused: nobody is (or
	// can start) reading it. See ai::HandBack for why this is not use_count().
	bool AISnapshotFree(const ai::Snapshot& s) const;
	bool AIGridFree(const std::vector<uint8_t>* grid) const;
	// The once-per-pool warning that a pool grew in play: a fill that was too
	// shallow, or a mark never given back. AllocTest fails a run that logs it.
	void WarnAIPoolGrew(int pool, const char* what, size_t now);
	// AssignFormation's aware-attacker index list — member scratch so the
	// every-frame formation pass doesn't heap-allocate (cleared, not freed), and
	// reserved for every monster at spawn (MakeMonster) so a fight's first aware
	// monster does not grow it either.
	std::vector<int> m_formationScratch;

	// Build the immutable snapshot the AI workers read, and hand it over. Cheap:
	// reuses the cached walkability grid unless the map's revision changed.
	void BuildAISnapshot();
	// Adopt the freshest plan batches into each monster's intent + cached path.
	void ConsumeAIPlans();
	// LOCKSTEP AI (docs/eval-harness.md): with the bucket workers paused, run
	// each bucket's compute inline whenever its cadence has elapsed in SIM time.
	// `dt` is the world dt already scaled by timescale, so a run at timescale 20
	// — or one stepping whole seconds per frame — thinks exactly as often per
	// simulated second as a run at 1 does. That equivalence IS the feature.
	void TickLockstepAI(float dt);
	// Live monster with this stable runtimeId, or null if none (died/erased/level
	// changed). Linear scan — fine at this scale; swap for a map if counts explode.
	Monster* MonsterByRuntimeId(u32 id);

	// The prop kinds, ONE CACHE PER CATALOG (code-review C302: one cache by bare
	// id served all four, so a door and a decoration of one id shared a kind).
	using PropKinds = std::flat_map<std::string, std::unique_ptr<DecorationKind>, std::less<>>;
	std::array<PropKinds, static_cast<size_t>(PropCatalog::Count)> m_decorationKinds;
	PropKinds& KindsOf(PropCatalog catalog) {
		return m_decorationKinds[static_cast<size_t>(catalog)];
	}
	const PropKinds& KindsOf(PropCatalog catalog) const {
		return m_decorationKinds[static_cast<size_t>(catalog)];
	}
	// unique_ptr so DecorationKind::tex stays valid as more sets are added
	// (flat_map stores values contiguously and reallocates on insert).
	std::flat_map<std::string, std::unique_ptr<PropTextures>> m_propTextures;
	// The model cache's store, by file name (see ModelFile). Each part is built
	// on first ask, so a file only ever drawn as a multi-material model never
	// uploads a single-mesh copy it would not use, and vice versa.
	struct CachedModel {
		// NOT const here, though every kind holds it as const: the cache alone
		// may drop the file's CPU images (assets::ReleaseImages) once they are
		// uploaded, which no holder reads (code-review C222).
		std::shared_ptr<assets::ModelData> data;
		std::shared_ptr<gfx::Mesh> mesh;                 // meshes[0]
		std::shared_ptr<const MultiMaterialModel> multi; // the template ModelMulti copies
		// The images are gone from `data` (uploaded into `multi`, or swept at
		// the end of a load); a multi built after that must read the file again.
		bool imagesReleased = false;
	};
	std::unordered_map<std::string, CachedModel> m_modelCache;
	std::vector<Decoration> m_decorations;
	std::optional<LevelTransition> m_pendingTransition; // raised by a stair step
	// A pit fall in flight: the transition latched when the party stepped onto
	// a `fall` link (m_falling says whether one is). The step glide finishes
	// first (m_fallT < 0 = still waiting), then the camera drops through the
	// hole (PartyEye) and the stashed transition is raised. See Tick's fall block.
	//
	// A KEPT member, ASSIGNED into, never constructed (code-review C210): the
	// step that latches it and the whole plunge are Playing frames the
	// allocation guard arms, and a fresh LevelTransition built its level string
	// there. LoadStairs reserves the string room for any destination the level
	// can name (ReserveFallRoom), and nothing ever shrinks it.
	LevelTransition m_fall;
	bool m_falling = false;
	float m_fallT = -1.0f;
	// m_fall's level is reserved to the longest level name the project and the
	// live map's stairs hold, and never less than this - room past any stem the
	// editor can give a level made or renamed after the load (its rename field
	// takes 24 characters).
	static constexpr size_t kFallLevelRoom = 64;
	void ReserveFallRoom();
	// The plunge's IMPACT, owed on the far side of the swap: the host clears
	// the message log as it places the party on the new level, so the bruise is
	// charged on the first frame after they arrive rather than before they
	// leave (OnFallImpact — otherwise its line would never be read).
	bool m_fellPending = false;
	// THE LEVEL'S TRANSIENTS (code-review C292, C293): what is IN FLIGHT or UNDER
	// WAY in the level and belongs to no record - shots and thrown things, a volley
	// still queued, a blast still spreading (a poison gas hangs 8 s), an Earth
	// light set down, the fixture damage table, the effects riding the monsters,
	// a stair or a pit fall under way. ONE list, called wherever a level is
	// replaced or put back: BeginLevelLoad (after the level left is stashed),
	// InstallLevel, the harness's arena, and ResetForNewGame - a new game or a
	// load, which on another level is a BeginLevelLoad as well. Each of those used
	// to clear its own copy of this list, and the copies had drifted: a gas thrown
	// before a stair bit the same squares of the next floor, a load or a new game
	// kept it and every burning monster, and the fixture table - which a re-seed
	// carries over BY CELL - handed a smashed sconce's wreck to whichever sconce
	// stood on that square of the next level. So the table is EMPTIED here, and
	// whoever puts the level back seeds it afresh.
	// NOT m_fellPending: a pit fall's bruise is owed on the far side of the very
	// level load that ends the fall (ResetForNewGame clears it - a fresh start
	// owes nothing). Lists are emptied with clear(), keeping what was reserved.
	void ClearLevelTransients();
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
	// re-parse. Stashed on leave when EDITED (StashEditedLayers), consumed on
	// entry (BeginLevelLoad), CREATED ON DEMAND by remote-level editing
	// (EditMapStash / EnsureMapStash - the map overlay can edit any level, not
	// just the active one) and ONLY by an edit: `savemap` (SaveAllLevels) writes
	// every stashed level back to its files, so a stash made to READ a level is
	// a file rewritten for nothing (code-review C307, "never stash to read" -
	// LevelForReading is the way to look). unique_ptr so references survive
	// sibling insertions.
	std::flat_map<std::string, std::unique_ptr<DungeonMap>> m_levelMaps;
	// The .ent-record twin of m_levelMaps: baseline records of inactive levels
	// whose RECORDS were edited (remote placements/erases, or active-level
	// prunes carried out by structural paints). Record ids are stable across
	// removals, so m_levelStates' per-id dynamic diffs stay valid against a
	// stashed baseline. Only edited levels get an entry (m_entsDirty tracks the
	// active level) - an untouched .ent file is never rewritten. The two stashes
	// are INDEPENDENT: an item erased stashes the records and not the map, a
	// wall painted the map and not the records, and each is written on its own.
	std::flat_map<std::string, std::unique_ptr<DungeonEntities>> m_levelEnts;
	// READ-ONLY copies of level files, for the checker (Validate) alone. NOT an
	// edit stash - nothing here is ever written back, which is the whole point:
	// see Validate. Re-parsed when either file's write time moves (a savemap, a
	// rename), so a copy never answers for a file that has since changed.
	struct ReadOnlyLevel {
		std::unique_ptr<DungeonMap> map;
		std::unique_ptr<DungeonEntities> ents;
		long long mapTime = -1, entTime = -1; // file write times when parsed
	};
	std::flat_map<std::string, ReadOnlyLevel> m_readOnlyLevels;
	// The read-only copy of `stem`, parsed or refreshed as needed. The returned
	// reference is only good until the next call (flat_map storage moves); the
	// map/ents it points AT are heap-owned and stay put.
	const ReadOnlyLevel& ReadOnlyLevelOf(const std::string& stem);
	// A level as it IS now, for READING (code-review C307): the live map and
	// records for the active level, else each layer's edit stash, else its file
	// read-only (ReadOnlyLevelOf). NEVER creates a stash. The one copy of the
	// lookup the checker, the census, the generator's palette donor, the stair
	// walks and every remote edit's validation share. The pointers stay good
	// until that level's stash is made or dropped, or its files change and are
	// read again.
	struct LevelRead {
		const DungeonMap* map = nullptr;
		const DungeonEntities* ents = nullptr;
	};
	LevelRead LevelForReading(const std::string& stem);
	// An edit of a level that is NOT the active one, landing in its stash only
	// when it CHANGES something (C307). With a stash already there it is simply
	// applied; with none, it is tried on a COPY of the level as it reads, and
	// that copy becomes the stash only if `edit` reports a change - so a refused
	// or no-op edit leaves no stash, and savemap rewrites nothing for it.
	// `edit(DungeonMap&) -> bool` / `edit(DungeonEntities&, const DungeonMap&)
	// -> bool` (the map the records validate against: the level's as it reads).
	template <class Edit> bool EditMapStash(const std::string& stem, Edit&& edit) {
		if (const auto it = m_levelMaps.find(stem); it != m_levelMaps.end())
			return edit(*it->second);
		auto probe = std::make_unique<DungeonMap>(*LevelForReading(stem).map);
		if (!edit(*probe)) return false;
		m_levelMaps.insert_or_assign(stem, std::move(probe));
		return true;
	}
	template <class Edit> bool EditEntStash(const std::string& stem, Edit&& edit) {
		const LevelRead read = LevelForReading(stem);
		if (const auto it = m_levelEnts.find(stem); it != m_levelEnts.end())
			return edit(*it->second, *read.map);
		auto probe = std::make_unique<DungeonEntities>(*read.ents);
		if (!edit(*probe, *read.map)) return false;
		m_levelEnts.insert_or_assign(stem, std::move(probe));
		return true;
	}
	// The active level's m_entities records diverged from the .ent file on disk
	// (a prune/re-face edited them); stash them on leave so the divergence
	// survives the swap and savemap writes it.
	bool m_entsDirty = false;
	// THE ACTIVE LEVEL'S MAP AS ITS FILE HOLDS IT (code-review C308): the
	// StaticLayerText of the map when it was read from its file, or when a save
	// last wrote it. ActiveMapEdited compares the map as it stands against this,
	// and a level is stashed on the way out only when they differ - so a level
	// merely visited is not stashed, and savemap does not rewrite it (which, for
	// a hand-written file, also lost its comments). MEASURED rather than a latch
	// every edit sets, as m_entsDirty is: an edit path that forgot the latch
	// would have its edits dropped at the next stair (C298's loss again), where
	// a comparison cannot miss one - and an undo back to the file reads clean.
	// Empty = not known to match a file (a level that came back from a stash, an
	// ambush, a file in an old form the next save must rewrite): edited, and
	// stashed on the way out, as every level used to be.
	std::string m_mapAsFiled;
	// A map's static layer as the stash writer writes it, under no stem (a
	// renamed level is not an edited one) and with PLAY's fires and niches put
	// back as authored: what play did to them is the level's dynamic state.
	static std::string StaticLayerText(DungeonMap map);
	// m_mapAsFiled for a map JUST READ from its file: its StaticLayerText - or
	// empty when the file is in an old form (DungeonMap::ReadInOldForm), which
	// a save must rewrite whether or not anything was edited.
	static std::string AsFiledText(const DungeonMap& justRead);
	// The active map with the live decoration placements synced back into its
	// records (AddDecoration only appends a live instance) - what a stash holds.
	DungeonMap ActiveStaticCopy() const;
	// The active level is PARKED (ParkActive): stashed, with the party outside it.
	bool m_parked = false;
	ParkedPose m_lastPark; // LastPark()
	// The live level is the authority again, IN PLACE - a save loaded or a game
	// begun on the level that was parked, a regenerate of it. What the park
	// stashed of its editor layers was a COPY of these live ones, so it is
	// dropped: left under the live level's own stem it outlived them - savemap
	// writes every stash after the live level (the copy over what had just been
	// written), and an edit undone in the meantime came back on the next visit.
	void Unpark();
	// Copies the active map into m_levelMaps, first syncing the live decoration
	// placements back into its records (AddDecoration only appends a live
	// instance; LoadDecorations rebuilds from records on return).
	void StashStaticMap();
	// Stashes the active level's EDITOR layers that differ from its files - the
	// map when ActiveMapEdited, the records when m_entsDirty - on every way out
	// of it (a stair, a park, a save loaded or a game begun elsewhere; C298,
	// C308), and DROPS a stash already held under its stem for a layer that
	// does not: what is held for a level left is exactly what differs from its
	// files. A level that is not the project's (an ambush's ground, which has no
	// file and is thrown away, never returned to) stashes nothing.
	void StashEditedLayers();
	// The stash for `stem`, parsing the level's files on first use - for an edit
	// that is about to change it (a READ goes through LevelForReading, and an
	// edit that may change nothing through EditMapStash / EditEntStash). Each
	// creates its own layer and no other: the records validate against the
	// level's map as it reads (soft: stale records skip with a warning). Never
	// call for the ACTIVE level (its truth is m_map/m_entities).
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
	// another solid wall of their cell (or drops them), by DungeonMap::SolidWall.
	// (Wall-mounted DECORATION records are the map's own: EditCellRemote re-hangs
	// them through DungeonMap::RehomeWallDecorations before this runs. There is
	// no soft loader for one left facing open floor - the parser asserts, and a
	// level saved that way could never load again; code-review C344.)
	void PruneStashRecordsForCell(const std::string& stem, int x, int z);
	// Serializes a stashed level back to its files: the .map when its map is
	// stashed, the .ent when its records are - each layer on its own, an
	// untouched one left byte for byte. False when neither is stashed or a write
	// failed. The static writer is shared with SaveLevel.
	bool WriteStashedLevel(const std::string& stem) const;

	// --- editor undo/redo internals (see the public section) ------------------
	// Live decoration placements as .map records (the SaveLevel writer's emit in
	// record form). Shared by StashStaticMap and the undo capture.
	std::vector<Entity> LiveDecorationRecords() const;
	// One live monster as its .ent record - its SPAWN square and facing, and the
	// per-instance overrides the inspector edits live - carrying its own id (-1
	// for one the editor placed). The one statement of the live-to-record
	// mapping: ActiveEntText writes it and SyncActiveRecords keeps the records in
	// step with it. The spawn, never where it stands: the editor is a live view,
	// and a save took each patrol's, chase's and corpse's current square for its
	// authored one (code-review C326).
	Entity LiveMonsterRecord(const Monster& m) const;
	// An editor removal of one live monster: its .ent record goes too (one left
	// behind came back with the next undo or respawn: C311), and the damage
	// ledger rebases, the monsters after it having slid down a slot (C355).
	void EraseMonster(std::vector<Monster>::iterator it);
	// A full editor-state snapshot: the active level (map with decoration
	// records synced, .ent records, dynamic-state diffs) + every stash. Levels
	// are a few KB, so a copy per edit is nothing.
	struct EditorSnapshot {
		// The WORLD as it was, when the editor is holding one. Copied per step
		// like everything else here: a world is a small grid and a few dozen
		// records, which is less than any one level.
		std::optional<WorldMap> world;
		std::string stem;            // active level at capture
		DungeonMap map;              // live decoration records synced in
		DungeonEntities ents;
		bool entsDirty = false;
		SaveData::LevelState state;  // live dynamic diffs (SnapshotActive)
		std::flat_map<std::string, std::unique_ptr<DungeonMap>> stashMaps;
		std::flat_map<std::string, std::unique_ptr<DungeonEntities>> stashEnts;
		// The project's opening square (SetOpeningForUndo), which a stair move
		// can carry along. -2 = not captured (no opening borrowed).
		int openX = -2, openZ = -2;
		// Where the PARTY stood, set only by a step that renumbers the squares (a
		// level resize): undoing one must put the party back where it was, since
		// its old coordinates name a different square now. -1 = not recorded, and
		// then a restore leaves the party alone, as it always has.
		int partyX = -1, partyZ = -1;
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
	u64 m_editRevision = 0;                      // see EditRevision
	// The monster powers, derived and resolved per kind of the project, and
	// their range (see MonsterPower). Rebuilt whole when the edit revision
	// moves or InvalidatePowers is called - the palette reads it every frame.
	struct PowerCache {
		struct Kind {
			double derived = 0.0, resolved = 0.0;
		};
		std::unordered_map<std::string, Kind> kinds;
		power::Range range;
		u64 revision = 0;
		bool valid = false;
	};
	mutable PowerCache m_powers;
	const PowerCache& Powers() const;
	struct CensusCache {
		std::vector<LevelCensus> levels;
		u64 revision = 0;
		size_t liveMonsters = 0; // the active level's live list, when counted
		std::string level;       // ...and which level that was
		bool valid = false;
	};
	mutable CensusCache m_census; // see Census
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
		// The light its fire gives (a lights.cat id; fixtures.cat `light`,
		// default fire_sconce on a wall, fire_brazier on the floor).
		std::string light;
		std::shared_ptr<gfx::Mesh> mesh;  // via the model cache
		std::shared_ptr<gfx::Mesh> mesh2;
		// A wall torch whose torch can be TAKEN (spell-updates): the bare bracket
		// it shows once taken (fixtures.cat `empty_model`, same placement as
		// `model`), and the item a taken torch becomes (`torch_item`, its lit
		// form used while the sconce burns). No empty_model = the torch stays put.
		std::shared_ptr<gfx::Mesh> meshEmpty;
		std::string torchItem;
		// Kept for the map-icon bake's bounds fit (shared via the model cache).
		std::shared_ptr<const assets::ModelData> model;
		Vec4 color{1, 1, 1, 1};
		Vec4 color2{1, 1, 1, 1};
		const PropTextures* tex = nullptr;
		const PropTextures* tex2 = nullptr;
		float modelScale = 1.0f; // fixtures.cat `scale`, like DecorationKind's
		FixtureFlame flame{0.0f, 0.0f, 0.0f};
		std::unique_ptr<gfx::Texture> iconTarget; // baked map icon
		// Breakability, off unless the type opts in (fixtures.cat `breakable`).
		bool breakable = false;
		float hp = 0.0f;
		float soak = 0.0f;
		ResistTable resists;
		// What the fire takes on when it goes out, by any cause (fixtures.cat
		// `on_douse`, the on_hit form): `smoke <power> <seconds>` is the haze its
		// smoke leaves hanging over the square (SetFireBurning).
		std::vector<fx::Proc> onDouse;
	};

	std::flat_map<std::string, std::unique_ptr<FixtureKind>> m_fixtureKinds;
	bool m_fixtureIconsBaked = false; // re-armed by a fresh kind (like decorations)
	FixtureKind& FixtureKindFor(const std::string& type);
	std::unique_ptr<gfx::ParticleBatch> m_particleBatch;
	// The per-frame billboard buffer: cleared and refilled every Update, so it
	// keeps its capacity and only allocates on a frame that beats every previous
	// frame's particle count. That high-water growth lands INSIDE a steady-state
	// frame (the fires are still settling when the guard's warm-up expires), so
	// the peak is reserved up front instead — see ReserveParticleScratch.
	std::vector<gfx::ParticleInstance> m_particleScratch;
	void ReserveParticleScratch();

	// The party leader's roster index (see Leader()), and the pass that hands
	// the lead on from a member who is no longer standing - every frame from
	// Update (cheap: one health check), and on load without a line.
	int m_leader = 0;
	void PassLeadIfDown(bool announce);

	// Dev console toggles (see the hooks above).
	float m_fovDegrees = 70.0f;
	bool m_shadowsEnabled = true;
	bool m_dustEnabled = true;
	float m_ambientScale = 1.0f; // multiplies kBaseAmbient (mood-pass knob)

	float m_time = 0.0f; // latest frame time (Update), drives the rune glow pulse
};

} // namespace dungeon::game
