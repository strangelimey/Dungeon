// ============================================================================
// Game/Game.h — the dungeon crawler's app state machine.
//
// Game is the thin coordinator over the module classes — it owns them, wires
// their callbacks together, and runs the state machine that decides what
// updates and renders each frame:
//
//   GameSettings — user options (quality, volume, theme, colors, keys),
//                  persisted to settings.ini next to the exe
//   SoundBank    — the loaded sound effects, shared by every system
//   LoadQueue    — staged loading, one task per rendered frame
//   DungeonWorld — the 3D world: map, party, monsters, fires, lights,
//                  camera; simulation + the shadow and scene passes
//   GameUI       — menus, the settings page, HUD, character sheet, loading
//                  screens and overlays
//
// App states:
//   Loading     — boot load: just the menu essentials (title art, click
//                 sounds), one step per frame, so the landing page appears
//                 fast even on a cold cache
//   Menu        — landing page over baked title art; mouse hover or
//                 keyboard selects an entry
//   LoadingGame — the heavy dungeon load (meshes, scanned textures, world
//                 build), entered from "Start New Game" the first time;
//                 staged one task per frame behind its own progress screen
//   Playing     — the crawler: UI input → world simulation
//   Paused      — Esc while playing: the world freezes (no simulation) and
//                 a pause menu (Save/Load/Settings/Exit/Back) draws over
//                 the frozen scene; Esc backs out / resumes
//   CharacterSheet — clicking a party-bar portrait while playing: the world
//                 freezes like Paused and the character details page draws
//                 over it (prev/next cycle members, Esc/Back resumes)
//
// Everything binary loads from the assets/ directory next to the exe
// (regenerate with tools/AssetBaker). Engine modules know nothing about
// dungeons — all gameplay rules live in this module.
// ============================================================================
#pragma once

#include "Audio/AudioEngine.h"
#include "Core/AllocTrack.h"
#include "Core/ThreadManager.h"
#include "Game/AssetDialog.h"
#include "Game/AssetPicker.h"
#include "Game/Character.h"
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
#include "Game/MonsterConfigDialog.h"
#include "Game/ButtonInspector.h"
#include "Game/StairInspector.h"
#include "Game/DoorInspector.h"
#include "Game/NicheInspector.h"
#include "Game/ProjectileInspector.h"
#include "Game/PropInspector.h"
#include "Game/Project.h"
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

	// Set by the pause menu's Exit entry (and Esc outside of play); the
	// main loop polls it to leave cleanly.
	bool QuitRequested() const { return m_quitRequested; }

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
		NotPlaying,  // the app is not in play; nothing ran at all
	};
	int StepWorld(float seconds, StepStop& why);

	// The per-call ceiling, named so `step` can quote it rather than restate it.
	// A ceiling PER CALL, not per second: a script asking for an hour by mistake
	// should come back and say how far it got, rather than appearing to hang
	// with a black window and no way to interrupt it.
	static constexpr int kStepTicksPerSecond = 60;
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
	// True while a queued script still has lines to run.
	bool EvalRunning() const { return m_evalIndex < m_evalLines.size(); }
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
	void RegisterDiagnosticCommands(); // guards, threads, health (Game_DevDiagnostics.cpp)
	void RegisterPartyCommands(); // members, gear, pools (Game_DevParty.cpp)
	void RegisterEvalCommands(); // the eval harness's (Game_DevEval.cpp)

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
	// sources (Installed / Duplicate), which have nothing to bake.
	void CreateCatalogEntry(const AssetDialog::CreateRequest& req);
	// Records an IMPORT in the project's provenance manifest (imports.cat):
	// which pool asset, from where, with which options. The baked asset is
	// gitignored, so this is what makes a created type reproducible from a
	// clean checkout (tools/ReplayImports.ps1 replays them).
	void RecordImport(const AssetDialog::CreateRequest& req);

	// Type editor Save: merge the dialog's working fields into the catalog entry
	// and persist. Starts from the EXISTING entry, so fields the dialog doesn't
	// know (hand-authored, or MonsterConfigDialog's animation rows) survive.
	void WriteTypeFields(const TypeEditorDialog::Config& cfg);
	// Re-bakes a surface type's worn block meshes (its `texture` set at the
	// type's relief/wear) and, on success, reloads the dungeon blocks in place.
	// Launches the async wornblock bake; the caller freezes its dialog
	// meanwhile. `relief` < 0 leaves the baker's per-kind default amplitude.
	void StartRestyleBake(const std::string& catalogKey, const std::string& texture,
						  float wear, float relief);
	// Opens the type editor for a catalog id (the palette's right-click), or
	// does nothing when the catalog/entry is unknown.
	void OpenTypeEditor(MapEditor::PaletteCat cat, const std::string& id);
	// Creates an entry in a pure-data catalog (dungeons/terrain/quests) with a
	// free id and the schema's defaults; the caller opens the type editor on it
	// so the id can be renamed there. "" if the category has no catalog.
	std::string CreateAuthoredType(MapEditor::PaletteCat cat);
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
	bool SaveFontCatalog();

	// The editor toolbar's [+] button: writes a minimal .map/.ent pair next to
	// the project's other levels, appends the stem to the manifest, and returns
	// it ("" on failure) so the map view can jump straight onto the new canvas.
	// --- the world tier (Game_World.cpp) -----------------------------------
	// Resolves terrain.cat into rules and loads the project's world map. Called
	// once from the constructor; leaves m_worldMap empty when the project has
	// no world/world.map.
	void LoadWorldMap();
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
	// world leaves it blank.
	void ResetWorldState();
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
	// content hooks the design asked for, plus the flag escape hatch.
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

	// The playability check, with the world tier included. Every caller goes
	// through here rather than DungeonWorld::Validate directly, so no route can
	// quietly check the dungeons and skip the world.
	std::vector<validate::Issue> ValidateProject();
	// The `world` dev command's report: size, terrain, areas and locations, one
	// line each. Empty-world-safe.
	std::vector<std::string> WorldReport() const;

	// Creates a level on disk (minimal .map/.ent), appends it to the manifest
	// and — when `dungeonId` names one — to THAT DUNGEON'S level list, so a
	// level made from the editor is not born an orphan (W5). An empty id
	// creates it loose, which is what the manifest did for every level before
	// the world tier existed. Returns the stem, or "" on failure.
	// --- worlds (W7) ---------------------------------------------------------
	// Michael's word for a project. Each is a self-contained game under
	// assets/projects/<name>: its own overworld, dungeons, levels and content.
	//
	// Which one to open is decided before anything else exists, so SWITCHING
	// RELAUNCHES — the adapter change's bargain, for the adapter change's
	// reason: the level meshes, the surface textures, the catalogs and the
	// world are all built from the choice at startup, and tearing that down in
	// place would be a long tail of stale caches for a saving of ten seconds.
	static std::string ChooseProjectFolder();
	// The world a launch falls back on when the one it asked for is missing —
	// which is why it is also the one world that cannot be deleted.
	static constexpr const char* kDefaultProject = "dungeon-demo";
	// Opens `name` and starts a new game in it — in the process, next frame
	// (m_pendingWorld), no relaunch since docs/world-on-demand.md. Remembered as
	// the last world played unless -project named this run's. False when no
	// such world exists.
	bool SwitchWorld(const std::string& name);
	// Writes a NEW world beside this one and returns its name ("" on failure).
	// CONTENT IS COPIED, PLACES ARE NOT (Michael, 2026-09-23): every catalog
	// comes across — surfaces, monsters, items, terrain — so you can build in
	// it at once, while dungeons and quests start empty and the overworld is
	// blank. It does get ONE room in one dungeon behind one doorway, because
	// the engine loads a level in DungeonWorld's constructor and a world with
	// nowhere at all in it could not stand up; that starter is the smallest
	// thing that both loads and passes the checker.
	std::string CreateWorld(const std::string& name);
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
	// list + a stair from the floor above) and return its stem, "" on failure.
	// Generated from `params` when given, else the minimal empty box. THE ONE
	// WRITER of new levels: the [+] dialog, the `newlevel` and `generate`
	// commands all come through here, so a generated level cannot be named,
	// grouped or linked differently from an empty one.
	std::string CreateNewLevel(const std::string& dungeonId = {},
							   const generate::Params* params = nullptr);
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
	// resolved HERE from the project's catalogs by theme tag - the generator
	// itself never sees a catalog (Game/Generate.h).
	std::vector<std::pair<int, int>>
	ComposeGeneratedLevel(const std::string& stem, generate::Params params,
						  const std::vector<std::string>& theme, std::string& map,
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
	std::vector<std::pair<int, int>> ArrivalsOn(const std::string& stem) const;
	// Build the level text (palette from `donor`, `stairs` carried across
	// verbatim), parse it, and hand it to the world.
	bool BuildAndInstall(const std::string& stem, const generate::Params& params,
						 const std::vector<std::string>& theme,
						 const DungeonMap& donor, std::span<const StairLink> stairs);
	// Resolve the theme into the id pools the generator picks from.
	void FillPools(generate::Params& params,
				   const std::vector<std::string>& theme);
	// One kind's threat (Game/Threat.h), its attacks resolved by the world when
	// one is loaded (spells, powers, on-hit effects), else melee from the catalog.
	threat::Parts ThreatOf(const CatalogEntry& monster) const;
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

	// Starts a mid-game level transition (P6): swaps the world to `stem`, stages
	// its load behind the loading screen, and arrives at (x,z,facing) when done
	// (x<0 = the level's start cell). `stashCurrent` saves the level being left
	// for a later return; pass false when leaving a throwaway baseline (save load).
	void BeginLevelTransition(const std::string& stem, int x, int z,
							  Direction facing, bool stashCurrent = true);
	// True when the frame now starting is one the steady-state allocation rule
	// covers (Core/AllocTrack). Stateful — it counts the warm-up.
	bool SteadyStateFrame();
	// Called when an overlay opens PART WAY THROUGH an already-armed frame:
	// disarms the guard for it and restarts the warm-up. See the definition.
	void OverlayOpenedThisFrame();
	// Advances a running `alloctest` window and reports when it closes. The
	// window is measured in ARMED frames, so time spent loading, warming up or
	// with the console open does not spend it.
	void UpdateAllocTest(float dt, bool steady);
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
	void LoadPortraits();      // baked party portraits (load task)
	void LoadHitSplats();      // hit-feedback splat icons (load task)
	void LoadItemIcons();      // rune + placeholder item cursor/inventory icons (load task)

	// --- state transitions --------------------------------------------------
	// Puts the party in a level, staging a LOAD only when it is not the one
	// already held. True = a load is in flight and the caller is done.
	bool OpenInLevel(const std::string& level, int x, int z);
	void StartNewGame();
	// Resets the roster to a fresh default party in place, keeping each slot's
	// loaded portrait. The HUD/sheet widgets address members by (roster, index)
	// and re-resolve every frame, so even a roster RESIZE can't dangle them —
	// but a size change must still call GameUI::RebuildForRoster (deferred, not
	// from a widget callback) to re-lay-out the per-member widgets. Shared by
	// StartNewGame and LoadGame.
	void ResetRoster();
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
	void OpenCharacterSheet(size_t index); // freezes the world, shows the page
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
	// + resolution) to the window and swapchain in place. Called at boot and by
	// the Video tab's Apply button for non-adapter changes.
	void ApplyDisplaySettings();
	// Relaunches the executable (a fresh process picks up the new adapter, the
	// only way to switch GPUs, or a new world) and flags this instance to quit.
	// `extraArgs` go on the new command line (`-newgame` for the world list).
	void RestartApp(const std::string& extraArgs = {});
	// The new-game world list's pick: a new game now if it is the world
	// running, else remember it and relaunch into a new game there.
	void StartNewGameIn(const std::string& folder);
	// Loads the settings' language file (falling back to English when it is
	// missing); rebuild=true also re-creates every UI page in the new
	// language. The language dropdown only records m_pendingLanguage —
	// Update applies it at the top of the next frame, after the dropdown's
	// callback has fully unwound (the rebuild destroys the dropdown).
	void ApplyLanguage(bool rebuild);
	// Per-frame adaptive thread governor (see the definition in Game.cpp). No-op
	// unless `governor auto` is enabled.
	void UpdateGovernor(float dt);
	// Feeds the slowest member's moveSpeed into the Party as its pace
	// multiplier; call whenever the roster's stats are (re)filled.
	void ApplyPartySpeed();
	// Put the game where `newgame` would, WITHOUT the level load — the eval
	// harness's world recycling (docs/eval-harness.md). Falls back to a real new
	// game when nothing is loaded yet, so a batch's FIRST script pays the twelve
	// seconds and none of the rest do.
	bool ResetForEval();
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
	// THE EVAL HARNESS ASKS FOR A LEVEL (Game_Eval.cpp). `reset` means "where a
	// new game would leave it", and since P4 that is the WORLD MAP, where
	// nothing simulates — ten combat suites would have gone on printing
	// plausible readouts about a party standing in a field
	// (docs/world-map.md "Obligations"). So the harness states what it wants
	// and StartNewGame honours it, instead of the two silently tracking each
	// other. Never set outside the harness.
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
	};
	std::optional<PendingWorld> m_pendingWorld;
	// The landing page's Editor entry asked for the editor, paused, once the
	// new game it started arrives (OpenEditorOnArrival).
	bool m_editorOnArrival = false;
	void OpenEditorOnArrival();
	void ApplyPendingWorld();
	u32 m_framesRendered = 0;
	// Consecutive frames that have been quietly Playing — the allocation guard's
	// warm-up counter (see SteadyStateFrame).
	u32 m_steadyFrames = 0;
	// `alloctest`: an armed-seconds budget, a wall-clock deadline so a test that
	// never reaches steady state reports SKIP instead of hanging, and the guard
	// stats at the window's start (the result is their delta).
	float m_allocTestRemaining = 0.0f;
	float m_allocTestDeadline = 0.0f;
	u32 m_allocTestFrames = 0;
	alloc::GuardStats m_allocTestStart;
	// `allocpoke`: allocate deliberately, every frame, for this many seconds.
	// It exists so the guard and tools\AllocTest.ps1 can be shown to FAIL — a
	// regression test that cannot fail proves nothing. m_pokeScratch holds the
	// result so the allocation cannot be optimized away.
	float m_allocPokeRemaining = 0.0f;
	std::unique_ptr<u32> m_pokeScratch;
	// Frame count when the current loading state was entered; tasks only run
	// once its screen has been presented at least once.
	u32 m_stateFrameMark = 0;
	bool m_quitRequested = false;
	float m_time = 0.0f;
	// Dev console `timescale`: multiplies the world's dt (1 = normal, 0 = freeze).
	float m_timeScale = 1.0f;
	// Language code picked in Settings this frame, applied (strings reloaded,
	// UI rebuilt) at the top of the next Update; empty = no change pending.
	std::string m_pendingLanguage;
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
	Direction m_pendingLevelFacing = Direction::South;
	// Free-look offset to re-layer once the arriving party is placed. Orthogonal
	// for ordinary transitions (stairs/new game); a save load on a DIFFERENT level
	// seeds it from the save so the exact look angle survives the level rebuild.
	float m_pendingLookYaw = 0.0f, m_pendingLookPitch = 0.0f;
	bool m_pendingLooking = false;
	// A save made on the world map is loading onto a different level: when the
	// load lands, ResumeOnWorldMap(m_pendingWorldPark) instead of playing on.
	bool m_pendingWorldMap = false, m_pendingWorldPark = false;

	// --- modules (construction order matters: settings load first, the world
	// and UI reference settings/sounds/characters) -------------------------------
	GameSettings m_settings;
	// The active project: content catalogs + levels (assets/projects/<name>).
	// Loaded before the world (which reads it for level paths and catalogs);
	// the editor will read and write it.
	Project m_project;
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
	// Party roster (up to four). Filled once in the constructor and never
	// resized — the party-bar panels and the sheet hold pointers into it, so
	// StartNewGame resets the members in place.
	bool m_harnessOpensInLevel = false;
	std::vector<Character> m_characters;
	// Baked portrait textures, parallel to m_characters (entries may be null
	// when the asset is missing; Character::portrait points in here).
	std::vector<std::unique_ptr<gfx::Texture>> m_portraitTextures;
	// Hit-feedback splat icons (small/medium/hard) + the pointer struct the
	// party bar reads. The struct address is stable, handed to GameUI once at
	// construction; LoadHitSplats fills it in during the staged load.
	std::unique_ptr<gfx::Texture> m_hitSplatTextures[3];
	HitSplatIcons m_hitSplats;

	// Item icons for the held cursor / hand slots / inventory. Game owns the
	// textures; the bank (catalog id → texture) is handed to GameUI once, address
	// stable, filled by LoadItemIcons: rune tablets load element PNGs, other
	// categories get a generated solid-tint placeholder (m_itemIconPlaceholders).
	std::array<std::unique_ptr<gfx::Texture>, kSymbolCount> m_runeIconTextures;
	std::vector<std::unique_ptr<gfx::Texture>> m_itemIconPlaceholders;
	ItemIconBank m_itemIcons;
	ItemWeightBank m_itemWeights; // catalog id → carry weight (kg), for the sheet
	ItemCategoryBank m_itemCategories; // catalog id → category, for the sheet
	// Equipment-slot outline silhouettes (slot type → texture), drawn as the
	// ghost behind an empty doll slot. Filled by LoadItemIcons from slot_*.png.
	std::vector<std::unique_ptr<gfx::Texture>> m_slotIconTextures;
	ItemIconBank m_slotIcons;
	// The item currently carried on the cursor (its catalog id), or empty. Set by
	// clicking a floor tablet; cleared by dropping it (world / portrait / hand /
	// inventory). GameUI reads the address to draw the cursor icon.
	std::optional<std::string> m_heldItem;
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
	// Typefaces, addressed by role (UI/FontLibrary.h). Declared BEFORE m_ui
	// because every UIContext there borrows a Font from it, and configured from
	// assets/fonts/fonts.cat before those contexts first resolve a role — see
	// MakeFontLibrary in Game.cpp.
	ui::FontLibrary m_fonts;
	GameUI m_ui;
	// Map/editor overlay (toggle with `M` while playing). Like the console it
	// does NOT pause the world — the party keeps walking; the overlay only
	// claims the mouse for panning/zooming/editing.
	// The overworld screen (docs/world-map.md). Its own class, not a third
	// MapView mode: MapView is built around a DungeonMap and its docks,
	// palette and brushes, none of which mean anything on the world.
	WorldMapView m_worldMapView;
	// A paint STROKE is open: the first changed cell began an undo step and the
	// mouse release closes it, so one drag is one Ctrl+Z.
	bool m_worldStroke = false;
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
	// The worlds BESIDE this one (W8): list, open (relaunches), create. The
	// world toolbar's leftmost disc; `worlds` is the same thing typed.
	WorldsDialog m_worldsDialog;
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
	// The inspector's config for the monster being edited — kept so route-laying can
	// reopen the inspector (with an updated waypoint count) when it finishes.
	EntityInspector::Config m_inspectCfg;
	// Live animation preview for that dialog: an Animator over the selected type's
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
	PreviewSpec m_inspectPreview;                 // cached spec (re-pass on route return)
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
	// Relief < 0 = unspecified: the baker keeps its per-kind default amplitude.
	float m_bakeWear = 1.0f;
	float m_bakeRelief = -1.0f;
	// True while the running bake is a Wall Style RESTYLE (no new catalog entry;
	// on success reload the dungeon blocks in place instead of FinishBake).
	bool m_restyleBake = false;

	// Child process launched to restart the game on an adapter change (it
	// outlives us; we quit right after).
	platform::Process m_restart;

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
	MapPage m_mapPage = MapPage::Dungeon;

	gfx::Rect MapPanel(float surfaceW, float surfaceH) const {
		if (m_mapView.CurrentMode() == MapView::Mode::Editor)
			return {0.0f, 0.0f, surfaceW, surfaceH};
		const float pw = surfaceW * 0.8f, ph = surfaceH * 0.8f;
		return {(surfaceW - pw) * 0.5f, (surfaceH - ph) * 0.5f, pw, ph};
	}
};

} // namespace dungeon::game
