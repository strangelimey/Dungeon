// ============================================================================
// Game/Game.h — the dungeon crawler's app state machine.
//
// Game is the coordinator over the module classes - it owns them, wires their
// callbacks together (Game_Wiring.cpp), and runs the state machine that decides
// what updates and renders each frame:
//
//   GameSettings - user options (quality, display, volume, theme, colours,
//                  keys, HUD layout), persisted to settings.ini next to the exe
//   Project      - the open WORLD's catalogs and levels (assets/projects/
//                  <name>), with its overworld (WorldMap) and the dynamic
//                  WorldState beside it
//   SoundBank    - the loaded sound effects, shared by every system
//   LoadQueue    - staged loading, one task per rendered frame
//   DungeonWorld - the 3D world: map, party, monsters, fires, lights, camera;
//                  simulation + the shadow and scene passes. Built by LoadWorld
//                  when a game starts, destroyed when another world opens, and
//                  null on the title screen until then
//   GameUI       - menus, the settings page, HUD, character sheet, loading
//                  screens and overlays
//   MapView, MapEditor and the editor dialogs - the map overlay and the editor
//
// App states (AppState):
//   Loading      - boot load: just the menu essentials (sounds, title art, the
//                  portrait catalog), one task per frame, so the landing page
//                  appears fast even on a cold cache
//   Menu         - the landing page over the title art; nothing simulates
//   LoadingGame  - the world's game assets (meshes, scanned textures, the HUD),
//                  staged behind a progress screen the first time a game starts,
//                  continues or loads in that world
//   LoadingLevel - a level change mid-game (a stair, a pit, a dungeon entered,
//                  a save made on another level): only the world half re-stages
//   Playing      - the crawler: UI input -> world simulation
//   WorldMap     - travelling the overworld between dungeons. A STATE, not an
//                  overlay: no level is drawn behind it and nothing simulates
//   Paused       - Esc while playing or travelling: the world freezes and the
//                  pause menu draws over it; Esc backs out of settings, then
//                  resumes to m_resumeState
//   CharacterSheet - a party portrait clicked. NOT A PAUSE: over a level the
//                  world goes on simulating under the page while the input stays
//                  the sheet's; Esc closes a popup first, then resumes
//
// ESC NEVER QUITS, in any state - it only backs out. Quitting is an Exit entry
// (the landing list or the pause menu), the console's `quit` / `exit`, or the
// window's own close button.
//
// A WORLD SWITCH happens in the process (SwitchWorld -> m_pendingWorld, applied
// at the top of the next frame); only an adapter change relaunches the exe
// (RestartApp). The ROSTER is 1..party::kMaxMembers members, and ResetRoster
// replaces the vector when a new party's size differs.
//
// Assets load from paths::AssetsDir(): the repo's assets/ in a dev build
// (DN_ASSETS_DIR), the copy beside the exe only in a packaged one; tools/
// AssetBaker regenerates the baked ones. Engine modules know nothing about
// dungeons - all gameplay rules live in this module.
// ============================================================================
#pragma once

#include "Audio/AudioEngine.h"
#include "Core/AllocTrack.h"
#include "Core/ThreadManager.h"
#include "Game/AssetDialog.h"
#include "Game/AssetPicker.h"
#include "Game/Character.h"
#include "Game/ClipPoke.h"
#include "Game/PartyRules.h"
#include "Game/DevConsole.h"
#include "Game/DungeonWorld.h"
#include "Game/GameSettings.h"
#include "Game/GameUI.h"
#include "Game/LoadQueue.h"
#include "Game/MapEditor.h"
#include "Game/MapView.h"
#include "Game/EntityInspector.h"
#include "Game/FireEffect.h"
#include "Game/FixtureInspector.h"
#include "Game/BalanceDialog.h"
#include "Game/InspectPicker.h"
#include "Game/LevelSettingsDialog.h"
#include "Game/Generate.h"
#include "Game/GenerateDialog.h"
#include "Game/ValidateDialog.h"
#include "Game/WorldMap.h"
#include "Game/WorldMapView.h"
#include "Game/WorldSettingsDialog.h"
#include "Game/WorldsDialog.h"
#include "Game/NewWorld.h"
#include "Game/NewWorldDialog.h"
#include "Game/MonsterConfigDialog.h"
#include "Game/ButtonInspector.h"
#include "Game/StairInspector.h"
#include "Game/DoorInspector.h"
#include "Game/NicheInspector.h"
#include "Game/ProjectileInspector.h"
#include "Game/PropInspector.h"
#include "Game/Project.h"
#include "Game/StyleLibrary.h"
#include "Game/TypeEditorDialog.h"
#include "Game/SoundBank.h"
#include "UI/FontLibrary.h"
#include "Graphics/ModelPreview.h"
#include "Graphics/PostProcess.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Process.h"
#include "Platform/Window.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

class Game {
public:
	Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		 gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio);
	// Stops all playback: the audio engine outlives Game but plays zero-copy
	// out of the Game-owned SoundBank (see ~Game in Game.cpp).
	~Game();

	void Update(float dt);
	void Render(ID3D12GraphicsCommandList* list);
	// The end-of-frame bookkeeping Render does, for a `-headless` run that never
	// calls it. NOT optional: the staged loader gates on the frame counter that
	// lives at the bottom of Render, so without this a headless run never
	// finishes loading. See the definition.
	void EndHeadlessFrame();

	// Set by an Exit entry (the landing list or the pause menu), the console's
	// `quit` / `exit`, a finished eval batch and RestartApp - never by Esc. The
	// main loop polls it to leave cleanly.
	bool QuitRequested() const { return m_quitRequested; }
	// A `-headless` run given no script: no window, no console a key could reach
	// and nothing to run, so nothing could ever drive it or end it. Main calls
	// this and the run quits as soon as its boot load has landed. It is how a
	// relaunch from a headless script run (`video restart`, which leaves the
	// script out - see RelaunchCommandLine) ends rather than idling on a title
	// screen nobody can see until something kills it (code-review batch 68).
	void QuitOnceLoaded() { m_quitOnceLoaded = true; }

	// THE EVAL HARNESS'S CLOCK (docs/eval-harness.md). Advance the world by
	// `seconds` of SIM time, right now, in fixed sub-steps — no frames
	// presented, no input read. Returns how many sub-steps actually ran.
	//
	// FIXED SUB-STEPS, not one big dt, and this is the load-bearing part: nearly
	// everything that paces this game counts DOWN a timer by dt — hand
	// cooldowns, monster attack and move cooldowns, stamina holdoff, the AI's
	// bucket clocks — so a single 30-second dt would let a monster take ONE step
	// and swing ONCE, and report the resulting non-fight as a measurement.
	// Stepping at a fixed tick makes thirty simulated seconds mean thirty
	// seconds of fighting, and makes it mean the same thing every run.
	//
	// Only steps while Playing; a level transition mid-step (a monster shoves
	// the party onto a stair) stops the run early rather than being followed,
	// since an eval that changed level is no longer measuring what it set up.
	// So does a party wipe: the app has left play, and a clock ticked on past it
	// charged the tally with seconds after the last member fell (C443).
	//
	// WHY it stopped, not just how far it got. The seconds actually run were
	// always reported, and for a whole release nothing read them: `step 3600`
	// runs 3333.33s against the ceiling below, and two suites labelled that "an
	// hour" (docs/eval-audit.md F3). A caller that has to compare two numbers to
	// notice a truncation will not notice it; one handed `Ceiling` will.
	enum class StepStop {
		Complete,    // ran every tick asked for
		Ceiling,     // hit kMaxStepTicks — the script asked for more than one call gives
		LevelChange, // the party left the level being measured
		RestEnded,   // a rest finished, which is the length a rest measurement wants
		PartyWiped,  // the last member fell and the app went back to the title
		NotPlaying,  // the app is not in play; nothing ran at all
	};
	int StepWorld(float seconds, StepStop& why);
	// WHY a clocked run stopped, in words - the ONE place `step` and `rest until`
	// both get theirs (code-review C444: `rest until` called a level change, a
	// wipe and the ceiling all "hit the cap"). Empty for a run that went the
	// whole way; RestEnded carries the rest's own reason.
	std::string StepStopReason(StepStop why) const;

	// The per-call ceiling, named so `step` can quote it rather than restate it.
	// A ceiling PER CALL, not per second: a script asking for an hour by mistake
	// should come back and say how far it got, rather than appearing to hang
	// with a black window and no way to interrupt it.
	// The world's own fixed tick (DungeonWorld::kTick), which a resting frame's
	// long dt runs in too - so a rest and a `step` simulate alike.
	static constexpr int kStepTicksPerSecond = DungeonWorld::kTicksPerSecond;
	static constexpr int kMaxStepTicks = 200000; // ~55 minutes of sim
	static constexpr float kMaxStepSeconds =
		static_cast<float>(kMaxStepTicks) / kStepTicksPerSecond;

	// What the app is doing, as a word — for the eval harness and the `state`
	// command. A script CANNOT otherwise tell: dev commands reach the world from
	// the MENU too (it is built at load), so `tp` and `monsters` answer happily
	// while the party is dead and the title screen is up. That trap cost a
	// debugging session, and a harness that cannot see it would report a whole
	// suite of encounters that never ran.
	const char* StateName() const;

	// Put the app back in Playing after a party wipe sent it to the title (see
	// the `heal` command). Only valid once a game has been loaded — from the
	// menu with no world behind it, the HUD does not exist and Playing would
	// dereference nothing (the crash the eval runner found on its first run).
	void ResumeAfterHeal() {
		if (m_gameLoaded && m_state == AppState::Menu) m_state = AppState::Playing;
	}

	// --- the eval batch runner (`-eval <script>`; docs/eval-harness.md) ------
	// Queue a script of console commands for the game to run on ITSELF. This is
	// the whole reason the harness is drivable: PostMessage-and-screenshot has
	// no way to know when a load finished, cannot read the console's answers,
	// and silently swallows everything after an accidental console toggle. A
	// script the game owns has none of those failure modes.
	//
	// Returns false if the file could not be read — the caller should not then
	// sit at the title screen forever pretending to be a test run.
	bool LoadEvalScript(const std::string& path);
	// Append a script to run AFTER the current one, in the same process. This is
	// what makes a long run affordable: a level load is ~12 seconds against a
	// `reset`'s ~340 ms, so one process running twenty scripts costs one load
	// instead of twenty (docs/eval-harness.md "Recycling the world").
	//
	// The runner does NOT reset between scripts — a script says `reset` when it
	// wants a clean baseline, and a progression series deliberately does not
	// (Michael's call). Queueing is therefore purely "run these in order".
	void QueueEvalScript(const std::string& path) { m_evalPending.push_back(path); }
	// True from the moment a script is loaded until the batch is finished: every
	// frame in between is the runner's, including the one that writes a script's
	// verdict and loads the next. NOT "lines left to run", which is false on
	// exactly that verdict frame. The pump's own early-out and the allocation
	// guard (SteadyStateFrame) both ask this, so they cannot disagree about which
	// frames belong to a scripted run.
	bool EvalRunning() const { return !m_evalFinished && !m_evalName.empty(); }
	// The process exit code a scripted run should return: 0 when every script
	// finished with every line matching a command, 1 otherwise. An ordinary play
	// session loaded no script and is always 0 — checked FIRST, because
	// "finished" is false for it too and it must not read as a failed run.
	int EvalExitCode() const {
		if (m_evalName.empty() && m_evalScripts == 0) return 0; // not a test run
		return m_evalFailed == 0 && m_evalFinished ? 0 : 1;
	}

private:
	enum class AppState {
		Loading,
		Menu,
		LoadingGame,
		LoadingLevel, // mid-game level transition (P6): re-stage the world load
		Playing,
		// On the overworld, travelling between dungeons (docs/world-map.md).
		// A STATE, not an overlay: no level is loaded and no scene is drawn
		// behind it — this is where the party IS, not a view of where it is.
		WorldMap,
		Paused,
		CharacterSheet
	};
	// A state as StateName's word, for any state (the `backdrop` readout).
	static const char* StateWord(AppState state);

	// --- construction (called once from the ctor; see Game.cpp) -----------
	void WireModuleCallbacks(); // the world↔UI/editor callback graph
	// The callbacks set ON the world object — wired each time one is built.
	void WireWorldCallbacks();
	// --- the world's lifetime (docs/world-on-demand.md) ----------------------
	// LoadWorld reads a world's project and builds its DungeonWorld (which
	// loads a level of its own), unloading any other first; its game assets
	// then arrive through BuildGameLoadTasks like a first start. False, and
	// nothing changed, if no such world exists. UnloadWorld drains the GPU —
	// in-flight frames still reference the world's buffers — closes everything
	// borrowing from it, and destroys it; the title screen has none.
	bool LoadWorld(const std::string& folder);
	void UnloadWorld();
	bool WorldLoaded() const { return m_world != nullptr; }
	// The dev-console command table, one Register*Commands per file by concern
	// (arg helpers shared through Game/DevCommandArgs.h).
	void RegisterDevCommands(); // the general ones (Game_DevCommands.cpp)
	void RegisterDungeonCommands(); // the dungeon tier's (Game_DevDungeons.cpp)
	void RegisterWorldCommands(); // the world tier's (Game_DevWorld.cpp)
	void RegisterLevelCheckCommands(); // `levelcheck` and its mutations (Game_LevelCheck.cpp)
	void RegisterDiagnosticCommands(); // guards, threads, health (Game_DevDiagnostics.cpp)
	void RegisterPartyCommands(); // members, gear, pools (Game_DevParty.cpp)
	void RegisterEvalCommands(); // the eval harness's (Game_DevEval.cpp)
	void RegisterStyleCommands(); // styles and the library (Game_Styles.cpp)
	void RegisterMapIconCommands(); // `mapicons` and its survey (Game_MapIcons.cpp)
	void RegisterDisplayCommands(); // `video` (Game_Display.cpp)
	// `mapicons survey on`: every monster kind's map icon beside its model's
	// asset-picker tile, drawn over everything but the console (Game_MapIcons.cpp).
	bool m_mapIconSurvey = false;
	void DrawMapIconSurvey(float dw, float dh);
	// The encounter tally as the `tally` command prints it: one key=value
	// line starting "TALLY ". Shared with `alloctest`'s verdict.
	std::string TallyLine() const;

	// --- loading (one task per frame while a loading screen shows) ---------
	void BuildBootLoadTasks(); // menu essentials, run before the landing page
	void BuildGameLoadTasks(); // the dungeon itself, run on first game start

	// Opens the create dialog for a palette category. Both ways in come through
	// here — the palette's "+ New..." (Import, nothing picked) and the type
	// editor's Duplicate (preset to a copy of `asset`) — so the two can't drift
	// on the category's label / catalog / texture-set-vs-model mode.
	void OpenCreateDialog(MapEditor::PaletteCat cat, AssetDialog::Source source,
						  const std::string& asset = {});
	// Asset bake (P4c): launch the AssetBaker command for the current step; and,
	// when the bake finishes, write the new catalog entry + save the project.
	bool StartBakeStep();
	void FinishBake();
	// Writes a newly created type's catalog entry (its shape seeded from the
	// category's schema defaults) and makes it reachable — a surface type joins
	// the viewed level's palette. Shared by the bake path and the no-bake
	// sources (Installed / Duplicate), which have nothing to bake. Returns ""
	// when it wrote the entry, else why it REFUSED: an entry whose model its
	// category could not load (UnloadableModelReason) is never written.
	std::string CreateCatalogEntry(const AssetDialog::CreateRequest& req);
	// Why a `catalogKey` type built from entry `e` could not load its model -
	// every field its loader reads one from, under either extension (AssetUtil's
	// FirstUnloadableModel), and a door's `trim` naming no doors.cat entry, which
	// SpawnDoor opens as a file - or "" when it could, or the catalog loads no
	// model. The create dialog and the type editor's Save refuse on it
	// (code-review C301): the load is a LoadModelOrDie.
	std::string UnloadableModelReason(const std::string& catalogKey, const CatalogEntry& e) const;
	// Why the type editor's Save of `catalogKey` entry `merged` (as it would be
	// written) is refused, or "": a model its category could not load
	// (UnloadableModelReason), or a terrain glyph the world could not be read
	// with (TerrainSaveRefusal). Both `typeset` and the dialog's Save ask it.
	std::string TypeSaveRefusal(const std::string& catalogKey, const CatalogEntry& merged) const;
	// Why `id` may not be a new or renamed `catalogKey` type because a RELATED
	// catalog already has it (code-review C302, Project::RelatedCatalogUsing):
	// one item catalog's id in another's (FindItem would find only one), or one
	// prop catalog's in another's. "" when it may. Create, duplicate and rename
	// all ask it.
	std::string RelatedIdRefusal(const std::string& catalogKey, const std::string& id) const;
	// What "Use installed" does with pool texture set `set` adopted as a surface
	// of `catalogKey` (walls / floors / ceilings) - code-review C407. A set's worn
	// meshes are ONE file per set in the shared pool, kind baked into the
	// geometry, so a set already painted as another kind is REFUSED (`refusal`
	// says by whom: its shipped record, a type in any world, the template or the
	// library, or the import that baked it), and a set whose meshes exist is
	// used AS IT IS (`bake` false): re-baking would reshape every type that
	// already paints with it. Not a surface catalog = nothing to say.
	struct SurfaceAdopt {
		std::string refusal; // "" = may be adopted
		bool bake = false;   // no worn meshes yet: bake them as this kind
	};
	SurfaceAdopt AdoptSurfaceSet(const std::string& catalogKey, const std::string& set) const;
	// Records an IMPORT in the project's provenance manifest (imports.cat):
	// which pool asset, from where, with which options. The baked asset is
	// gitignored, so this is what makes a created type reproducible from a
	// clean checkout (tools/ReplayImports.ps1 replays them).
	void RecordImport(const AssetDialog::CreateRequest& req);

	// Type editor Save: merge the dialog's working fields into the catalog entry
	// and persist. Starts from the EXISTING entry, so fields the dialog doesn't
	// know (hand-authored, or MonsterConfigDialog's animation rows) survive.
	void WriteTypeFields(const TypeEditorDialog::Config& cfg);
	// The entry WriteTypeFields would write, unwritten - what the Save's model
	// check judges before anything reaches the catalog.
	CatalogEntry MergedTypeEntry(const TypeEditorDialog::Config& cfg) const;
	// The type editor's Save of a `rebakes` field (a surface's texture, relief
	// or wear): launches the async wornblock bake of `merged`'s texture set at
	// its relief/wear (relief unset bakes at the set's own, Assets/WornSets.h)
	// and holds `cfg` UNWRITTEN until the bake lands (LandRestyleBake). Returns
	// "" when the bake was launched, else why nothing was: a bake already
	// running, or a baker that would not start. Writing first and baking after
	// (code-review C346) left a failed bake's new `texture` in the catalog, and
	// the next level load aborted on worn meshes that were never made.
	std::string StartRestyleBake(const TypeEditorDialog::Config& cfg, const CatalogEntry& merged);
	// The restyle bake's end, from Update's poll: on a clean exit the held Save
	// is written and the dungeon blocks reload; on a failure NOTHING is written,
	// and the type editor (if it made the Save) stays open saying so.
	void LandRestyleBake(int exitCode);
	// Opens the type editor for a catalog id (the palette's right-click), or
	// does nothing when the catalog/entry is unknown.
	void OpenTypeEditor(MapEditor::PaletteCat cat, const std::string& id);
	// `typeset dialog ...`: the type editor driven step by step from the console
	// (open, its rows, a tab, typing a stage id, Save), each step reporting
	// where it stands - what a harness reads (Game_DevWorld.cpp).
	void TypesetDialog(const std::vector<std::string>& args);
	// The Balance dialog on the live tuning, its Effects tab filled from the
	// project's effects.cat (the toolbar button and the console's `balance`).
	void OpenBalanceDialog();
	std::vector<BalanceDialog::EffectRow> EffectRows() const;
	// The type editor was opened OVER the Balance dialog (its Effects tab): when
	// it closes, the tab's rows are rebuilt, since a save may have renamed one.
	bool m_typeOverBalance = false;
	// Creates an entry in a pure-data catalog (dungeons/terrain/quests) with a
	// free id and the schema's defaults; the caller opens the type editor on it
	// so the id can be renamed there. "" if the category has no catalog. A
	// TERRAIN also gets a glyph no other kind has, and is saved and handed to
	// the world at once, so it can be painted (code-review C345); "" when every
	// glyph is taken.
	std::string CreateAuthoredType(MapEditor::PaletteCat cat);
	// STYLES (Game_Styles.cpp): add a library style to this world (the style
	// and whatever it points at that the world lacks, saved; monsters it lacks
	// reported), and save a world style back to the library. Both report what
	// they did through onMessage and return it for the console.
	StyleLibrary::AddResult AddStyleFromLibrary(const std::string& id);
	bool SaveStyleToLibrary(const std::string& id, std::vector<StyleLibrary::Copy>& out);
	// Renames a catalog type EVERYWHERE: the entry, every level record that
	// names it (DungeonWorld::SweepTypeRefs), the cross-catalog references
	// (stairs `pair`, doors `key`) and the project's default fixture ids. False
	// (with a reason in `problem`) when the new id is taken or invalid.
	bool RenameType(const std::string& catalogKey, const std::string& id,
					const std::string& newId, std::string& problem);
	// Deletes a catalog type, but only when NOTHING references it — the sweep
	// names the levels that do, so the caller can say where. Refusing is the
	// point: a dangling type id would abort the level load that meets it.
	bool DeleteType(const std::string& catalogKey, const std::string& id,
					std::string& problem);
	// Every reference to a type OUTSIDE the levels: another catalog entry's
	// field (stairs `pair`, doors `key`) or a project.ini default. Returns the
	// number found, rewriting them when `newId` is given.
	int SweepCatalogRefs(const std::string& catalogKey, const std::string& id,
						 const std::string* newId);
	// Save files naming a type. A save stores an editor-placed monster or a
	// dropped item as a WHOLE spawn row carrying its type (SaveData::EntityState
	// with id < 0), so a rename or delete strands those rows: on load the type
	// resolves through the "unlisted type" fallback (<type>.gltf + default
	// stats) — or aborts, if the type's id and model name differ. Saves are not
	// rewritten (they are dev-cycle artifacts, like a level rename's), so the
	// names are REPORTED and the caller says so.
	std::vector<std::string> SavesReferencingType(const std::string& id) const;
	// Reports (log + world message) the saves that still name a type after it
	// was renamed or deleted.
	void WarnStaleSaves(const std::string& id);
	// Opens a monster type's animation + behaviour dialog (the type editor's
	// extra button — that dialog owns those rows).
	void OpenMonsterConfig(const std::string& id);

	// Copies the active project (with its edits) from the exe-side asset copy
	// back into the repo source tree. False (with a log) when no source path is
	// baked in (shipped build) or the copy fails. Shared by the editor's
	// "To source" button and the synctosource console command.
	bool SyncProjectToSource();

	// The typeface audition (docs/fonts.md Phase 4): the `font` console
	// command's body, and the fonts.cat writer behind `font save`.
	void FontCommand(const std::vector<std::string>& args);
	// `editor palette ...` (the category bar, for the harness).
	void PrintPalette(const std::vector<std::string>& args);
	// One line of a thumbnail brightness survey (`assetpicker survey`, `editor
	// palette swatches`; code-review C158): where an image was drawn (device px)
	// and the stored mean of the file it was loaded from, which a correct draw
	// averages to. A harness photographs the window and compares.
	void PrintThumbSurveyLine(std::string_view head, const std::string& name,
							  const std::string& stem, const gfx::Rect& rect, bool drawn);
	// `editor dock ...` / `editor overview ...` (MapView_Docks.cpp).
	void PrintDocks(const std::vector<std::string>& args);
	bool SaveFontCatalog();

	// --- the world tier (Game_World.cpp) -----------------------------------
	// Resolves terrain.cat into rules and loads the project's world map. Called
	// by LoadWorld, so once for EVERY world built or switched to (UnloadWorld
	// resets m_worldMap first); leaves it empty when the project has no
	// terrain.cat entries or no world/world.map.
	void LoadWorldMap();
	// terrain.cat's kinds as the world reads them (GlyphOf's '?' for a glyph
	// that is not one character) - what LoadWorldMap parses against.
	WorldMap::TerrainRules CatalogTerrainRules() const;
	// Hands the loaded world terrain.cat's kinds, matched by id (WorldMap::
	// SyncTerrains), after every write of the catalog - a Save, a "+ New", a
	// delete - and WRITES the world when a kind went or a glyph changed, whether
	// or not the loaded world's text moved: world.map ON DISK must spell its
	// squares as the catalog on disk does, and a square painted over since the
	// last save is still the old kind there, or the next launch cannot read it
	// (code-review C345). A rename needs no sync: SweepCatalogRefs renames the
	// world's copy. False when the catalog's kinds could not be applied (logged;
	// only a hand edit gets there).
	bool SyncWorldTerrains();
	// Why a terrain entry `merged` (as the Save would write it) cannot be saved,
	// in the dialog's words, or "": its glyph must be one character, not
	// lowercase, not reserved (WorldMap::CheckGlyph) and no other kind's.
	std::string TerrainSaveRefusal(const CatalogEntry& merged) const;
	// Writes world/world.map from the loaded world, and READS IT BACK to check
	// it round-trips. Part of `savemap`. False when there is no world, or the
	// write failed.
	bool SaveWorld();
	// Opens the world settings dialog (W4) on the loaded world, gathering the
	// catalog dungeons and the manifest fields it edits alongside it. A
	// non-empty `selectLocation` opens it ON that doorway — what a right-click
	// on the world map wants.
	void OpenWorldSettings(const std::string& selectLocation);
	// Its callbacks, split out of the wiring file because every one of them is
	// the same sentence: open an undo step, let WorldMap decide, close the step
	// with whether anything changed.
	void WireWorldSettingsDialog();
	// Puts the world state where a NEW GAME starts it: the world map's own start
	// cell, revealed, nothing discovered, no time elapsed. A project with no
	// world leaves it blank. Ends with ReserveWorldState.
	void ResetWorldState();
	// Gives the world state's lists room for every entry the catalogs can make
	// (WorldState::Reserve, code-review C212): a quest per quests.cat entry and
	// per item quest hook, a flag per flags.cat entry and per item flag hook, a
	// place per world-map location and per item `reveals`, each string as long
	// as the longest id or value among them. So a quest moving on, a place
	// revealed or a flag set in play constructs nothing. After every new game
	// and every load (a load takes the save's lists whole, spares and all gone).
	void ReserveWorldState();
	// One step on the world map. False when the target is off the grid or
	// impassable — the caller says so rather than the move silently not
	// happening. A successful step advances world time by the cost of the
	// square ENTERED, settles that span's supply cost, and reveals what the
	// party can now see.
	bool TravelStep(int dx, int dz);
	// Charges `hours` of travel against the party. Supplies only — see the
	// comment at the definition for what is deliberately NOT settled.
	void SettleJourney(float hours);
	// Camp where the party stands: rest, reached from the world map, settled in
	// the same slices a journey uses and stopped by the same rules rest already
	// has. Returns the hours it lasted (0 = it never started).
	float Camp();
	// An item has been LIFTED. Applies its quest/flag/reveal hooks — the two
	// content hooks the design asked for, plus the flag escape hatch. A lift is
	// a guarded-frame event: this allocates nothing, a hook that fires included.
	void OnItemFound(const std::string& itemId);
	// Reveals a world cell and its eight neighbours, discovering any location
	// standing on them.
	void RevealAround(int x, int z);
	// Enter or leave the world map. P4 gives this a reason to happen (a
	// location, a dungeon exit); for now the dev console is the way in.
	void SetOnWorldMap(bool on);
	// Enter the dungeon behind a world-map location, at its entry level. False
	// (with a reason said or logged) when the location is unknown, undiscovered,
	// not a dungeon, or names one with no levels.
	bool EnterLocation(const std::string& id);
	// Leave the dungeon for the world map. `viaLocation` is the doorway being
	// used — an exit stair names its own, since a dungeon may have several and
	// the back way does not surface at the front gate. Empty falls back to the
	// location the party came IN by.
	bool LeaveDungeon(const std::string& viaLocation = {});
	// The tail of a load whose save was made ON THE WORLD MAP: back to the
	// travel screen rather than into the level loaded underneath, with that
	// level PARKED again when the party had walked out of it (`parked` — its
	// state rode the save), so re-entering it keeps what happened there.
	void ResumeOnWorldMap(bool parked);
	// The two QUESTIONS in front of those (Michael, 2026-09-24): walking onto a
	// doorway on the world map asks before going in, and stepping onto an exit
	// stair asks before leaving. Only the WALK asks — Enter on a doorway and the
	// `enter` / `leave` commands are already the deliberate act, and the eval
	// harness steps the world itself, so no script ever meets a question.
	void OfferEntrance(); // at the party's world square, if it holds a doorway
	void OfferExit(const std::string& viaLocation);
	// A stair the world raised: an exit asks (OfferExit), anything else loads.
	void FollowLevelTransition(const DungeonWorld::LevelTransition& t);

	// The playability check, with the world tier included. Every caller goes
	// through here rather than DungeonWorld::Validate directly, so no route can
	// quietly check the dungeons and skip the world.
	std::vector<validate::Issue> ValidateProject();
	// LIVE VALIDATION (docs/editor-updates-plan.md, P2): while the level editor
	// is open, re-run ValidateProject whenever DungeonWorld::EditRevision moves
	// - but only once no mouse button is held, so a drag is checked once, when
	// it ends, not per square. The map boxes what it finds (MapView::SetIssues).
	// `m_liveRev` is the revision the cached findings answer for; `m_liveValid`
	// false forces a run (the editor just opened, or a world was loaded).
	void RefreshLiveIssues(bool pointerHeld);
	std::vector<validate::Issue> m_liveIssues;
	u64 m_liveRev = 0;
	bool m_liveValid = false;
	bool m_liveTimed = false; // the first run's cost has been logged
	// The `world` dev command's report: size, terrain, areas and locations, one
	// line each. Empty-world-safe.
	std::vector<std::string> WorldReport() const;

	// --- worlds (W7) ---------------------------------------------------------
	// Michael's word for a project. Each is a self-contained game under
	// assets/projects/<name>: its own overworld, dungeons, levels and content.
	//
	// The DEFAULT world a start opens, chosen once at startup: `-project`, else
	// settings.ini's last world played, else kDefaultProject (see the
	// definition). Nothing is opened until a game starts (LoadWorld), and a
	// switch to another world happens in the process (SwitchWorld): the old one
	// is unloaded (UnloadWorld) and the new one built from scratch.
	static std::string ChooseProjectFolder();
	// The world a launch falls back on when the one it asked for is missing —
	// which is why it is also the one world that cannot be deleted.
	static constexpr const char* kDefaultProject = "dungeon-demo";
	// Opens `name` and starts a new game in it — in the process, next frame
	// (m_pendingWorld), no relaunch since docs/world-on-demand.md. Remembered as
	// the last world played unless -project named this run's. False, with the
	// reason in `why` when given, when no such world exists or an asset BAKE is
	// running: the bake writes its type into the world loaded when it lands, so
	// the switch would hand it to the next world (code-review C234).
	bool SwitchWorld(const std::string& name, std::string* why = nullptr);
	// "" when the world may change now, else why not (a bake running). Asked by
	// SwitchWorld and by a menu load of another world's save.
	std::string WorldSwitchRefusal() const;
	// Writes a NEW world beside this one and returns its name ("" on failure,
	// with the reason in `problem` when given). HOW is the spec's (NewWorld.h):
	// blank from the template, this world whole, or one of its levels. Every
	// kind is built in a hidden `.building-<name>` folder and renamed into
	// place only once complete, so a failure part-way leaves nothing a world
	// list would offer. A blank or one-level world gets ONE dungeon behind ONE
	// doorway, because the engine loads a level in DungeonWorld's constructor -
	// a world with nowhere in it could not stand up - and its level an exit
	// stair out to that doorway, so a party that walks in can walk out.
	// (Game_NewWorld.cpp.)
	std::string CreateWorld(const std::string& name, const NewWorldSpec& spec = {},
							std::string* problem = nullptr);
	// The minimal 16x16 rock block with a 3x3 room at 7..9 and the start at its
	// centre, as grid rows. A new world's first room and an empty new level
	// both start from it; FIXED on purpose (scenarios build on the room's place).
	static void AppendStarterRoom(std::string& map);
	static constexpr int kStarterSize = 16, kStarterCentre = 8;
	// The same box as a floor grid (row-major, 1 = open).
	static std::vector<u8> StarterFloor();
	// THE EXIT STAIR'S RECORD, placed ONE way (code-review C333; it was written
	// four ways, and the encounter's faced north whatever was there): a stair of
	// `type` (Project::ExitStairType) out to `dest`, newline included, for a map
	// that says `stairfacing arrive`. OnStart lays it ON the start square, facing
	// the first open side (north, east, south, west; north if none) - an
	// encounter's and the wizard's, where you arrive on it and step off into the
	// level, and a stair stepped off into rock is one nobody can use. BesideStart
	// lays it on the first open square beside the start, facing back toward it -
	// a new world's first room and a copied level, where the start is the
	// level's own; "" when no side is open. `open` says whether a square is
	// walkable floor, and false out of bounds. (Game_Generate.cpp.)
	enum class ExitSpot { OnStart, BesideStart };
	static std::string ExitStairRecord(const std::string& type, int startX, int startZ,
									   const std::function<bool(int, int)>& open,
									   ExitSpot spot, const std::string& dest);
	// The box in style `styleId` (Phase 7): `palettes` (wall, floor, ceiling)
	// take the style's theme members where it names any, and the returned text
	// is the level's `tags` record plus the `theme` records painting the room and
	// its walls. "" (palettes untouched) for no style or one `project` lacks.
	static std::string StyledStarterRecords(const Project& project, const std::string& styleId,
											std::array<std::vector<std::string>, 3>& palettes);
	// CreateWorld's three builders, each writing a whole world into `folder`
	// (the hidden build folder). False on failure, `problem` set when it knows.
	bool BuildBlankWorld(const std::string& folder, const std::string& id,
						 const NewWorldSpec& spec, std::string* problem);
	// Blank and Wizard: the spec's LIBRARY style into `made` (with the themes and
	// surfaces it names) before anything is built in it. True with no style.
	bool AddSpecStyle(Project& made, const NewWorldSpec& spec, std::string* problem) const;
	bool BuildCopiedWorld(const std::string& folder, const std::string& id,
						  std::string* problem);
	bool BuildLevelWorld(const std::string& folder, const std::string& id,
						 const std::string& stem, std::string* problem);
	bool BuildWizardWorld(const std::string& folder, const std::string& id,
						  const NewWorldSpec& spec, std::string* problem);
	// The wizard's TAG choices: every content tag the template's monsters
	// carry, sorted (they are what the wizard's world will draw from).
	std::vector<std::string> WizardTags() const;
	// Deleting one (W9). NOTHING BRINGS IT BACK — the undo history is in memory
	// and about THIS world, and a world made in the editor was never in git — so
	// the confirmation lives in the dialog (type the name, case-sensitive) and
	// the RULES live here, where the console reaches them too. The refusal is
	// "" when allowed, else the sentence saying why: no such world, the one
	// running, or the fallback every launch lands on.
	std::string WorldDeleteRefusal(const std::string& name) const;
	// What deleting it destroys, read off disk, for the confirmation to name.
	std::string DescribeWorld(const std::string& name) const;
	bool DeleteWorld(const std::string& name);
	// Deleting a DUNGEON (W10) takes its levels with it — files, manifest
	// entries, stashes — and no undo reaches it. So, as for a world: the rules
	// here, the typed confirmation in the dialog (the type editor's Delete on a
	// dungeon). The refusal is "" when allowed, else what is in the way: the
	// party's level, the game's opening, the harness level, a doorway, a level
	// another dungeon also claims, or a stair from outside leading in. Not
	// const: the stair walk parses levels not yet in memory.
	std::string DungeonDeleteRefusal(const std::string& id);
	// The confirmation's account of what goes: the dungeon, then each level.
	std::vector<std::string> DescribeDungeon(const std::string& id) const;
	bool DeleteDungeon(const std::string& id);
	void WarnSavesInLevels(const std::vector<std::string>& stems);
	// Mint a NEW level in `dungeonId` (files + manifest + the dungeon's level
	// list, so a level made from the editor is not born an orphan - W5) and
	// return its stem, "" on failure. An empty id creates it loose, as `levelN`.
	// Generated from `params` when given - with a stair from the dungeon's floor
	// above - else the minimal empty box, joined up later with the stair brush.
	// THE ONE WRITER of new levels: the [+] dialog, the `newlevel` and `generate`
	// commands all come through here, so a generated level cannot be named,
	// grouped or linked differently from an empty one.
	// The EMPTY box takes `emptyStyle` (Phase 7: the [+] dialog's style), its
	// look and tags; a generated level's style rides `params`.
	std::string CreateNewLevel(const std::string& dungeonId = {},
							   const generate::Params* params = nullptr,
							   const std::string& emptyStyle = {});
	// --- random encounters (Game_Generate.cpp, docs/world-map.md) ----------
	// Builds a throwaway space from the area's difficulty and its terrain's
	// tags and drops the party into it. It NEVER touches disk: generated to
	// text, parsed from text, discarded on the way out.
	bool StartEncounter(float difficulty, const std::vector<std::string>& tags,
						u32 seed);
	// Is the party in one right now? Keyed on the reserved level stem, so
	// there is no second flag to fall out of step with where the party is.
	bool InEncounter() const;

	// A generated level's text for CreateNewLevel, plus the cells a stair from
	// the floor above may land on, best first. The knobs' content pools are
	// resolved HERE from the project's catalogs by tag - the generator
	// itself never sees a catalog (Game/Generate.h).
	std::vector<std::pair<int, int>>
	ComposeGeneratedLevel(const std::string& stem, generate::Params params,
						  const std::vector<std::string>& tags, std::string& map,
						  std::string& ent);
	// Author the stair pair joining `stem` to the floor above it IN ITS DUNGEON,
	// on the first of `cells` that suits both levels. False (and a log line when
	// it had a floor to try) if there is none.
	bool LinkToFloorAbove(const std::string& stem,
						  const std::vector<std::pair<int, int>>& cells);
	// Regenerate the VIEWED level in place, as ONE undo step. Destructive by
	// design (the decision on record): the reroll replaces the level and Ctrl+Z
	// brings the old one back — which is why it must not go through a level
	// transition, since that clears the history the promise rests on.
	bool RegenerateViewedLevel(generate::Params params);
	// The squares of `stem` that something OTHER than a stair lands the party
	// on: the game's opening (project start_x/z) and every world-map doorway
	// with an authored entryx/z. A regenerate keeps them open like stairs.
	// Which level each lands on is Project::OpeningLevel / DoorwayLevel, the
	// one arrival rule (Game/Arrival.h).
	std::vector<std::pair<int, int>> ArrivalsOn(const std::string& stem) const;
	// Build the level text (palette from `donor`, `stairs` carried across
	// verbatim with their flags, `mood` the level's atmosphere / uistone
	// records), parse it, and hand it to the world.
	bool BuildAndInstall(const std::string& stem, const generate::Params& params,
						 const std::vector<std::string>& tags,
						 const DungeonMap& donor, std::span<const StairLink> stairs,
						 const std::string& mood);
	// Hands a level's whole text to the world in place of what it held (the
	// files stay untouched until `savemap`, like every editor edit).
	bool InstallLevelText(const std::string& stem, const std::string& map,
						  const std::string& ent);
	// Resolve the tags into the id pools the generator picks from - this
	// world's catalogs, or `project`'s (the new-world wizard draws from the
	// TEMPLATE, which is not the running world).
	void FillPools(generate::Params& params,
				   const std::vector<std::string>& tags);
	void FillPools(generate::Params& params, const std::vector<std::string>& tags,
				   const Project& project);
	// The new-world wizard's first floor (P5): generated from `spec`'s tag,
	// size, difficulty and seed, drawing content AND surfaces from `project`
	// (the new world's own catalogs), with an exit stair on its start square out
	// to `doorway`. The .map / .ent text; false when there is nothing to build
	// with. Deterministic: the same spec and catalogs give the same level.
	bool GenerateWizardLevel(const Project& project, const std::string& stem,
							 const NewWorldSpec& spec, const std::string& doorway,
							 std::string& map, std::string& ent);
	// One kind's threat (Game/Threat.h), its attacks resolved by the world when
	// one is loaded (spells, powers, on-hit effects), else melee from the catalog.
	threat::Parts ThreatOf(const CatalogEntry& monster) const;
	// Its power: the threat, or the entry's `power` override (Game/Power.h).
	double PowerOf(const CatalogEntry& monster) const;
	// The last generate's asked-vs-built, as the console prints it (English,
	// one line, with every branch's length) - docs/level-building.md: a knob you
	// cannot measure is a knob you cannot tune. The dialog's localized form is
	// two lines (ShowGenReport), since one ran 162px past the dialog.
	std::string GenReportText() const;
	// The level whose surface palette a generated one copies: `chosen` when the
	// project knows it (P4b's dialog choice), else `fallback`.
	const DungeonMap& PaletteDonor(const std::string& chosen, const DungeonMap& fallback);
	// The generator's saved presets (P4b), in the project's genpresets.cat.
	// Save returns the id actually used ("" on failure); a preset holds no seed.
	std::vector<std::string> GenPresetNames() const;
	bool LoadGenPreset(const std::string& name, generate::Params& params) const;
	std::string SaveGenPreset(std::string name, const generate::Params& params);
	bool DeleteGenPreset(const std::string& name);
	// P5, the play-test loop: close the generator and the editor and put the
	// party on `stem` at its start - a level transition, or, when it is the
	// level the party is already on (a reroll of the active one), a step to its
	// start. False, and nothing done, outside play or for an unknown level.
	bool PlayLevel(const std::string& stem);
	static std::vector<std::string> SplitKnobs(const std::string& line);
	void ShowGenReport(const std::string& levelStem);

	// --- the workflow, wired through (Game_Populate.cpp, tool-refinement Phase 7)
	// Loads style `id`'s shape knobs (styles.cat `knobs`, the settings line) into
	// `params` and names it as the style - the seed kept, so loading a style is
	// not a reroll. False, and `params` untouched, for a style the world lacks.
	bool LoadStyleKnobs(const std::string& id, generate::Params& params) const;
	// The tags a generated level is drawn by: a chosen `tag`, else the STYLE's,
	// else `fallback` (the viewed level's, or a dungeon's flavour tags).
	std::vector<std::string> TagsFor(const generate::Params& params, const Project& project,
									 std::vector<std::string> fallback) const;
	// POPULATE ONLY: monsters and loot for the VIEWED level from the knobs'
	// population half and pools (the style's monster list when one is named,
	// else the tags'), its shape untouched - however it was built. Replaces what
	// populating can make (monsters and loot of the pools' kinds); a key, a quest
	// item or a monster outside the pool is left where it stands. ONE undo step.
	// Returns the monsters placed (-1 on failure); the report holds the rest.
	int PopulateViewedLevel(const generate::Params& params);
	// The dungeon a [+] lands in, and the style it opens on: the dungeon's
	// `style` when this world has it, else the current (armed) one, else none.
	std::string DefaultStyleFor(const std::string& dungeonId) const;
	// After a create: the new level viewed in the BUILD stage of the palette,
	// with `style` armed for the shape brushes (Phase 7's "lands you in Build").
	void LandInBuild(const std::string& stem, const std::string& style);

	// The Level dialog's inline rename: validates (unique stem), drives
	// DungeonWorld::RenameLevel (files, stashes, stair dests), then updates
	// the manifest and the map view's browse snapshot. False = refused.
	// `why`, when given, receives the refusal - each rule its own sentence.
	bool RenameLevel(const std::string& oldStem, const std::string& newStem,
					 std::string* why = nullptr);

	// Persists a monster type's edited animation config (the right-click dialog's
	// Save): rewrites the `states` + `anim_<state>` rows of its monsters-catalog
	// entry (preserving every other field) and saves the project to disk.
	void WriteMonsterAnim(const MonsterConfigDialog::Config& cfg);
	// The rows that dialog owns (behaviour, threat, states, anim_*), written
	// into `entry` IN PLACE: a row already there keeps its position and its
	// comment, a new one is appended, and one the config no longer writes is
	// removed. Pure, so `catround` can check it on an entry of its own.
	static void ApplyMonsterConfig(CatalogEntry& entry, const MonsterConfigDialog::Config& cfg);

	// Starts a mid-game level transition (P6): swaps the world to `stem`, stages
	// its load behind the loading screen, and arrives at (x,z,facing) when done
	// (x<0 = the level's start cell). An unset facing is a WAY IN - a stair, a
	// doorway, the opening, `play` - and faces whatever stair stands on the
	// landing square (DungeonWorld::ArrivalFacingAt); only a save load and a pit
	// fall bring a facing of their own. `stashCurrent` saves the level being left
	// (its dynamic state) for a later return; pass false when leaving a throwaway
	// baseline (save load). Its unsaved editor work is stashed either way
	// (DungeonWorld::BeginLevelLoad; code-review C298).
	void BeginLevelTransition(const std::string& stem, int x, int z,
							  std::optional<Direction> facing, bool stashCurrent = true);
	// True when the frame now starting is one the steady-state allocation rule
	// covers (Core/AllocTrack). Stateful — it counts the warm-up.
	bool SteadyStateFrame();
	// The app states the steady-state rule covers at all: Playing, or the
	// character sheet over a level (and, under AllocTest's own switch, the idle
	// party creation page). SteadyStateFrame adds the rest (no console, no bake,
	// the warm-up), and Update disarms a frame that ends outside them.
	bool GuardedState() const;
	// Called when an overlay opens PART WAY THROUGH an already-armed frame:
	// disarms the guard for it and restarts the warm-up. See the definition.
	// Update also calls it for a frame that leaves the guarded states.
	void OverlayOpenedThisFrame();
	// Everything Update does per state, below the allocation guard's bookkeeping
	// (which has to see the frame's END, past all of this function's returns).
	void UpdateStates(float dt);
	// A frame's `dt` real seconds as WORLD seconds: times `timeScale` (the dev
	// console's; the `frames` command passes 1) and the rest multiplier. THE ONE
	// PLACE rest multiplies time (docs/health-and-healing.md); the world then
	// runs a long result in fixed ticks (DungeonWorld::kTick).
	float WorldDt(float dt, float timeScale) const;
	// THE ONE WORLD TICK (code-review C125): a frame's world update, the level
	// transition it raised followed (the map closed, the sheet left for play),
	// and the HUD's position and Rest button refreshed - for play, the map
	// overlay, the sheet and the open console alike. They were four copies, and
	// they had drifted: the console's never followed a transition, the sheet
	// froze under it, the overlay's skipped the Rest button. `acceptInput` false
	// hands the world kNoInput (the console, the sheet and the palette's filter
	// box own the keyboard). Does nothing unless WorldRuns.
	void TickWorld(const Input& input, float wdt, bool acceptInput);
	// THE ONE WORLD-FROZEN DECISION (code-review C78), asked by TickWorld and so
	// by every path that ticks: the world runs under a LEVEL - Playing, or the
	// sheet opened over one (the sheet is not a pause) - unless something holds
	// it: the exit prompt, an editor dialog (EditorModal), the editor's pause
	// button. The console owns the input, never the clock: over a paused editor
	// or an open dialog the world used to run, and over the sheet it froze.
	// WorldHeldBy names what holds it in one word (`worldclock` prints it), or
	// null when it runs.
	const char* WorldHeldBy();
	bool WorldRuns() { return WorldHeldBy() == nullptr; }
	// THE EDITOR'S MODAL DIALOGS, topmost first - one list for two questions.
	// Handed the input, the topmost open one takes the frame (its Update and the
	// preview it drives) and the answer says one did; handed null, it only says
	// whether one is up, which is what WorldRuns asks whoever owns the input. One
	// list, so a dialog cannot be routed and still let the world run under it.
	bool EditorModal(const Input* input, float dt);
	// The world's input while something else has the keyboard: nothing held.
	// Built at startup, ONE for the game (C125) - a function-local static is
	// constructed, and allocates, on its first use, and the sheet's first frame
	// is a guarded one.
	static const Input kNoInput;
	// Advances a running `alloctest` window and reports when it closes. The
	// window is measured in ARMED frames, so time spent loading, warming up or
	// with the console open does not spend it. Its first armed frame OPENS the
	// harness's window too (the tally restarts, a held autocast is released)
	// and its last logs the tally, so a script can prove what happened INSIDE.
	void UpdateAllocTest(float dt, bool steady);
	// `allocpoke once`: the one deliberate allocation. Its own function so the
	// stack the guard logs names it (tools\AllocTest.ps1 -SelfTest looks for it),
	// and NEVER inlined: SymFromAddr does not resolve inline frames, so an
	// optimised build that folded it into Update would name Update instead and
	// fail the self-test with the guard working.
	__declspec(noinline) void AllocPokeOnce();
	// One line per frame from the queued eval script, and the run's verdict when
	// it empties. Called from Update.
	void PumpEvalScript(float dt);
	// Read a script file into `out`, stripping comments and blank lines. Shared
	// by the root script and by `include`, so a fragment is parsed exactly as
	// the file that pulled it in.
	static bool ReadEvalLines(const std::string& path, std::vector<std::string>& out);
	// An `include` argument resolved against the ROOT SCRIPT's folder — a suite
	// names its presets by a short relative path, and resolving against the
	// process's working directory would tie every script to wherever the exe was
	// launched from (for this project, a build folder well away from the scripts).
	std::string ResolveEvalPath(std::string_view spec) const;

	// --- eval script state (docs/eval-harness.md) ---------------------------
	std::vector<std::string> m_evalLines; // the queued script, comments stripped
	size_t m_evalIndex = 0;               // next line to run
	int m_evalUnknown = 0;                // lines that matched no command
	// Lines whose command EXISTED and then declined to act — a spawn onto rock,
	// a tp into a wall, a step that hit its ceiling. Counted separately from
	// unknown because they are a different fault: the script is well formed and
	// the world is not what it assumed (docs/eval-audit.md F11).
	int m_evalRefused = 0;
	// `expect-refuse` lines that RAN WITHOUT refusing (or were refused only by
	// the title-screen gate): a probe of a rule that no longer holds. The mirror
	// of m_evalRefused, and a failure for the same reason (code-review C442).
	int m_evalUnrefused = 0;
	bool m_evalFinished = false;          // the LAST script emptied (vs timed out)
	std::string m_evalName;               // the script's filename, for the verdict
	std::string m_evalDir;                // its folder — what `include` resolves against
	// Scripts still to run in THIS process, in order. Popped by PumpEvalScript
	// when the current one empties, instead of quitting.
	std::vector<std::string> m_evalPending;
	int m_evalScripts = 0; // how many have run, for the summary
	int m_evalFailed = 0;  // how many came back FAIL — the exit code reads this
	// Wall-clock seconds ONE SCRIPT may take, re-armed by LoadEvalScript. Per
	// script rather than per run, because a batch of twenty is deliberately
	// long-lived and a whole-run budget would either strangle it or stop
	// catching the hang it exists for. A script waiting on a load that never
	// completes would otherwise hang a machine with no window worth looking at.
	static constexpr float kEvalScriptTimeout = 600.0f;
	float m_evalDeadline = kEvalScriptTimeout;
	bool RunLoadTasks();       // executes one task per frame; true when done
	// Dumps the finished queue's per-task time/allocation table to the log —
	// once as the last task lands, and again on demand (`loadstats`, which also
	// echoes it into the console scrollback).
	void LogLoadStats(bool echoToConsole = false);
	void LoadPortraitCatalog(); // portraits.cat (boot load task: party creation reads it)
	void LoadPortraits();      // every member's portrait (game load task)
	// Loads the portrait of every member whose portraitId differs from what is
	// loaded for that slot (draining the GPU first, since in-flight frames still
	// sample the old texture - the SRV recycling rule), and re-points every
	// member's `portrait`. Cheap when nothing changed, so every path that can
	// change an id (new game, load, SetPortrait) just calls it.
	void SyncPortraits();
	// Sets one member's portrait to a portraits.cat id. False (and nothing
	// changes) for a member out of range or an id the catalog does not list.
	bool SetPortrait(size_t member, const std::string& id);
	// Opens the portrait picker for a member (its pick -> SetPortrait, wired as
	// GameUI::onSetPortrait) and excuses this frame from the allocation guard.
	void OpenPortraitPicker(size_t member);
	void LoadHitSplats();      // hit-feedback splat icons (load task)
	void LoadItemIcons();      // rune + placeholder item cursor/inventory icons (load task)
	// The UI's PER-ITEM banks - icons, rune tablets, flames, weights, categories,
	// capacities, holdable and wear slots - cleared WHOLE and rebuilt from the
	// world's item kinds and the catalogs (code-review C330: the wear slots were
	// never cleared, so a world switch kept the last world's). LoadItemIcons runs
	// it, and so does every item type's create, save, rename and delete, which
	// rebuild the kinds the banks mirror.
	void RefreshItemBanks();

	// --- state transitions --------------------------------------------------
	// Puts the party in a level, staging a LOAD only when it is not the one
	// already held. True = a load is in flight and the caller is done.
	bool OpenInLevel(const std::string& level, int x, int z);
	void StartNewGame();
	// How the party came to be standing in a level, which decides what the log
	// says when play begins: a stair says nothing, a new game the opening and the
	// keys, a loaded game where it is.
	enum class Arrival { Level, NewGame, LoadedGame };
	// THE ONE TAIL of every way into play in a level (code-review C364): the log
	// the player arrives to, the HUD's position re-derived, and Playing. A new
	// game or a load that had to load its level first used to return early and
	// leave the level's completion to clear the log - so Start New Game from
	// crypt2 began in silence while the same click on crypt1 showed the opening.
	void BeginPlay(Arrival how);
	// Which world's saves the menus, `load` and Continue list (ListSaves): in a
	// `-project` run the world in hand (the default one while none is loaded) -
	// that flag is how a harness or a scenario opens a world, and a Continue must
	// not carry it off into another - else every world's (code-review C207: this
	// was latched at launch and went stale after a switch).
	std::string SaveListWorld() const;
	// What GAME itself holds from the game before - the item on the cursor
	// (code-review C296) and a Yes/No question still asked (C115) - put down, the
	// question unanswered (`why` names the ending in its log line). The world's
	// side is DungeonWorld::ResetForNewGame; this is the half it cannot reach.
	// Called wherever a game begins: StartNewGame, LoadGame (once its save is
	// read) and ResetForEval's recycle; the switch and the cold start reach
	// StartNewGame. UnloadWorld puts both down too, and the title the question.
	void ClearGameTransients(const char* why);
	// Resets the roster to a fresh party: `party` (a CREATED one, docs/party-
	// creation-plan.md), or the default four when null. The same SIZE is assigned
	// member by member, in place; a different size replaces the vector and
	// re-lays-out the HUD (GameUI::RebuildForRoster - safe here: every caller,
	// StartNewGame, LoadGame and ResetForEval, runs outside the HUD's widget
	// walk). The HUD/sheet widgets address members by (roster, index) and
	// re-resolve every frame, so neither case can dangle them. Each slot's
	// portrait follows its member's id (SyncPortraits reloads only a changed
	// one). Only the default four take the Settings palette's colours; a created
	// member keeps the one it was given.
	void ResetRoster(const std::vector<Character>* party = nullptr);
	// The default four (CreateDefaultParty) holding THIS world's rune tablets
	// (DungeonWorld::RuneItemFor, code-review C347). With no world there are no
	// tablets to hand out.
	std::vector<Character> DefaultParty() const;

	// --- party creation (Game_Party.cpp) ------------------------------------
	// One member from a spec: race (stats from PartyRules + the race's bases,
	// pace and resists), points, portrait, colour, boosted skills, starting items
	// placed where they go. Empty optional + `why` when the spec is refused.
	std::optional<Character> BuildMember(const party::MemberSpec& spec, std::string& why) const;
	// A whole party (1..4 members); false + `why` on the first refusal.
	bool BuildParty(const std::vector<party::MemberSpec>& specs, std::vector<Character>& out,
					std::string& why) const;
	// natureResists from the member's race (they are not saved, so a load
	// re-derives them; a premade or unknown race has none).
	void ApplyRaceResists(Character& member) const;
	// The party the NEXT new game starts with, set by party creation or the
	// `newparty` command and consumed by StartNewGame; empty = the default four.
	std::optional<std::vector<Character>> m_startParty;
	void RegisterPartyCreationCommands(); // newparty / roster / partypage (Game_Party.cpp)
	void RegisterPartyPageCommands();     // partypage (the page's dev twin)
	// THE PAGE (phase 3): the world `folder` (empty = the resident one, else the
	// default) is opened first - deferred a frame when it is another world, as a
	// switch always is - then GameUI's party creation page shows what it offers.
	void OpenPartyCreation(const std::string& folder);
	// What the open world offers the page: races, skills, starting items, the
	// default four as premade specs, and the build it previews with.
	PartyCreationData PartyCreationDataFor();
	// Start with the page's party (its Start button and `partypage start`).
	bool StartWithParty(const std::vector<party::MemberSpec>& specs, std::string& why);
	// Captures the live world + roster to a named slot under SaveDir. Requires
	// the dungeon to be loaded (m_gameLoaded); no-op otherwise.
	// False when nothing was written — no game loaded, inside a random
	// encounter, or the file could not be created. The CALLER has to say so:
	// a console that prints "saved" whatever happened is worse than silence,
	// because it is the only thing anyone checks.
	bool SaveGame(const std::string& name);
	// Loads a save file: rebuilds the level baseline, applies the save on top,
	// and enters Playing. Requires the dungeon already loaded (the deferred
	// first-load path is wired by the menu, step 2). Returns false on failure.
	bool LoadGame(const std::string& path);
	void OpenCharacterSheet(size_t index); // shows the page; the world runs on
	// Out of this game to the title, the dungeon left resident. ONE trip for
	// both ways it happens (a party wipe, and the pause menu's Return to Main
	// Menu), and it LOGS which, because a wipe used to reach only the HUD: with
	// the console open the world still simulates, and an idle party killed there
	// read in dungeon.log as the game silently refusing in-game commands.
	void ReturnToTitle(const char* why);
	void SetQuality(Quality quality);      // persists + hot-swaps the world
	// Centered "working..." box, drawn on the frame a blocking operation is
	// about to stall on (see m_pendingQuality / m_geomNoticeLatched). Shared so
	// every such notice reads the same.
	void DrawBusyNotice(const std::string& text, float dw, float dh);
	// Applies m_settings' display mode (windowed/borderless/exclusive + monitor
	// + resolution) to the window and swapchain in place, at boot.
	void ApplyDisplaySettings();
	// Applies a display choice in place - the Video tab's Apply for anything
	// but an adapter change, and the boot's through ApplyDisplaySettings. The
	// monitor is looked up by its DEVICE NAME in the one display list, every
	// monitor whichever GPU it hangs off (code-review C198/C199); one no longer
	// there is the monitor the window is on. A Windowed one is centred in the
	// chosen monitor's WORK AREA and shrunk to fit it (Window::SetWindowed,
	// C196); an Exclusive one is not taken at all by a hidden (`-headless`)
	// window, which must never hold a monitor (C391). Returns whether it took:
	// the caller saves a choice only then (C199 - a refused Exclusive used to
	// stay saved, and come back at every boot).
	bool ApplyDisplay(const DisplayChoice& choice);
	// The `video status` readout (Game_DevCommands): what the Video tab has
	// STAGED, what is RUNNING - read off the device and the window, never the
	// settings - and what is SAVED, a line each, plus a line per monitor.
	void PrintVideoStatus();
	// Resolves the UI material the PLACE asks for - the active level's
	// `uistone` record, else its dungeon's `ui_stone`, else none - and hands it
	// to GameUI, which applies it when the player's Material setting follows
	// the place. Run from UpdateStates when the level or the edit revision
	// moves, and after a Level settings save.
	void RefreshPlaceStone();
	// Relaunches the executable and flags this instance to quit: a fresh process
	// binds the newly chosen adapter, the only way to switch GPUs (the Video
	// tab's Apply; dev `video restart`). Nothing else relaunches - a world switch
	// is in-process. The child is this exe by its own path, with this run's
	// arguments (`-project` keeps its world) less any `-eval` scripts, plus
	// `-relaunched <this pid>`, which makes it wait for this process to exit and
	// then APPEND to the log they share (code-review C398). Quits only when the
	// child started; false = it did not, and this run goes on as it was.
	bool RestartApp();
	// The new-game world list's pick: a new game now if it is the world
	// running, else a switch to it (SwitchWorld) and a new game there.
	void StartNewGameIn(const std::string& folder);
	// Loads the language file - a script's (m_scriptLanguage), else the
	// settings' - falling back to English when it is missing; rebuild=true also
	// re-creates every UI page in the new language. The language dropdown only
	// records m_pendingLanguage - Update applies it at the top of the next frame,
	// after the dropdown's callback has fully unwound (the rebuild destroys the
	// dropdown).
	void ApplyLanguage(bool rebuild);
	// The language drawn now: a script's switch, else the settings' own.
	const std::string& ActiveLanguage() const {
		return m_scriptLanguage.empty() ? m_settings.language : m_scriptLanguage;
	}
	// Per-frame adaptive thread governor (see the definition in Game.cpp). No-op
	// unless `governor auto` is enabled.
	void UpdateGovernor(float dt);
	// Feeds the slowest member's moveSpeed into the Party as its pace
	// multiplier; call whenever the roster's stats are (re)filled.
	void ApplyPartySpeed();
	// Put the game where `newgame` would, WITHOUT the level load — the eval
	// harness's world recycling (docs/eval-harness.md). Falls back to a real new
	// game when nothing is loaded yet, so a batch's FIRST script pays the twelve
	// seconds and none of the rest do. A party left on ANOTHER level (a `goto`, a
	// world-map trip, an encounter) goes back to the harness level by a real
	// staged load, as a new game would take it there (code-review C300).
	// `detail` is the refusal's reason, or which level a switch came from.
	enum class EvalReset { Refused, Loaded, Recycled, Switched };
	EvalReset ResetForEval(std::string& detail);
	// The level a harness new game opens in: the manifest's eval_level, else the
	// first level - StartNewGame's harness branch and `reset` both ask this.
	std::string HarnessLevel() const;
	// Pushes the settings' per-slot identity colors (member_<n>=, Settings →
	// UI pickers) onto the roster; call whenever the roster is (re)filled.
	// The pickers also write the live roster directly while playing.
	void ApplyMemberColors();

	Window& m_window;
	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	gfx::SpriteBatch& m_spriteBatch;
	audio::AudioEngine& m_audio;
	// HDR scene target + bloom + ACES composite; Render brackets the world's
	// scene pass with BeginScene/Resolve (see DungeonWorld::RenderScene).
	gfx::PostProcess m_postProcess;

	// --- app state -------------------------------------------------------------
	AppState m_state = AppState::Loading;
	// Where Esc/Resume goes back TO. Pause and the character sheet can now be
	// opened from two places — inside a dungeon and out on the world map — and
	// an unconditional "resume means Playing" would quietly teleport a
	// travelling party into whatever level was last loaded.
	AppState m_resumeState = AppState::Playing;
	// The state whose PICTURE is behind the current one. The pause menu and the
	// sheet are drawn over whatever they were opened from, so from the world map
	// they sit over the world map - not over the parked dungeon that stays
	// resident under it, which Render used to draw (shadows and all) because it
	// chose the 3D pass from m_state alone (code-review C365). Every other state
	// is its own backdrop. Render's 3D gate and its 2D switch both ask this.
	AppState BackdropState() const {
		return m_state == AppState::Paused || m_state == AppState::CharacterSheet
				   ? m_resumeState
				   : m_state;
	}
	// What the last RENDERED frame drew behind the state's own 2D: the backdrop
	// whose picture it drew - set in the case of Render's switch that DREW it,
	// so a case that drew nothing (or the world map's with no map loaded)
	// leaves it empty and reads as "nothing", not as whatever backdrop the
	// switch was asked for - and whether the 3D scene pass ran. The `backdrop`
	// command prints them, and InGameTest reads that (a headless run renders
	// nothing).
	std::optional<AppState> m_drawnBackdrop;
	bool m_drewScene = false;
	u32 m_backdropFrame = 0; // the frame (1-based) they describe; 0 = none rendered
	// The frame (1-based, as m_backdropFrame) the world view last DREW, as the
	// travel screen or the map's world page; 0 = never. `worldview` reports the
	// view's LastDrawn() only when it is the last rendered frame's.
	u32 m_worldViewFrame = 0;
	LoadQueue m_loadQueue;
	bool m_gameLoaded = false; // the loaded world's game assets are resident
	// The world a start with nothing else to go on opens: `-project`, else
	// settings.ini's last world, else dungeon-demo (ChooseProjectFolder). The
	// harness's `reset` on a cold start and a one-world Start New Game use it.
	std::string m_defaultWorld;
	// From the command line, read once in the constructor. A `-project` run has
	// its world chosen (the new-game list does not ask).
	bool m_worldFromCommandLine = false;
	// A WORLD SWITCH ASKED FOR MID-FRAME, applied at the top of the next Update
	// (ApplyPendingWorld). A switch destroys the world, and the asks come from
	// inside widget callbacks and dialog updates whose callers carry on after
	// they return — the m_pendingLanguage rule, for the same reason. With a
	// save path it continues that save; without, it starts a new game.
	struct PendingWorld {
		std::string folder;
		std::string savePath;
		// Opens the party creation page once the world is in, instead of
		// starting a game (OpenPartyCreation).
		bool partyPage = false;
	};
	std::optional<PendingWorld> m_pendingWorld;
	// The landing page's Editor entry asked for the editor, paused, once the
	// new game it started arrives (OpenEditorOnArrival).
	bool m_editorOnArrival = false;
	void OpenEditorOnArrival();
	void ApplyPendingWorld();
	u32 m_framesRendered = 0;
	// Consecutive frames that have been quietly Playing - the allocation guard's
	// warm-up counter (see SteadyStateFrame) - and the quiet frames it must count
	// before a frame is armed.
	u32 m_steadyFrames = 0;
	static constexpr u32 kGuardWarmupFrames = 120;
	// How long the last quiet run lasted, kept when it ends (EndQuietRun). A
	// command only runs with the console open or a script running, neither of
	// them quiet, so `allocguard` reports THIS - the run the console or script
	// ended - where it used to print the current count, always 0 (code-review
	// C450).
	u32 m_lastQuietRun = 0;
	void EndQuietRun() {
		if (m_steadyFrames > 0) m_lastQuietRun = m_steadyFrames;
		m_steadyFrames = 0;
	}
	// `allocguard partypage on`: the idle party creation page counts as a
	// guarded state (GuardedState) - AllocTest.ps1 -PartyPage's switch, and
	// nothing else's. Off by default, not saved.
	bool m_guardPartyPage = false;
	// `alloctest`: an armed-seconds budget, a wall-clock deadline so a test that
	// never reaches steady state reports SKIP instead of hanging, and the guard
	// stats at the window's start (the result is their delta).
	float m_allocTestRemaining = 0.0f;
	float m_allocTestDeadline = 0.0f;
	u32 m_allocTestFrames = 0;
	// Armed window frames that LEFT the guarded states (Esc to the pause menu, a
	// stair load) and were disarmed for it - the evidence AllocTest.ps1 -Pause
	// reads that a transition happened inside the window at all.
	u32 m_allocTestTransitions = 0;
	// Armed window frames that OPENED a Yes/No prompt over play (an exit stair's
	// "Leave?") and were disarmed for it - AllocTest.ps1 -Exit's evidence that
	// the prompt opened inside the window (code-review C210).
	u32 m_allocTestPrompts = 0;
	// What -Exit does around its prompt, counted only in MEASURED frames - armed
	// to the end of Update (alloc::FrameArmed) and inside the window: presses of
	// the log's Help button (GameUI::HelpPresses) and pit falls begun by a step
	// (Harness::fallsBegun), each differenced across the frame in Update. A count
	// since launch, or since the window opened, also took the warm-up's frames
	// and the frames after a prompt, which the guard does not measure - a Help
	// click in the warm-up passed with its line unchecked (code-review C210, C217).
	u32 m_allocTestHelps = 0;
	u32 m_allocTestFalls = 0;
	// AllocTest -Lever's, counted the same way: lever presses (Harness::
	// leverPresses) and the presses that flipped a niche (Harness::nicheFlips) -
	// one wired to no niche and a reveal, each checked (code-review C211).
	u32 m_allocTestLevers = 0;
	u32 m_allocTestNiches = 0;
	// AllocTest -Sheet -AllSpells', counted the same way: the most Known Spells
	// rows a bake REACHED FOR THE FIRST TIME in a measured frame - on the sheet,
	// and on a party-window card (CharacterSheet::SpellRowsMost rising across
	// the frame). A list's rows grow only on its first bake that long, so a
	// count since setup also took a first bake in the warm-up, which the guard
	// never saw (code-review C220).
	size_t m_allocTestSheetSpells = 0;
	size_t m_allocTestCardSpells = 0;
	// AllocTest -DisplayChange's, counted the same way: re-reads of the display
	// list (GameUI::DisplayRefreshes) in a measured frame - its evidence that a
	// WM_DISPLAYCHANGE's re-read and the Settings page rebuilt from it were
	// checked (code-review batch 69).
	int m_allocTestDisplays = 0;
	// The party's Act count when the window opened (Party::ActCount): the
	// verdict's moves= is the difference, -Walk's evidence that it moved.
	unsigned m_allocTestActsAt = 0;
	// ui::inspect::ArmedChainDraws when the window opened: the verdict's uitree=
	// is the difference, AllocTest -UiTree's evidence that the `uitree`
	// breadcrumb was built in measured frames (code-review C223).
	u64 m_allocTestChainsAt = 0;
	// Each member's effect count when the window opened, and the most it reached
	// in an armed frame of it: the verdict's effectsrose= is the difference per
	// member, AllocTest -Effects' evidence that the party bar's effect strips
	// GREW inside the window (code-review C219, C228). Sampled at the top of each
	// armed frame, so a rise the world made in the last one is read at the verdict.
	std::array<size_t, party::kMaxMembers> m_allocTestFxAt{};
	std::array<size_t, party::kMaxMembers> m_allocTestFxPeak{};
	alloc::GuardStats m_allocTestStart;
	// `allocpoke`: allocate deliberately, every frame, for this many seconds.
	// It exists so the guard and tools\AllocTest.ps1 can be shown to FAIL — a
	// regression test that cannot fail proves nothing. m_pokeScratch holds the
	// result so the allocation cannot be optimized away.
	float m_allocPokeRemaining = 0.0f;
	// `allocpoke once`: ONE allocation, on the first armed frame after a disarm -
	// the frame whose stacks went uncaptured (code-review C214). An every-frame
	// poke could not show that: its second frame captured, and hid the first.
	bool m_allocPokeOnce = false;
	std::unique_ptr<u32> m_pokeScratch;
	// `inputpoke`: throw the typed text away unread for this many seconds - the
	// loss tools\TypingTest.ps1 -SelfTest must be seen to catch - and then on
	// to the end of the line it closed in (m_inputPokeMidLine), so no fragment
	// survives to run as a command.
	float m_inputPokeRemaining = 0.0f;
	bool m_inputPokeMidLine = false;
	// `crashpoke uiclip`: the scratch tree whose scroll area threw mid-walk, kept
	// up and walked every frame so a later click on its button can be seen to
	// land (Game/ClipPoke.h). Null until the poke.
	std::unique_ptr<ClipThrowPoke> m_clipPoke;
	// Frame count when the current loading state was entered; tasks only run
	// once its screen has been presented at least once.
	u32 m_stateFrameMark = 0;
	bool m_quitRequested = false;
	bool m_quitOnceLoaded = false; // see QuitOnceLoaded
	float m_time = 0.0f;
	// Dev console `timescale`: multiplies the world's dt (1 = normal, 0 = freeze).
	float m_timeScale = 1.0f;
	// Language code picked in Settings this frame, applied (strings reloaded,
	// UI rebuilt) at the top of the next Update; empty = no change pending.
	std::string m_pendingLanguage;
	// That pick came from a SCRIPT (an eval's `lang`), not the player: it lands
	// in m_scriptLanguage and is never saved.
	bool m_pendingLanguageScripted = false;
	// The language a script switched to, drawn instead of the settings' own and
	// NEVER written to settings.ini; empty = the settings' own. The harnesses run
	// the build Michael plays and share its settings.ini, and a script that died
	// between its `lang de` and `lang en` (the crash partypage.eval exists to
	// catch) left his game starting in German.
	std::string m_scriptLanguage;
	// Quality tier picked in Settings (or by the dev `quality` command) this
	// frame, applied at the top of the next Update. Deferred for a DIFFERENT
	// reason than the language: the swap BLOCKS for seconds (every surface
	// texture reloads at the new resolution, and Ultra's 4k sets are the slow
	// case), and a frame that never presents reads as a hang. Latching one
	// frame gets the "applying" notice on screen first, so the stall freezes on
	// it — the same trick as m_geomNoticeLatched. The optional IS the latch.
	std::optional<Quality> m_pendingQuality;
	// Save chosen from the landing page before the dungeon was resident: the
	// heavy load runs first (LoadingGame), then this save is applied instead of
	// starting fresh. Empty = the load should StartNewGame as usual.
	std::string m_pendingLoadPath;

	// Pending level-transition arrival (set by BeginLevelTransition, applied when
	// the LoadingLevel queue finishes): the cell + facing the party enters at.
	int m_pendingLevelX = 0, m_pendingLevelZ = 0;
	std::optional<Direction> m_pendingLevelFacing; // unset = ArrivalFacingAt
	// Free-look offset to re-layer once the arriving party is placed. Orthogonal
	// for ordinary transitions (stairs/new game); a save load on a DIFFERENT level
	// seeds it from the save so the exact look angle survives the level rebuild.
	float m_pendingLookYaw = 0.0f, m_pendingLookPitch = 0.0f;
	bool m_pendingLooking = false;
	// A save made on the world map is loading onto a different level: when the
	// load lands, ResumeOnWorldMap(m_pendingWorldPark) instead of playing on.
	bool m_pendingWorldMap = false, m_pendingWorldPark = false;
	// What the level load in flight is FOR, so its completion begins play the way
	// that path would have (BeginPlay). Set by StartNewGame / LoadGame after
	// they stage the load; a plain transition (BeginLevelTransition) resets it.
	Arrival m_pendingArrival = Arrival::Level;

	// --- modules (construction order matters: settings load first, the world
	// and UI reference settings/sounds/characters) -------------------------------
	GameSettings m_settings;
	// The active project: content catalogs + levels (assets/projects/<name>).
	// Loaded before the world (which reads it for level paths and catalogs);
	// the editor will read and write it.
	Project m_project;
	// The shared STYLE LIBRARY (assets/library, Game/StyleLibrary.h): what the
	// palette's Styles section offers to add. Read with each world.
	StyleLibrary m_library;
	// The overworld above the dungeons (docs/world-map.md), loaded from the
	// project once at construction. EMPTY IS LEGAL: a project need not have a
	// world authored yet, so everything that reads this must cope with nullopt
	// rather than assume it. Deliberately NOT inside DungeonWorld, which is the
	// simulation of one LEVEL — the world sits a tier above it.
	std::optional<WorldMap> m_worldMap;
	// The DYNAMIC half: where the party is in the world, elapsed hours, what it
	// has discovered, the global flags. Beside the map rather than inside
	// DungeonWorld, for the same reason the map is (docs/world-map.md). Saved
	// whole as the save's global tier — it IS SaveData::world's type.
	WorldState m_worldState;
	// The encounter knobs, dev-facing rather than authored: `encounters` on the
	// console. The RATE multiplies difficulty x hours into a probability, so
	// zero is not the way to turn them off — a separate flag is, because a rate
	// of zero and "switched off" should not be the same state to read back.
	float m_encounterRate = 0.25f;
	bool m_encountersOff = false;
	SoundBank m_sounds;
	// THE EVAL HARNESS ASKS FOR A LEVEL (Game_Eval.cpp). `reset` means "where a
	// new game would leave it", and since P4 that is the WORLD MAP, where
	// nothing simulates - ten combat suites would have gone on printing
	// plausible readouts about a party standing in a field
	// (docs/world-map.md "Obligations"). So the harness states what it wants
	// and StartNewGame honours it, instead of the two silently tracking each
	// other. Never set outside the harness.
	bool m_harnessOpensInLevel = false;
	// Party roster, 1..party::kMaxMembers members. The HUD and sheet widgets
	// hold no pointer into it: they address it by (vector, index) and
	// re-resolve every frame (PartyHudTypes.h RosterMember), and the world
	// borrows the VECTOR (SetRoster). So ResetRoster assigns members in place
	// when the size holds and replaces the vector when it does not, then calls
	// GameUI::RebuildForRoster to re-lay-out the per-member widgets.
	std::vector<Character> m_characters;
	// Portrait textures, parallel to m_characters (entries may be null when the
	// image is missing; Character::portrait points in here), and the id each
	// slot's texture was loaded for - nullopt = never loaded, so a missing image
	// is tried once rather than on every sync.
	std::vector<std::unique_ptr<gfx::Texture>> m_portraitTextures;
	std::vector<std::optional<std::string>> m_portraitIds;
	// assets/portraits/portraits.cat: every portrait that ships, with its tags.
	Catalog m_portraitCatalog;
	// Hit-feedback splat icons (small/medium/hard) + the pointer struct the
	// party bar reads. The struct address is stable, handed to GameUI once at
	// construction; LoadHitSplats fills it in during the staged load.
	std::unique_ptr<gfx::Texture> m_hitSplatTextures[3];
	HitSplatIcons m_hitSplats;

	// Item icons for the held cursor / hand slots / inventory. Game owns the
	// textures; the bank (catalog id → texture) is handed to GameUI once, address
	// stable, filled by RefreshItemBanks: a rune tablet is its baked tablet (else
	// its symbol's flat PNG), a model item its baked thumbnail, the rest a
	// generated solid-tint placeholder (m_itemIconPlaceholders).
	std::array<std::unique_ptr<gfx::Texture>, kSymbolCount> m_runeIconTextures;
	// The Magic window's glowing runes: glyph + halo per symbol (BuildRuneGlow.py).
	std::array<std::unique_ptr<gfx::Texture>, kSymbolCount> m_runeGlyphTextures;
	std::array<std::unique_ptr<gfx::Texture>, kSymbolCount> m_runeGlowTextures;
	// A burning torch's flame over its icon (ItemIconBank). The glow under it is
	// the shared glow_radial (AssetUtil GlowIcon, code-review C330 - it was
	// loaded here AND by GameUI, a second SRV slot for one image).
	std::unique_ptr<gfx::Texture> m_flameTexture;
	std::vector<std::unique_ptr<gfx::Texture>> m_itemIconPlaceholders;
	ItemIconBank m_itemIcons;
	ItemWeightBank m_itemWeights; // catalog id → carry weight (kg), for the sheet
	ItemCategoryBank m_itemCategories; // catalog id → category, for the sheet
	// Equipment-slot outline silhouettes (slot type → texture), drawn as the
	// ghost behind an empty doll slot. Filled by LoadItemIcons from slot_*.png.
	std::vector<std::unique_ptr<gfx::Texture>> m_slotIconTextures;
	ItemIconBank m_slotIcons;
	// Hand-use pictures (verb → texture): what an empty HUD hand SET to that
	// verb shows (punch, kick). Filled by LoadItemIcons from every
	// ui/use_<verb>.png (tools/gen_use_icons.ps1), so a verb gains a picture by
	// gaining a file.
	std::vector<std::unique_ptr<gfx::Texture>> m_useIconTextures;
	ItemIconBank m_useIcons;
	// The item currently carried on the cursor (its catalog id), or empty. Set by
	// clicking a floor tablet; cleared by dropping it (world / portrait / hand /
	// inventory). GameUI reads the address to draw the cursor icon.
	HeldItem m_heldItem;
	// Editor undo/redo defers the surface rebake while the full-screen editor
	// hides the scene (DungeonWorld::GeometryDirty). On leaving editor mode
	// this latches ONE frame so Render shows the centered "rebuilding
	// geometry" notice, then the next Update runs the blocking FlushGeometry —
	// the stall freezes on the notice frame.
	bool m_geomNoticeLatched = false;

	// Right-mouse free-look drag: the previous cursor position, so each frame's
	// motion becomes a yaw/pitch delta. Valid only while m_looking (RMB held).
	bool m_looking = false;
	float m_lookPrevX = 0.0f;
	float m_lookPrevY = 0.0f;
	// Where the right button went down and how far the pointer has strayed since:
	// a release still within kClickSlop is a CLICK, not a look, and opens the
	// details of the floor item under the press (docs/ui-updates-plan.md P4).
	float m_lookPressX = 0.0f;
	float m_lookPressY = 0.0f;
	float m_lookStray = 0.0f;

	// The engine's worker threads (Core/ThreadManager.h). Declared before m_world
	// so it outlives every subsystem that spawns workers on it — m_world's AI is
	// the first client. The dev-console THREADS panel inspects/controls it.
	threads::Manager m_threads;
	// Adaptive governor (dev `governor auto`): eases all worker cadences when the
	// frame runs over m_governorTargetMs, restores them when it runs under.
	bool m_governorAuto = false;
	float m_governorScale = 1.0f;
	float m_governorTargetMs = 1000.0f / 60.0f;
	// THE WORLD, built when a game starts and destroyed when another world is
	// chosen (docs/world-on-demand.md) — null on the title screen until then.
	// Rebuilt rather than reset: a new object has none of the old world's
	// caches to forget, and a world made from another shares its catalog ids.
	std::unique_ptr<DungeonWorld> m_world;
	// What RefreshPlaceStone last resolved for: the level and the world's edit
	// revision. The revision starts impossible so the first frame resolves.
	std::string m_placeStoneLevel;
	u64 m_placeStoneRev = ~0ull;
	// Typefaces, addressed by role (UI/FontLibrary.h). Declared BEFORE m_ui
	// because every UIContext there borrows a Font from it, and configured from
	// assets/fonts/fonts.cat before those contexts first resolve a role — see
	// MakeFontLibrary in Game.cpp.
	ui::FontLibrary m_fonts;
	// A LANGUAGE'S OWN FACES (docs/fonts.md Phase 6): a .lang file may name a
	// face for a role (`lang.font.<role>` = a file under assets/, plus an optional
	// `lang.font.<role>.scale`) when the shipped one lacks its script - Russian
	// names a Cyrillic face for Script, since IM Fell English has none. Each
	// overridden role keeps the face it had (fonts.cat's, or an audition's), so a
	// switch to a language without one puts it back, and `font save` writes that
	// face, never the language's. Empty = the role is not overridden.
	std::array<std::optional<ui::FaceSpec>, ui::kFontRoleCount> m_langFontBase;
	// Lays the loaded language's faces over the roles (ApplyLanguage calls it).
	void ApplyLanguageFonts();
	// The face a role has apart from any language override (`font save`).
	const ui::FaceSpec& BaseFace(ui::FontRole role) const;
	GameUI m_ui;
	// The overworld screen (docs/world-map.md). Its own class, not a third
	// MapView mode: MapView is built around a DungeonMap and its docks,
	// palette and brushes, none of which mean anything on the world.
	WorldMapView m_worldMapView;
	// A paint STROKE is open: the first changed cell began an undo step and the
	// mouse release closes it, so one drag is one Ctrl+Z.
	bool m_worldStroke = false;
	// Map/editor overlay (toggle with `M` while playing). Like the console it
	// does NOT pause the world - the party keeps walking; the overlay only
	// claims the mouse for panning/zooming/editing.
	MapView m_mapView;
	// The Editor-mode brush palette + tools, driven by m_mapView while it is in
	// Editor mode (see MapEditor.h). Declared after m_mapView so it can take a
	// reference to it in the ctor init list.
	MapEditor m_mapEditor;
	// Fullscreen dev overlay (toggle with `~`); does not pause the world.
	DevConsole m_console;

	// Editor 3D model preview (P4a). The offscreen render target plus the model
	// currently shown in it (dev `preview <model>`); a null mesh = inactive. The
	// model spins by m_previewOrbit each frame. P4b embeds this in the asset
	// dialog; for now it draws full-screen via the dev command.
	gfx::ModelPreview m_modelPreview;
	assets::ModelData m_previewModel;
	std::unique_ptr<gfx::Mesh> m_previewMesh;
	gfx::MaterialParams m_previewMaterial;
	float m_previewOrbit = 0.0f;

	// Asset-creation dialog (P4b), opened from the palette's "+ New".
	AssetDialog m_assetDialog;
	// Monster-type animation config dialog, opened by right-clicking a monster in
	// the editor palette (states + per-state clip table).
	MonsterConfigDialog m_monsterDialog;
	// Combat-tuning dialog (the balance.cat/attacks.cat front-end), opened by
	// the editor map's Balance header button.
	BalanceDialog m_balanceDialog;
	// Per-level atmosphere dialog (the .map `atmosphere` record front-end),
	// opened by the editor toolbar's Level button for the VIEWED level.
	LevelSettingsDialog m_levelSettingsDialog;
	// The WORLD's own settings (W4): its start cell, the game's opening, the
	// harness level, its areas and its doorways. Opened by the world screen's
	// toolbar, and by a right-click on a doorway (which opens it ON that one).
	WorldSettingsDialog m_worldSettingsDialog;
	// The worlds BESIDE this one (W8): list, open (in the process, SwitchWorld),
	// create. The world toolbar's leftmost disc; `worlds` is the same thing typed.
	WorldsDialog m_worldsDialog;
	// Making a world (P4): blank, this world whole, or one level. Opened from a
	// disc on both editor toolbars and the Worlds dialog's "New world..."; it
	// sits ABOVE the Worlds dialog when opened from it.
	NewWorldDialog m_newWorldDialog;
	ValidateDialog m_validateDialog;
	GenerateDialog m_generateDialog;
	generate::Report m_lastGenReport; // the most recent generate's, for the readouts
	// Per-TYPE catalog editor, opened by right-clicking any palette row: a form
	// rendered from CatalogSchema, so it serves every category. Save writes the
	// .cat (and re-bakes the worn meshes when a surface's look changed).
	TypeEditorDialog m_typeDialog;
	// The pool browser behind every `texture` / `model` field — modal OVER the
	// type editor, since that is what opens it. What it picks goes back through
	// m_pickApply (the field's own setter), so the picker knows nothing about
	// catalogs.
	AssetPicker m_assetPicker;
	std::function<void(const std::string&)> m_pickApply;
	// Per-instance entity inspector, opened by Select-clicking a placed monster.
	EntityInspector m_entityInspector;
	// Per-instance fixture inspector, opened by Select-clicking a wall torch/sconce.
	FixtureInspector m_fixtureInspector;
	// Per-instance editor for placed items and decorations (facing, ...).
	PropInspector m_propInspector;
	// Per-instance door editor (open/closed + required key + name).
	DoorInspector m_doorInspector;
	// Per-instance button editor (target door wiring).
	ButtonInspector m_buttonInspector;
	// Per-instance wall-niche editor (shape / secret start / name).
	NicheInspector m_nicheInspector;
	// Per-instance stair/pit editor (facing, arrival facing, go there, delete).
	StairInspector m_stairInspector;
	// In-flight projectile details (read-only + dismiss); transient content.
	ProjectileInspector m_projectileInspector;
	// Chooser shown when a Select-clicked cell holds >1 inspectable object; picking a
	// row opens the matching inspector. One target per object at the clicked cell.
	InspectPicker m_inspectPicker;
	struct InspectTarget {
		enum class Kind {
			Monster, Sconce, Brazier, Door, Button, Decoration, Item, Projectile, Niche,
			Stair
		} kind = Kind::Monster;
		u32 runtimeId = 0;        // Monster / Projectile: the stable id
		Direction wall = Direction::North; // Sconce / Niche: the wall it is on
		int handle = 0;           // Decoration: list index; Item: stable entity id
		int nicheX = 0, nicheZ = 0; // Niche: its floor cell (with `wall` = its face)
		std::string type;         // catalog display name (Decoration/Item title)
	};
	std::vector<InspectTarget> m_inspectTargets; // objects at the last inspected cell
	int m_inspectCellX = 0, m_inspectCellZ = 0;  // that cell (for fixture configs)
	void OpenInspectorFor(const InspectTarget& t); // routes to the right dialog
	// Shared tail of the sconce/brazier cases: preview pane + flame overlay + Open.
	void OpenFixtureInspector(const FixtureInspector::Config& fc,
							  const std::vector<Direction>& walls,
							  const DungeonWorld::FixturePreviewData& sp);
	// Every per-instance dialog, the projectile card and the chooser, closed with
	// NO revert, and the chooser's targets dropped: what they edit is about to be
	// rebuilt (a respawn hands every monster a new id and frees a reloaded kind's
	// mesh, which their previews borrow) or is going (an unload). Code-review
	// C232/C233.
	void CloseInspectors();
	// The monster dialog's preview borrows its kind's mesh, skeleton and clips:
	// forgotten whenever that kind may go, and rebuilt by the next Update.
	void ForgetMonsterPreview();
	// THE PATROL ROUTE'S KEYS, while one is being laid: Backspace takes the last
	// waypoint back, Enter or Esc finishes it - and the inspector reopens on the
	// monster the route BELONGS to, looked up afresh by its runtimeId through
	// OpenInspectorFor, which declines when that monster is gone (code-review
	// C80, C232: it used to re-pass a cached config and preview, so it reopened
	// on whichever monster was inspected last, or drew a freed mesh). ONLY ON THE
	// EDITOR MAP (C233): the player's map has no route to finish. True when the
	// key was the route's; the map's Update and `editor route key` both ask.
	enum class RouteKey { Back, Finish };
	bool RouteKeyPressed(RouteKey key);
	// `editor inspector ...` / `editor route ...` (Game_Inspect.cpp): the open
	// inspector's clicks and keys, and the route's grid clicks and keys, for a
	// harness - each the call its control makes.
	void InspectorCommand(const std::vector<std::string>& args);
	void RouteCommand(const std::vector<std::string>& args);
	// The chooser's row for the first target of a KIND ("door", "monster", ...)
	// on the square it lists - `editor inspect <x> <z> <kind>`, which a click on
	// that row would be. False when the kind is unknown or the square has none.
	bool PickInspectTarget(const std::string& kind);
	// `editor levelsettings|newasset|levellist|arm ...` (Game_Inspect.cpp): the
	// editor's other dialogs and its level drop-down opened and pressed as their
	// controls are, and where each stands - what a check of the Esc ladders
	// (code-review C81, `presskey esc`) reads between keys.
	void EditorDialogCommand(const std::vector<std::string>& args);
	// Live animation preview for the monster dialog (and a monster inspector's,
	// which shares the Animator): an Animator over the selected type's
	// (borrowed) skeleton+clips, rendered into m_modelPreview and blitted into the
	// dialog's preview pane. m_previewType/Clip track what it's currently playing so
	// a change re-Plays; the mesh/material/scale are cached from MonsterPreviewFor.
	anim::Animator m_previewAnim;
	std::string m_previewType, m_previewClip;
	const gfx::Mesh* m_previewMonMesh = nullptr;
	gfx::MaterialParams m_previewMonMat;
	// Every drawable piece of the previewed type (one per primitive for a
	// multi-material rig); the Render pass draws these with the palette.
	std::vector<gfx::PreviewSubmesh> m_previewMonSubs;
	float m_previewMonScale = 1.0f;
	float m_previewMonYaw = 0.0f; // modelyaw fixup, so the preview faces like in-world
	Vec3 m_previewMonPivot{};     // its rig root's rest point, so it is centred in the pane

	// Live 3D preview for the per-INSTANCE edit dialogs. Each dialog OWNS a
	// PreviewSpec (built here in OpenInspectorFor, from the world's meshes) and the
	// render/update loop reads it generically via ActiveInstanceInspector()->Preview()
	// — no per-type switch. m_previewAnim (skinned monster) and m_previewFire (torch)
	// hold the per-frame simulation state the spec drives.
	// Every per-instance dialog, in the order they take input and draw. ONE list,
	// so the modal chain in Update, the draw pass in Render and
	// ActiveInstanceInspector cannot disagree about which dialogs exist — adding
	// an inspector is one entry here rather than three hand-written `if` blocks
	// that must be kept in step. They share a base (InstanceInspector), so
	// nothing in those loops needs the concrete type.
	std::array<InstanceInspector*, 7> InstanceInspectors();
	InstanceInspector* ActiveInstanceInspector(); // the open per-instance dialog, or null
	// The project's flags as the inspectors' dropdowns list them: (id, "name
	// (scope)").
	FlagChoices FlagChoiceList() const;
	gfx::ParticleBatch m_previewParticles;        // preview-only particle batch (torch)
	FireEffect m_previewFire;
	std::vector<gfx::ParticleInstance> m_previewFireScratch;
	float m_previewSpin = 0.0f; // turntable angle for auto-fit item previews
	// Asset bake (P4c): the AssetBaker subprocess for the dialog's Create. A
	// texture-set import is two steps (import textures, then rebake worn meshes);
	// a model import is one. Polled in Update so the frame never blocks.
	platform::Process m_bake;
	AssetDialog::CreateRequest m_bakeReq;
	bool m_baking = false;
	int m_bakeStep = 0;
	// Surface-look knobs for a `wornblock` bake (StartBakeStep appends them as
	// --wear/--relief). Defaults reproduce the original worn look, so the
	// asset-create path leaves them untouched; a type restyle sets them.
	// Relief < 0 = unspecified: the baker takes the set's own (Assets/WornSets.h).
	float m_bakeWear = 1.0f;
	float m_bakeRelief = -1.0f;
	// True while the running bake is a surface type's RESTYLE - the type editor
	// saved a `rebakes` field (StartRestyleBake). No new catalog entry; on
	// success LandRestyleBake writes the held Save and reloads the dungeon
	// blocks in place, instead of FinishBake.
	bool m_restyleBake = false;
	// The Save the restyle bake is for, written only once it lands clean
	// (C346), and whether the open type editor made it (`typeset` saves with
	// none open, and must not close or unfreeze an unrelated one).
	TypeEditorDialog::Config m_restyleCfg;
	bool m_restyleFromDialog = false;
	// HARNESS (`bake hold`): a finished bake does not land until `bake wait`
	// releases it - one landing per wait, the hold kept for the next (an import's
	// second run) until `bake hold off`. A debug wornblock bake takes ~60 ms, a
	// handful of headless frames, so without it what a script reads "while the
	// bake runs" - or a `bake wait` itself - raced the landing.
	bool m_bakeHeld = false;
	bool m_bakeRelease = false;

	// Child process launched to restart the game on an adapter change (it
	// outlives us; we quit right after it starts, and only then).
	platform::Process m_restart;
	// Window::DisplayChanges as last seen: a frame that finds it moved re-reads
	// the display list (GameUI::RefreshDisplays, code-review C199).
	u32 m_seenDisplayChanges = 0;
	// Window::Activations as last seen, and an activation not yet acted on: once
	// the window is not minimized, an Exclusive full-screen lost on the focus
	// loss (asked LIVE, GraphicsDevice::ExclusiveLost) is re-entered
	// (GraphicsDevice::RestoreExclusive, code-review C194).
	u32 m_seenActivations = 0;
	bool m_reenterExclusive = false;

	// The map overlay's panel in the given surface's pixel space (window pixels
	// for input, device pixels for drawing): full-screen in Editor mode (it
	// covers everything), else an 80%-centered rect for the player map.
	// The WORLD map owns the whole window: it is a STATE drawn alone, with no
	// scene or HUD behind it to show around the edges, so the inset that makes
	// the player's dungeon-map overlay read as an overlay would here just be
	// wasted screen with nothing under it.
	static gfx::Rect WorldPanel(float surfaceW, float surfaceH) {
		return {0.0f, 0.0f, surfaceW, surfaceH};
	}

	// --- the player map's two pages (W6) -------------------------------------
	// The M-map can show the DUNGEON the party is in or the WORLD it is in.
	// `m_mapView.IsOpen()` stays the flag for "the overlay is up" — it IS the
	// overlay — and this says which view fills it.
	//
	// THE WORLD PAGE IS PLAYER-MODE ONLY. The editor map has its own world
	// editor on its own screen, and a page flip in the middle of an editing
	// session would take the brushes away with it.
	enum class MapPage { Dungeon, World };
	// DERIVED, never latched — it asks whether the overlay is up, not whether
	// anything remembered to say so. A stair fired while the world page was
	// showing closes the map from somewhere that knows nothing about pages,
	// and a latched flag would have left the TRAVEL screen offering a way back
	// to a dungeon the party had left.
	bool ShowingWorldPage() const {
		return m_mapPage == MapPage::World && m_mapView.IsOpen() && m_worldMap &&
			   m_mapView.CurrentMode() == MapView::Mode::Player;
	}
	// Flips the page. Arriving on the world page refits it, the same bargain
	// MapView::Open makes: predictable, rather than wherever it was left.
	void ShowMapPage(MapPage page) {
		if (page == MapPage::World && m_mapPage != page) m_worldMapView.Reset();
		m_mapPage = page;
	}
	// Closes the map overlay: Esc, and both pages' close boxes. A stroke in
	// progress lands its undo step first, and the world view goes back to
	// being the travel screen.
	void CloseMapOverlay() {
		m_mapEditor.EndStroke();
		m_mapView.Close();
		ShowMapPage(MapPage::Dungeon);
	}
	MapPage m_mapPage = MapPage::Dungeon;

	gfx::Rect MapPanel(float surfaceW, float surfaceH) const {
		if (m_mapView.CurrentMode() == MapView::Mode::Editor)
			return {0.0f, 0.0f, surfaceW, surfaceH};
		const float pw = surfaceW * 0.8f, ph = surfaceH * 0.8f;
		return {(surfaceW - pw) * 0.5f, (surfaceH - ph) * 0.5f, pw, ph};
	}
};

} // namespace dungeon::game
