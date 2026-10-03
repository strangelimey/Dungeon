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
	// LOAD rather than from that call, plus the harness's own modes.
	void ResetForEval();
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
	void SetResting(bool on);
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
		// Monster ACTION (movement and attacks) stops while everything that
		// HAPPENS TO them keeps running — animation, effects, blasts, damage. A
		// geometry probe needs its instruments to hold still: monsters parked on
		// known cells to read a blast's falloff otherwise walk off those cells
		// mid-measurement and report where they ended up instead.
		bool frozen = false;
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
		struct AutoCast {
			static constexpr int kMaxEntries = 4;
			struct Entry {
				int member = 0;
				char spell[32] = {};
				u8 len = 0;
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

	// Trains `avoid` on an evaded blow or the worn armor on a blunted one —
	// call once per RESOLVED attack against a member.
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
	void DropAtPartyFeet(std::string_view itemId);
	bool ShoveAhead(int cells);
	ProjectileSystem::Repelled RepelAhead(float power, int casterIndex);
	void BlastAroundParty(const ProjectilePayload& payload, SpellSymbol school,
						  int casterIndex);
	// A puff of `school`'s element just ahead of a cast's origin (its lane at the
	// eye) along `dir` - flame, dust, a breath of air, a splash of water - and a
	// brief shadowless glow.
	void HandPuff(SpellSymbol school, const Vec3& origin, const Vec3& dir);
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
	// onto the floor where the ray meets it, when that square is walkable, in
	// reach and seen - snapped to the quarter slot nearest the hit point.
	// THROW OR DROP (ui-updates Phase 10, Grimrock's screen-height rule): a click
	// that meets no reachable floor - above the floor's horizon, on a wall,
	// beyond reach - drops NOTHING and returns false, and the caller throws.
	bool DropItemAt(const std::string& typeId, float mx, float my, float w, float h,
					float charge = -1.0f); // the item's charge goes down with it
	// THROWING (DungeonWorld_Throw.cpp): a member throws an item (catalog id)
	// straight ahead down their quadrant lane - `member` < 0 = the party LEADER
	// (the cursor's throw), else that roster slot (a hand's `throw` use). False =
	// nobody threw (the thrower is down, or still recovering from their last
	// throw - throw_interval): the item stays where it was.
	bool ThrowItem(const std::string& typeId, int member = -1, float charge = -1.0f);
	// Brings every thrown item still in the air down where it is - before a save
	// (a flight is not saved; the item must be) and a level change.
	void LandThrownItems() { m_projectiles.LandCargo(); }
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
	// PARKS the active level: the party has walked OUT of it (to the world map)
	// while it stays loaded underneath. Its state is stashed NOW, exactly as a
	// stair would stash it, because whatever replaces it next — a doorway, a
	// random encounter, a load — does so without stashing (those paths replace
	// a throwaway baseline, and cannot tell one from a level the party left).
	// Idempotent; any level load or install ends it.
	void ParkActive();
	bool Parked() const { return m_parked; }

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
	void EditCell(int x, int z, Cell cell);

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
	// The loaded albedo behind a surface-palette catalog id, for the editor
	// map's textured cell fill. Only the ACTIVE level's palette sets are ever
	// loaded, so the lookup searches those; null for any other id (a browsed
	// level on a foreign palette) — the map falls back to its flat ink.
	const gfx::Texture* SurfaceAlbedoForId(SurfaceSel sel,
										   const std::string& id) const;
	// A surface type's SWATCH for a list (the palette, a theme's member
	// rows): the loaded albedo when the active level has it, else a small
	// thumbnail LoadSurfaceThumb made earlier, else null (the flat colour).
	// Draw-safe: it never loads.
	const gfx::Texture* SurfaceSwatchForId(SurfaceSel sel, const std::string& id) const;
	// Loads that thumbnail for a type the level has not loaded (once per set,
	// found or not) - the asset picker's loader, trimmed to swatch size. It
	// uploads, which drains the GPU: call from Update, never mid-frame. True
	// when it went to disk (so a caller can pace itself), false when there was
	// nothing to do.
	bool LoadSurfaceThumb(SurfaceSel sel, const std::string& id);

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
	// longer exists. The caller re-spawns live objects afterwards
	// (RespawnFromRecords) and saves (`savemap`) to persist.
	TypeUsage SweepTypeRefs(const std::string& catalogKey, const std::string& id,
							const std::string* newId = nullptr);
	// --- generator support ----------------------------------------------------
	// Could a stair stand on (x,z) of `stem`? Walkable, and nothing already
	// there that a stair cannot share a square with. Public so the generator's
	// seam can find a cell that suits BOTH levels of a link before authoring it.
	// Loads the level on demand, like every other cross-level query here.
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
	// else its stash (parsed on first use, as every cross-level query here
	// does). The generator copies a chosen level's surface palette from it.
	const DungeonMap& MapOf(const std::string& stem) {
		return stem == m_currentLevel ? m_map : EnsureMapStash(stem);
	}

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
	// for CurrentLevel; it is never a path and never written.
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

	// Door instance surface for the editor's inspector. DoorSettings reports the
	// door on (x,z) (false = none); SetDoorSettings applies the edit to the LIVE
	// door (the leaf animates, initialOpen follows — the editor edits the
	// AUTHORED state) and to its .ent record's params (in-memory until savemap,
	// like every editor edit). `name` is what a button's target= points at
	// (ToggleDoorsNamed).
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
	// The erase ladder for a remote cell, mirroring the live tool: stair (pair
	// removed too) → one monster/door/button/item record → one decoration
	// record → fixture → reset the cell's surface variants. Always acts (the
	// last rung is a reset), messaging what it did.
	void EraseRemote(const std::string& stem, int x, int z);

	// Saves every level with unsaved edits: the active one (SaveLevel) plus
	// each stashed level (WriteStashedLevel). Returns the stems written.
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
	// stair dest= across all levels (disk-only ones via a lazy stash, so the
	// fix persists on the next savemap). Drops the undo history (its
	// snapshots are keyed by the old stem). The MANIFEST is the owner's:
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
	// Walks every level NOT in `dying` (the live one, the stashes, and any not
	// yet in memory — parsed on demand, like the type sweep) for a traversable
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
	// A counter that moves whenever the editor changes something - the signal
	// live validation re-runs on. Bumped by every kept undo step, undo/redo, a
	// history clear (level transitions, renames, deletes), and by the edits that
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
	// across a relaunch. Returns false if either file could not be written.
	bool SaveLevel() const;

	// --- dev console hooks ---------------------------------------------------
	// "kind @ x,z" for each live monster.
	std::vector<std::string> MonsterList() const;
	// --- the one-pipeline check (Game/DamageLedger.h) ------------------------
	// Arming, strictness and the counters live on the ledger itself; the console
	// reaches them through here. Anything that REPLACES party or world state
	// wholesale (a load, a save restore, a respawn, a `heal`) must call
	// RebaseDamageLedger afterwards — the values it overwrote no longer exist to
	// be reconciled, and without a fresh baseline the next checkpoint reports the
	// replacement itself as a violation.
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
	// Point lights submitted this frame (after UpdateLights).
	size_t ActiveLightCount() const { return m_lights.points.size(); }
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
		};
		std::vector<Sub> subs; // one per model.meshes
		Vec3 boundsMin{}, boundsMax{}; // world-space AABB of the baked geometry
		// The model's LONG axis (0 x, 1 y, 2 z - the AABB's biggest extent) and
		// which way along it the "handle" end lies (+1 / -1): the half holding
		// the widest cross-section, which on a blade is the guard's side. The item
		// details dialog stands a weapon on this axis, handle up.
		int longAxis = 1;
		float handleSign = 1.0f;
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
		// level); the colour is what its flame and its light are, instead of the
		// party's torchlight setting. flameTinted=false = an ordinary flame.
		float powerLevel = 0.0f;
		Vec3 flameColor{1.0f, 0.62f, 0.28f};
		bool flameTinted = false;
		// A container one fill level up (items.cat `fill_as`): what a Splash, or
		// any later filling, makes of it. Empty = it takes no water.
		std::string fillAs;
		bool Lit() const { return burnTime > 0.0f; }
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
		bool isRune = false;
		// items.cat `upright`: it STANDS on the floor as authored (a bottle) and
		// its icon stands too, instead of being laid along its length.
		bool upright = false;
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
		// item snaps to the quarter nearest the cursor; up to 4 share a cell. Render
		// + pick + the glow light use SlotCenter(x,z,Medium,slot). See SlotGrid.h.
		int slot = 0;
		// The wall NICHE this item sits in (Direction index; -1 = an ordinary floor
		// item). Niche items pile at the pocket centre (NicheItemPos), ignore `slot`,
		// and are hidden + unpickable while the niche is closed.
		int niche = -1;
		// The item's own charge (ItemSlot::charge - a torch's seconds left),
		// kept while it lies here and handed back when it is lifted. LAST, so
		// the positional inits above need not name it.
		float charge = -1.0f;
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
	struct PropTextures {
		std::unique_ptr<gfx::Texture> albedo, normal, mr;
		float heightScale = 0.0f;
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
	};
	// Loads a PBR set by base name at the current quality tier, falling back to
	// the always-present 2k set. `required` (surfaces) dies if even the albedo is
	// missing; otherwise (props) returns maps with a null albedo so the caller
	// keeps its flat material. The single source of the res→2k fallback.
	PbrMaps LoadPbrSet(const std::string& name, bool required);

	void LoadDungeonBlocks();      // loads the worn block set for the quality tier
	void LoadFeatureMeshes();      // wall/surface feature meshes (file-cached)
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
	// The type's opener with the placement's override applied. Shared by
	// SpawnDoor and the inspector's edit so both land on the same resolution.
	void ResolveDoorOpener(Door& door, const std::string& type,
						   const std::string& openerParam,
						   const std::string& sideParam);
	// Where an opener hangs in WORLD space, for `face` = +1 / -1 (the two sides
	// of the door). Declared down here rather than beside ToggleDoorAhead
	// because a member's SIGNATURE can only name nested types already declared,
	// and Door is defined further down the class.
	Vec3 OpenerPos(const Door& door, float face) const;
	Door* DoorAt(int x, int z);
	const Door* DoorAt(int x, int z) const;
	// (DoorwayFacing moved to DungeonMap — it only ever read the map, and the
	// placement resolver needs it without dragging the whole world in.)
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
	// items.cat `liquid_color`: generates the liquid inside the kind's glass and
	// appends it to its model as one more part (DungeonWorld_Load.cpp).
	void AddLiquid(ItemKind& kind, const CatalogEntry& def, const std::string& modelFile);
	// Lays a RUNTIME drop (negative id) on the floor: into the slot of a
	// runtime drop that was picked back up (it is dead - the save skips it)
	// when there is one, else onto the end. With ReserveDropRoom's headroom, a
	// drop allocates nothing, and a pick-and-drop loop never grows the list.
	void PlaceDrop(const Item& item);
	// Tops up m_items' spare capacity for drops, at load time (kDropRoom).
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
	// A stair prop's transform: on its cell, turned OPPOSITE its facing (the
	// meshes' +Z is the way you travel on them; the facing is the way you step
	// off). Shared by placement and the stair inspector's turn.
	Mat4 StairPropWorld(const DecorationKind& kind, int x, int z, Direction facing) const;
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
	// decoration path.
	static std::unique_ptr<MultiMaterialModel> BuildMultiMaterialModel(
		gfx::GraphicsDevice& device, const assets::ModelData& model);
	// Bakes the entry's metallic=/roughness=/color= overrides into an authored
	// model's per-submesh materials (the multi-material twin of the DecorationKind
	// ApplyPropMaterial overload).
	static void BakeCatalogMaterial(MultiMaterialModel& model,
									const CatalogEntry* def);
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
	// may bake its own material overrides into it.
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
	// lands its payload on every combatant of its target side in the cell it died
	// in — CELL-WIDE, where a hit is lane-wide (see the definition for why).
	void ResolveProjectileExpiry(const ProjectileExpiry& expiry);
	// A bolt that broke against a SHUT, BREAKABLE door in (cx, cz) strikes it
	// (its damage, then its procs - a fire bolt may set it alight) and returns
	// true. False when there is no such door: an immune door is not a target.
	bool StrikeDoorWithBolt(int cx, int cz, const ProjectileExpiry& expiry);
	// The same for a THROWN item (DungeonWorld_Throw.cpp): its blow, an enchanted
	// weapon's element and its on_hit effects. The item still comes down in front.
	bool StrikeDoorWithThrow(int cx, int cz, const ProjectileExpiry& expiry);
	// A THROWN item's two ends (DungeonWorld_Throw.cpp). A strike: a monster in
	// the lane takes the blow through fx::Deal as a swing's (a carried blast
	// bursts instead), the thrower trains `throwing` on a landed one, and the
	// item falls in that cell either way. A landing: the item comes down in the
	// last OPEN square it flew through (a wall's square is never one), so a
	// thrown item is never lost - unless it shatters (`throw_breaks`), when what
	// it carried is let go there.
	bool ResolveThrowHit(const ProjectileImpact& impact);
	void LandThrown(const ProjectileExpiry& expiry);
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
	std::vector<ActiveBlast> m_activeBlasts;
	// Advance every live blast and apply whatever has come due. Called per frame.
	void UpdateBlasts(float dt);
	// Apply one tick's worth at one square: monsters, the party (friendly fire),
	// and whatever pieces of dungeon stand there.
	void ApplyBlastHit(const blast::Hit& hit, const ActiveBlast& active);
	// Walk every breakable piece of dungeon standing in a cell — THE one place
	// that knows which kinds those are, so a new one reaches blasts, bolts and
	// whatever comes later all at once. (Forward-declared: the adapter itself is
	// defined further down beside the two combatant ones, and a reference in a
	// std::function needs only an incomplete type.)
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
	// Douse a broken fixture: its light, flame and smoke all go with `lit`.
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
	// The consequence table a source actually uses: its own when it authored
	// one, else the balance.cat default. Resolved per fumble so a Balance dialog
	// change lands on the next swing rather than on the next level load.
	// A VIEW, copying nothing: onto `own`, or onto `fallback` (the caller's
	// inline default table, filled here), since a fumble is a steady-state event.
	std::span<const mishap::Entry> FumbleTable(const std::vector<mishap::Entry>& own,
											   bool severe,
											   mishap::DefaultTable& fallback) const;
	// Lay an item on the floor of a cell as a RUNTIME drop (negative id, saved
	// as a `drop` diff) — NOT an .ent record, which is what an editor placement
	// authors. Shared by the cursor drop and by a fumbled weapon.
	void DropItemInCell(const std::string& typeId, int cx, int cz, float charge = -1.0f);
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
	// resting (it does nothing), which is what lets the wound path call it
	// unconditionally rather than testing the flag at the call site.
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
	// strike, mitigate, absorb — is shared.
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
	// called and what breaking it does, shared by ForEachBreakableAt (a blow, a
	// blast) and TickBreakables (an effect riding it), so the two cannot drift.
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
	// Rebuilds the single chunk region (chunkX, chunkZ) in place.
	void RebuildChunkRegion(int chunkX, int chunkZ);
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
	// Shadow-slot budgeting + cube-cache scheduling (UpdateLights feeds it the
	// frame's lights; RenderShadowMaps asks it which cubes to redraw). See
	// ShadowScheduler.h.
	ShadowScheduler m_shadows;
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
	// Swatch thumbnails for surface types the active level has NOT loaded
	// (LoadSurfaceThumb), by texture SET name - a set is a pool asset, so one
	// survives level and world changes. A null entry was tried and missing.
	// Bounded by the surface catalogs (~16 KB and one SRV slot apiece).
	std::unordered_map<std::string, std::unique_ptr<gfx::Texture>> m_surfaceThumbs;
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
	// Feature meshes by MODEL FILE (a feature type's `model`.gltf). Features are
	// project-wide, not per level, so each file is read once per world and the
	// per-type maps below point into this. Node-based on purpose: the maps hold
	// pointers, which a flat_map would invalidate on insert.
	std::unordered_map<std::string, assets::MeshData> m_featureMeshCache;
	// Niche panels by wallfeatures.cat type (each entry's `model`.gltf); the mesh
	// builder stamps the one matching a niche's type. NicheMeshFor resolves it.
	std::flat_map<std::string, const assets::MeshData*> m_nicheMeshes;
	const assets::MeshData* NicheMeshFor(const std::string& type) const;
	// See-through bore panels by wallfeatures.cat type (its `model`.gltf); stamped
	// on the two flanking faces of a bored wall block. BoreMeshFor resolves it.
	std::flat_map<std::string, const assets::MeshData*> m_boreMeshes;
	const assets::MeshData* BoreMeshFor(const std::string& type) const;
	// Surface-feature tiles by surfacefeatures.cat type (its `model`.gltf), split
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
	// The party's own light (DungeonWorld_Light.cpp): the cursor's item
	// (borrowed, SetCursorItem), a scratch slot the cursor's torch burns
	// through, and the per-frame passes.
	HeldItem* m_cursorItem = nullptr;
	ItemSlot m_cursorScratch;
	// DropAtPartyFeet's id, assigned rather than constructed (a guarded frame).
	std::string m_dropIdScratch;
	// The glows HandPuff leaves, each fading over its `life` (UpdateLights adds
	// the live ones). FIXED: a cast lands in a guarded frame; a fifth puff while
	// four still glow takes the oldest's place.
	struct HandGlow {
		Vec3 pos{};
		Vec3 color{};
		float timeLeft = 0.0f;
		float life = 0.0f;
		float intensity = 0.0f; // at the puff; fades to nothing over `life`
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
	// Burns every lit torch held (hands, cursor) by `dt`, and puts out any
	// stowed in a pack.
	void TickCarriedLight(float dt);
	// One light per lit torch held, at its member's side of the eye.
	void AppendCarriedLights(float time);
	// Burns one slot's torch; true if it burnt out (and became its stub).
	bool BurnTorch(ItemSlot& slot, float dt, const Character* holder);
	// 1 = a torch at full light, falling to a floor over its last tenth.
	static float TorchBrightness(const ItemKind& kind, float charge);
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
	// EVERY eval-harness field the world holds, in one member (see `Harness`).
	// Four bools-and-counters that used to sit loose among the world's own state
	// reading like something nobody had got round to explaining.
	Harness m_harness;
	// REST. Transient by design — not saved, so a save made mid-rest loads
	// standing up. `m_restLockstep` remembers the AI mode rest replaced, because
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
	// still be in a worker's hands. Now a grid no one else holds (use_count == 1)
	// is refilled in place, and a level of a new size builds a SPARE beside its
	// grid so the first such change has one waiting.
	std::vector<std::shared_ptr<std::vector<uint8_t>>> m_walkablePool;
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
	// LOCKSTEP AI (docs/eval-harness.md): with the bucket workers paused, run
	// each bucket's compute inline whenever its cadence has elapsed in SIM time.
	// `dt` is the world dt already scaled by timescale, so a run at timescale 20
	// — or one stepping whole seconds per frame — thinks exactly as often per
	// simulated second as a run at 1 does. That equivalence IS the feature.
	void TickLockstepAI(float dt);
	// Live monster with this stable runtimeId, or null if none (died/erased/level
	// changed). Linear scan — fine at this scale; swap for a map if counts explode.
	Monster* MonsterByRuntimeId(u32 id);

	std::flat_map<std::string, std::unique_ptr<DecorationKind>> m_decorationKinds;
	// unique_ptr so DecorationKind::tex stays valid as more sets are added
	// (flat_map stores values contiguously and reallocates on insert).
	std::flat_map<std::string, std::unique_ptr<PropTextures>> m_propTextures;
	// The model cache's store, by file name (see ModelFile). Each part is built
	// on first ask, so a file only ever drawn as a multi-material model never
	// uploads a single-mesh copy it would not use, and vice versa.
	struct CachedModel {
		std::shared_ptr<const assets::ModelData> data;
		std::shared_ptr<gfx::Mesh> mesh;                 // meshes[0]
		std::shared_ptr<const MultiMaterialModel> multi; // the template ModelMulti copies
	};
	std::unordered_map<std::string, CachedModel> m_modelCache;
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
	// The active level's m_entities records diverged from the .ent file on disk
	// (a prune/re-face edited them); stash them on leave so the divergence
	// survives the swap and savemap writes it.
	bool m_entsDirty = false;
	// The active level is PARKED (ParkActive): stashed, with the party outside it.
	bool m_parked = false;
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

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	int m_torchPalette = 0; // index behind m_torchColor (saved/restored)

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
