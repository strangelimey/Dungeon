// ============================================================================
// Game/Game.cpp — see Game.h. The module classes do the real work; this file
// is construction, the world's lifetime (LoadWorld / UnloadWorld), the staged-
// load task lists, portraits and icons, new game / save / load, arming the
// allocation guard (SteadyStateFrame, `alloctest`), and the state machine
// (Update / UpdateStates / Render). The callback wiring is Game_Wiring.cpp;
// each other Game_*.cpp is one subsystem hanging off the state machine.
// ============================================================================
#include "Game/Game.h"

#include "Assets/Image.h"
#include "Core/AllocTrack.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Profile.h"
#include "Core/StringUtil.h"
#include "Game/AssetUtil.h"
#include "Game/GenerateKnobs.h"
#include "Game/PartyHudDraw.h" // DrawFlame (the details dialog's burning torch)
#include "Graphics/DisplayEnum.h"
#include "Graphics/Texture.h"
#include "Platform/PerfMonitor.h"
#include <shellapi.h> // CommandLineToArgvW - the `-project` flag
#include "UI/TreeInspector.h" // the frame hook for the `uioverlap` audit

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>

namespace dungeon::game {

namespace {

// Whether this launch was given `flag` on its command line.
bool CommandLineHas(std::wstring_view flag) {
	int argc = 0;
	bool found = false;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
		for (int i = 1; i < argc && !found; ++i) found = std::wstring_view(argv[i]) == flag;
		LocalFree(argv);
	}
	return found;
}

// Builds the font library with assets/fonts/fonts.cat already applied.
//
// It is a function rather than two statements in the constructor body because
// the roles must be set BEFORE the UI contexts first resolve one: m_ui is a
// member, so its constructor runs before any constructor body could configure
// the library, and each context would otherwise build an atlas for the fallback
// face and abandon it a frame later.
//
// The library cannot read its own config: Catalog/Serialize live in this lib,
// which sits ABOVE UI in the layer order. Same split as DungeonMap taking
// FixtureTypes because the map has no catalog access.
ui::FontLibrary MakeFontLibrary(gfx::GraphicsDevice& device) {
	ui::FontLibrary fonts(device);
	Catalog cat;
	cat.Load(paths::Asset("fonts\\fonts.cat"));
	for (int i = 0; i < ui::kFontRoleCount; ++i) {
		const auto role = static_cast<ui::FontRole>(i);
		const CatalogEntry* entry = cat.Find(ui::FontRoleName(role));
		if (!entry) continue;
		ui::FaceSpec spec;
		// An absent or empty `file` leaves the path empty, which the library
		// reads as "system fallback" — how every role ships until the audition
		// (docs/fonts.md Phase 4) picks a face.
		if (const std::string file = entry->Get("file", ""); !file.empty())
			spec.path = paths::Asset(file);
		spec.scale = entry->GetFloat("scale", 1.0f);
		fonts.SetFace(role, std::move(spec));
	}
	return fonts;
}

// The world's input while the character sheet has the keyboard: nothing held.
// At namespace scope, built at startup - as a function-local static it was
// constructed (and allocated) on the first sheet frame, which is guarded.
const Input kNoInput;

} // namespace

// THE WORLD THE GAME OPENS (W7), decided before anything else exists. Three
// sources, in order, because each answers a different question:
//   1. `-project <name>` on the command line — ONE RUN, touching no settings.
//      This is how a test scenario gets its own world: the harness launches
//      into it and the developer's own choice is left alone.
//   2. settings.ini `project=` - the world you last switched to (SwitchWorld
//      writes it), so the next launch opens where you left off.
//   3. dungeon-demo, the one that ships.
// A HARNESS RUN (`-eval`) SKIPS 2. It measures a scenario, and the scenario must
// not be whichever world the developer last switched into — the first time
// Michael switched to a world of his own, every eval suite started measuring
// ITS starter room instead of eval_arena, and would have passed doing it. A
// harness that wants another world says so with `-project`.
// A name that does not resolve falls back rather than aborting: a settings file
// naming a world since deleted must not make the game unlaunchable.
std::string Game::ChooseProjectFolder() {
	const std::string root = paths::Asset("projects");
	std::string name;
	bool harness = false;
	int argc = 0;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
		for (int i = 1; i < argc; ++i)
			if (std::wstring_view(argv[i]) == L"-eval") harness = true;
		for (int i = 1; i + 1 < argc; ++i)
			if (std::wstring_view(argv[i]) == L"-project") {
				// UTF-8 like every name here (Core/Paths.h). World names are
				// ASCII (CreateWorld filters them), so anything else simply
				// matches no world and falls back below.
				name = str::Narrow(argv[i + 1]);
				break;
			}
		LocalFree(argv);
	}
	if (name.empty() && !harness) {
		GameSettings probe; // the same file Game re-loads, read before it exists
		probe.Load();
		name = probe.projectName;
	}
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), name) == found.end()) {
		if (!name.empty() && name != kDefaultProject)
			log::Warn("no world '{}' under {} - opening {}", name, root, kDefaultProject);
		name = kDefaultProject;
	}
	// Only the DEFAULT: nothing is opened until a game starts (LoadWorld).
	log::Info("Default world '{}'", name);
	return Project::FolderFor(root, name);
}

// ============================================================================
// Construction — cheap setup only; the heavy asset work is queued as load
// tasks that run one per frame behind the loading screen (see Update).
// ============================================================================
Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
	: m_window(window), m_device(device), m_renderer(renderer),
	  m_spriteBatch(spriteBatch), m_audio(audio), m_postProcess(device),
	  m_fonts(MakeFontLibrary(device)),
	  m_ui(window, device, spriteBatch, audio, m_sounds, m_settings,
		   m_characters, m_fonts),
	  m_worldMapView(device, m_fonts),
	  m_mapView(device, m_settings, m_fonts),
	  m_mapEditor(m_mapView, m_settings),
	  m_console(m_fonts, m_threads),
	  m_modelPreview(device, 512),
	  m_assetDialog(device, window),
	  m_monsterDialog(device, m_fonts), m_balanceDialog(device, m_fonts),
	  m_levelSettingsDialog(device, m_fonts),
	  m_worldSettingsDialog(device, m_fonts),
	  m_worldsDialog(device, m_fonts),
	  m_newWorldDialog(device, m_fonts),
	  m_validateDialog(device, m_fonts),
	  m_generateDialog(device, m_fonts),
	  m_typeDialog(device, m_fonts),
	  m_assetPicker(device, m_fonts),
	  m_entityInspector(device, m_fonts), m_fixtureInspector(device, m_fonts),
	  m_propInspector(device, m_fonts), m_doorInspector(device, m_fonts),
	  m_buttonInspector(device, m_fonts), m_nicheInspector(device, m_fonts),
	  m_stairInspector(device, m_fonts),
	  m_projectileInspector(device, m_fonts), m_inspectPicker(device, m_fonts),
	  m_previewParticles(device) {
	m_mapView.SetEditor(&m_mapEditor); // the view drives the editor in Editor mode
	// The editor's header save buttons: Save = write every edited level (what the
	// savemap console command does); To source = also copy the project into the
	// repo tree. Feedback goes through the world's message channel.
	m_mapView.onSave = [this](bool toSource) {
		if (!m_gameLoaded) return; // no world to save yet
		const std::vector<std::string> saved = m_world->SaveAllLevels();
		bool ok = !saved.empty();
		// The world and the opening too, as `savemap` does: an editor step can
		// reach both now (a dragged stair carries a doorway's arrival square and
		// the opening), and a Save that left them behind would put the files out
		// of step with each other.
		if (m_worldMap) ok = SaveWorld() && ok;
		if (m_world->ConsumeOpeningMoved()) ok = m_project.Save() && ok;
		if (ok && toSource) ok = SyncProjectToSource();
		if (!m_world->onMessage) return;
		if (!ok) {
			m_world->onMessage(loc::View("map.save.failed"));
			return;
		}
		std::string list;
		for (const std::string& s : saved) list += (list.empty() ? "" : ", ") + s;
		m_world->onMessage(
			loc::FormatLine(toSource ? "map.save.synced" : "map.save.done", list));
	};
	// The editor's Balance header button → the combat-tuning dialog. Edits
	// apply LIVE (the world's Balance is the one every formula reads, and the
	// derived resource maxima follow); Save also writes the two catalogs back
	// to the project (the asset copy — To source syncs them to the repo).
	m_mapView.onBalance = [this] { OpenBalanceDialog(); };
	// The Effects tab's edit disc: the type editor, over the Balance dialog.
	m_balanceDialog.onEditEffect = [this](const std::string& id) {
		OpenTypeEditor(MapEditor::PaletteCat::Effects, id);
	};
	// Both also drop the cached monster powers: a monster's threat is scored
	// through the balance knobs (Potent), and moves with no edit to mark it.
	m_balanceDialog.onApply = [this](const Balance& b) {
		m_world->GetBalance() = b;
		m_world->RecomputePartyMaxima();
		m_world->InvalidatePowers();
	};
	m_balanceDialog.onSave = [this](const Balance& b) {
		m_world->GetBalance() = b;
		m_world->RecomputePartyMaxima();
		m_world->InvalidatePowers();
		b.Save(m_project.balance, m_project.attacks);
		const bool ok =
			m_project.balance.Save(m_project.CatalogPath("balance.cat"),
								   "Balance: the attack-formula knob sheet "
								   "([formula] block; docs/combat.md).") &&
			m_project.attacks.Save(m_project.CatalogPath("attacks.cat"),
								   "Attacks: per-melee-verb numbers "
								   "(damage/accuracy/speed multipliers); "
								   "identity (damage type) is C++ (Balance.h).");
		if (m_world->onMessage)
			m_world->onMessage(loc::View(ok ? "map.balance.saved" : "map.save.failed"));
	};
	// The editor toolbar's Level button → the per-level atmosphere dialog,
	// opened on the VIEWED level's effective values (a browsed level's come
	// from its stash-backed snapshot). Edits preview live only while that
	// level is the active one — a browsed level isn't on screen to preview.
	// Save commits to the level's map/stash; the values persist as the .map
	// `atmosphere` record on the next savemap (the toolbar Save).
	m_mapView.onLevelSettings = [this] {
		float dust, haze, ambient;
		DungeonWorld::EffectiveAtmosphere(m_mapView.ViewedMap(), dust, haze, ambient);
		// The tags go in as the user types it: space-separated words.
		std::string tags;
		for (const std::string& tag : m_mapView.ViewedMap().Tags())
			tags += (tags.empty() ? "" : " ") + tag;
		// What "the dungeon's" means for this level - its dungeon's material,
		// else the default - so that row's picture is what the level would wear.
		std::string dungeonStone = GameSettings::kDefaultUiStone;
		if (const CatalogEntry* d = m_project.DungeonOfLevel(m_mapView.ViewedLevel()))
			dungeonStone = d->Get("ui_stone", dungeonStone);
		m_levelSettingsDialog.Open(m_mapView.ViewedLevel(), dust, haze, ambient, tags,
								   m_mapView.ViewedMap().UiStone(), dungeonStone);
	};
	m_levelSettingsDialog.thumbFor = [this](const std::string& stone) {
		return m_ui.StoneThumb(stone);
	};
	m_levelSettingsDialog.stoneOrder = [this] { return m_ui.StoneOrder(); };
	m_levelSettingsDialog.stoneFilterLabels = [this] { return m_ui.StoneFilterLabels(); };
	m_levelSettingsDialog.stoneFilterColors = [this] { return m_ui.StoneFilterColors(); };
	m_levelSettingsDialog.stoneFilterBits = [this](const std::string& stone) {
		return m_ui.StoneFilterBits(stone);
	};
	// Picking a material shows it at once; closing without Save hands the
	// chrome back (a Save has already re-resolved the place by then).
	m_levelSettingsDialog.onPreviewStone = [this](const std::string& stone) {
		if (stone.empty()) m_ui.EndStonePreview();
		else m_ui.PreviewStone(stone);
	};
	m_levelSettingsDialog.sampleSkin = &m_ui.GameSkin();
	// The Check toolbar button: run the whole-project playability check and show
	// what it found. Reads live state, so it answers for unsaved edits too —
	// which is exactly when you want to hear that a door just became unopenable.
	m_mapView.onValidate = [this] { m_validateDialog.Open(ValidateProject()); };
	// The Generate toolbar button + its knobs. A reroll replaces the VIEWED
	// level as one undo step, then the check runs immediately — the whole reason
	// the lock ordering is built by construction is so that comes back clean, and
	// showing it is how anyone finds out it stopped.
	m_mapView.onGenerate = [this] {
		// The CURRENT style (Phase 7) is the one a reroll or a populate is in:
		// arming another since loads its recipe; a style this world lacks (the
		// knobs are remembered across worlds) is dropped rather than half-used.
		generate::Params p = m_generateDialog.Knobs();
		const std::string& armed = m_mapEditor.CurrentStyle();
		if (!armed.empty() && armed != p.style) LoadStyleKnobs(armed, p);
		if (!m_project.styles.Contains(p.style)) p.style.clear();
		m_generateDialog.SetKnobs(p);
		m_generateDialog.OpenRegenerate(m_mapView.ViewedLevel());
	};
	m_generateDialog.onPopulate = [this](const generate::Params& p) {
		if (PopulateViewedLevel(p) < 0) return;
		ShowGenReport(m_mapView.ViewedLevel());
		m_validateDialog.Open(ValidateProject());
	};
	m_generateDialog.onGenerate = [this](const generate::Params& p) {
		if (!RegenerateViewedLevel(p)) return;
		ShowGenReport(m_mapView.ViewedLevel());
		if (m_world->onMessage)
			m_world->onMessage(loc::FormatLine("map.gen.done", m_mapView.ViewedLevel(),
											  p.seed));
		m_validateDialog.Open(ValidateProject());
	};
	// Clicking a finding shows it: browse to its level, and select the cell so
	// the highlight says which square. A finding about a level as a whole
	// (x < 0) just browses there.
	m_validateDialog.onJump = [this](const std::string& level, int x, int z) {
		m_mapView.SetViewLevel(level);
		if (x >= 0) m_mapEditor.SelectCell(x, z);
	};
	m_levelSettingsDialog.onApply = [this](float dust, float haze, float ambient) {
		if (m_levelSettingsDialog.Level() != m_world->CurrentLevel()) return;
		m_world->SetDustDensity(dust);
		m_world->SetHazeAmbient(haze);
		m_world->SetAmbientScale(ambient);
	};
	m_levelSettingsDialog.onSave = [this](float dust, float haze, float ambient,
										 const std::string& tags,
										 const std::string& uiStone) {
		m_world->SetLevelAtmosphere(m_levelSettingsDialog.Level(), dust, haze, ambient);
		m_world->SetLevelTags(m_levelSettingsDialog.Level(), ParseTags(tags));
		m_world->SetLevelUiStone(m_levelSettingsDialog.Level(), uiStone);
		RefreshPlaceStone(); // the chrome follows at once if this is the active level
		// Authoring a level's material while the player's setting is pinned to
		// another would show nothing in play (Michael hit exactly that), so a
		// Save hands the choice back to the place - and says so.
		const bool unpinned = m_ui.FollowPlaceStone();
		if (m_world->onMessage) {
			m_world->onMessage(loc::FormatLine("map.level.applied",
											  m_levelSettingsDialog.Level()));
			if (unpinned) m_world->onMessage(loc::View("map.level.uistone.follow"));
		}
	};
	// The editor toolbar's [+] button: the generator dialog in CREATE mode,
	// aimed at the viewed dungeon (docs/level-building.md P1). Its Create /
	// Empty buttons mint the level (files + manifest + the dungeon's level list
	// + the stair from the floor above) and the view jumps onto it. A generated
	// one is CHECKED at once, like a regenerate.
	m_mapView.onNewWorld = [this] { m_newWorldDialog.Open(); };
	m_mapView.onNewLevel = [this](const std::string& dungeonId) {
		// Said up front: which dungeon, and the floor the new one will stair
		// down from (CreateNewLevel links to the dungeon's LAST floor).
		std::string where = loc::Tr("map.gen.nodungeon");
		if (const CatalogEntry* d = m_project.dungeons.Find(dungeonId)) {
			const std::vector<std::string> floors = m_project.DungeonLevels(dungeonId);
			where = floors.empty()
						? loc::Format("map.gen.wherefirst", d->Display())
						: loc::Format("map.gen.where", d->Display(), floors.back());
		}
		// OPENS ON THE DUNGEON'S STYLE (Phase 7), which fills the tags, the look
		// and the shape knobs in one go - no Level settings visit first, no
		// palette donor to pick. No style: the knobs as last used.
		generate::Params p = m_generateDialog.Knobs();
		p.style.clear();
		if (const std::string style = DefaultStyleFor(dungeonId); !style.empty())
			LoadStyleKnobs(style, p);
		m_generateDialog.SetKnobs(p);
		m_generateDialog.OpenCreate(dungeonId, where);
	};
	m_generateDialog.onCreate = [this](const std::string& dungeonId,
									   const generate::Params* p) {
		// The empty box is in the dialog's style too.
		const std::string style = p ? p->style : m_generateDialog.Knobs().style;
		const std::string stem = CreateNewLevel(dungeonId, p, style);
		if (stem.empty()) return stem;
		if (p) ShowGenReport(stem);
		// ...and lands you in the BUILD stage with that style armed.
		LandInBuild(stem, style);
		if (p) m_validateDialog.Open(ValidateProject());
		return stem;
	};
	m_generateDialog.onChoice = [this](std::string_view key, generate::Params& p) {
		// Picking a style brings its recipe with it.
		return key == "style" && !p.style.empty() && LoadStyleKnobs(p.style, p);
	};
	// P4b: the dialog's choices and presets, answered from the project.
	m_generateDialog.choicesFor = [this](std::string_view key) {
		std::vector<std::pair<std::string, std::string>> out{{"", loc::Tr("map.gen.asbefore")}};
		if (key == "style") {
			out.front().second = loc::Tr("map.gen.nostyle");
			for (const CatalogEntry& s : m_project.styles.Entries()) out.push_back({s.id, s.Display()});
		} else if (key == "palette") {
			for (const std::string& stem : m_project.levels) out.push_back({stem, stem});
		} else if (key == "tag") {
			// Every tag the pools could be drawn by: the content catalogs' and
			// the dungeons' own flavour tags, each once, in order.
			std::vector<std::string> tags;
			for (const Catalog* c : {&m_project.monsters, &m_project.items, &m_project.dungeons})
				for (const CatalogEntry& e : c->Entries())
					for (const std::string& t : ParseTags(e.Get("tags", "")))
						if (std::find(tags.begin(), tags.end(), t) == tags.end())
							tags.push_back(t);
			std::sort(tags.begin(), tags.end());
			for (const std::string& t : tags) out.push_back({t, t});
		}
		return out;
	};
	m_generateDialog.presetNames = [this] { return GenPresetNames(); };
	m_generateDialog.onPlay = [this](const std::string& stem) { PlayLevel(stem); };
	m_generateDialog.onPresetLoad = [this](const std::string& name, generate::Params& p) {
		return LoadGenPreset(name, p);
	};
	m_generateDialog.onPresetSave = [this](const std::string& name,
										   const generate::Params& p) {
		return SaveGenPreset(name, p);
	};
	m_generateDialog.onPresetDelete = [this](const std::string& name) {
		return DeleteGenPreset(name);
	};
	// Persist the knobs when they are USED, so the next session's dialog opens
	// on the level you were last rolling rather than on the defaults.
	m_generateDialog.onKnobsUsed = [this](const generate::Params& p) {
		m_settings.generatorKnobs = generate::Encode(p);
		m_settings.Save();
	};
	// The Level dialog's inline name edit → the full rename flow.
	m_levelSettingsDialog.onRename = [this](const std::string& oldStem,
											const std::string& newStem) {
		return RenameLevel(oldStem, newStem);
	};
	m_settings.Load();
	{
		generate::Params knobs;
		generate::Decode(m_settings.generatorKnobs, knobs);
		m_generateDialog.SetKnobs(knobs);
	}
	ApplyLanguage(false); // strings must exist before any UI builds
	m_audio.SetMasterVolume(m_settings.volume);
	m_device.SetPresentInterval(m_settings.presentInterval);

	m_characters = CreateDefaultParty();
	ApplyMemberColors(); // the settings palette wins over the authored defaults
	// (The world takes the roster, the keys and the look settings when it is
	// BUILT — LoadWorld — since there is none yet.)
	m_ui.SetHitSplats(&m_hitSplats);  // stable address; LoadHitSplats fills it in
	m_ui.SetItemIcons(&m_itemIcons);    // stable; LoadItemIcons fills it in
	m_ui.SetItemWeights(&m_itemWeights); // stable; LoadItemIcons fills it in
	m_ui.SetItemCategories(&m_itemCategories); // stable; LoadItemIcons fills it in
	m_ui.SetSlotIcons(&m_slotIcons);     // stable; LoadItemIcons fills it in
	m_ui.SetUseIcons(&m_useIcons);       // stable; LoadItemIcons fills it in
	m_ui.SetHeldItem(&m_heldItem);    // cursor icon reads the held catalog id

	// NO WORLD IS LOADED HERE (docs/world-on-demand.md): the title screen runs
	// without one, and LoadWorld builds it when a game starts. What is decided
	// now is only which world a start with nothing else to go on would open.
	m_defaultWorld = std::filesystem::path(ChooseProjectFolder()).filename().string();

	// Read once: a `-project` launch has its world decided already, so the
	// new-game list does not ask...
	m_worldFromCommandLine = CommandLineHas(L"-project");
	// ...and its saves are that world's alone (SaveGame.h SetSaveWorldFilter).
	if (m_worldFromCommandLine) SetSaveWorldFilter(m_defaultWorld);

	// The console's section states round-trip settings.ini (console_*_expanded).
	m_console.SetSections({m_settings.consolePerfExpanded, m_settings.consoleProfileExpanded,
						   m_settings.consoleThreadsExpanded});
	m_console.onSectionsChanged = [this] {
		const DevConsole::Sections s = m_console.GetSections();
		m_settings.consolePerfExpanded = s.perf;
		m_settings.consoleProfileExpanded = s.profile;
		m_settings.consoleThreadsExpanded = s.threads;
		m_settings.Save();
	};

	WireModuleCallbacks();
	RegisterDevCommands();
	RegisterDungeonCommands();
	RegisterWorldCommands();
	RegisterDiagnosticCommands();
	RegisterPartyCommands();
	RegisterPartyCreationCommands();
	RegisterEvalCommands();
	RegisterStyleCommands();
	RegisterMapIconCommands();
	// THE TITLE SCREEN HAS NO WORLD (docs/world-on-demand.md), and most
	// commands reach into one. Rather than a guard in each of a hundred and
	// twenty handlers, ONE gate: with no world loaded, only the commands listed
	// here run — the global ones, plus the two the harness starts a game with.
	// A new command is gated by default, which is the safe way round: it can
	// only fail to run on the title screen, never crash there.
	m_console.gate = [this](std::string_view name) -> std::string {
		if (m_world) return {};
		static constexpr std::string_view kNoWorldNeeded[] = {
			"help", "clear", "echo", "profile", "quit", "exit", "fps", "framecap",
			"lang", "quality", "fonts", "font", "ver", "loadstats", "allocguard",
			"allocpoke", "crashpoke", "health", "throttle", "governor",
			"threads", "threadspawn", "threadwedge", "threadprio", "threadaffinity",
			"threadreap", "uitree", "uioverlap", "clippoke", "logecho", "timescale",
			"state",
			"worlds", "newgame", "reset", "newparty", "partypage",
		};
		for (std::string_view n : kNoWorldNeeded)
			if (n == name) return {};
		return "no world is loaded - start or continue a game first";
	};

	m_ui.BuildStaticUi();
	BuildBootLoadTasks();

	// Honor a saved borderless/exclusive display mode now that the window and
	// device exist (windowed at the default size needs nothing) - unless the
	// window is HIDDEN (`-headless`). The harnesses run the build Michael plays,
	// sharing its settings.ini, and applying his saved mode showed every headless
	// run's window, covered a monitor in black for Borderless and switched the
	// display for Exclusive (code-review C391). Exclusive goes through the
	// swapchain, not the window, so this is the guard that covers it.
	if (!m_window.IsHidden()) ApplyDisplaySettings();
}

Game::~Game() {
	// The AudioEngine outlives Game (constructed before it in Main), but its
	// playback references SoundBank sample memory owned HERE — silence every
	// voice before that memory goes away, or the mixer thread reads freed
	// buffers on a quit mid-sound.
	m_audio.StopAll();
	// The shared dialog icons hold SRV slots on the GraphicsDevice, which
	// outlives Game (Main owns it) but not by much — drop them HERE, while the
	// device is certainly alive, rather than leaving a namespace-scope texture
	// to destruct at exit against a dead device. See AssetUtil::CloseIcon.
	ReleaseSharedIcons();
}

// ============================================================================
// The world's lifetime (docs/world-on-demand.md) - LoadWorld, the deferred
// switch, UnloadWorld. (The callback wiring is Game_Wiring.cpp; each dev-console
// Register*Commands lives in its own file - see Game.h.)
// ============================================================================

bool Game::LoadWorld(const std::string& folder) {
	const std::string root = paths::Asset("projects");
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), folder) == found.end()) {
		log::Warn("load world: no world '{}' under {}", folder, root);
		return false;
	}
	UnloadWorld();
	m_project = Project::Load(Project::FolderFor(root, folder));
	log::Info("Opening world '{}'", folder);
	// The shared style library, re-read with each world (it is a few small
	// files, and a library edited outside the game is picked up this way).
	m_library.Load(paths::Asset("library"));
	m_mapEditor.SetLibrary(&m_library);
	m_mapEditor.SetCurrentStyle({}); // another world's style means nothing here
	// The world tier: text-only and tiny, so it loads here rather than as a
	// staged task, and an absent world map is legal (docs/world-map.md).
	LoadWorldMap();
	m_world = std::make_unique<DungeonWorld>(m_device, m_renderer, m_audio, m_sounds,
											 m_settings, m_project, m_threads);
	m_mapView.SetWorld(m_world.get());
	m_mapEditor.SetWorld(m_world.get());
	// A new world's edit counter starts again at zero, so the cached live
	// findings could match its revision by coincidence: force a fresh check.
	m_liveValid = false;
	m_liveIssues.clear();
	// `hasWorld` hides the player map's world-page toggle in a project that is
	// all dungeon, rather than dimming it.
	m_mapView.hasWorld = m_worldMap.has_value();
	WireWorldCallbacks();
	// The world joins the editor's ONE undo history (his answer: a step that
	// spans tiers undoes as one thing). Borrowed by pointer, like the roster.
	m_world->SetWorldForUndo(&m_worldMap);
	// ...and so does the project's opening square, which dragging the stair it
	// lands on carries along (DungeonWorld::MoveArrivals).
	m_world->SetOpeningForUndo(&m_project.startLevel, &m_project.startX, &m_project.startZ);
	m_world->GetParty().SetKeys(m_settings.moveKeys);
	m_world->GetParty().SetLook(m_settings.look);
	m_world->GetParty().SetHeadBob(m_settings.headBob);
	m_world->SetRoster(&m_characters); // combat drains these; reset in place
	m_world->SetCursorItem(&m_heldItem); // a torch on the cursor burns and lights
	// Doors, levers and stairs read the game's flags, and levers write them.
	m_world->SetFlagStore(&m_worldState);
	// AFTER SetRoster, not before: the pace rule reads the roster through the
	// world (conditioning feeds it), so it has nothing to average until then.
	ApplyPartySpeed();
	return true;
}

void Game::ApplyPendingWorld() {
	if (!m_pendingWorld) return;
	const PendingWorld p = std::exchange(m_pendingWorld, std::nullopt).value();
	if (!LoadWorld(p.folder)) {
		// The world went while the ask was pending (deleted in another window):
		// stay where we are, which is the title if the old one was unloaded.
		m_state = AppState::Menu;
		m_ui.ResetToMainPage();
		m_editorOnArrival = false; // the game it was waiting for is not coming
		return;
	}
	if (p.partyPage) OpenPartyCreation(p.folder);
	else if (p.savePath.empty()) m_ui.onStartNewGame();
	else m_ui.onLoadSave(p.savePath);
}

void Game::UnloadWorld() {
	if (!m_world) return;
	// In-flight frames still reference the world's meshes and textures, and
	// the icon banks below point into its baked thumbnails.
	m_device.WaitIdle();
	// Everything that borrows from the world or its project goes first.
	m_mapView.Close();
	m_typeDialog.Close();
	m_assetDialog.Close();
	m_assetPicker.Close();
	m_monsterDialog.Close();
	m_balanceDialog.Close();
	m_levelSettingsDialog.Close();
	m_worldSettingsDialog.Close();
	m_worldsDialog.Close();
	m_newWorldDialog.Close();
	m_validateDialog.Close();
	m_generateDialog.Close();
	CloseInspectors();
	// The editor's session is this world's: a route being laid names one of its
	// monsters by an id the next world hands to another, and a selection or a
	// stroke is of its cells (code-review C233 - an Esc in the next world used
	// to reopen the inspector on the stale id, and its Save wrote that world's
	// .ent).
	m_mapEditor.ResetSession();
	// Borrowed GPU pointers into the world's kind caches.
	ForgetMonsterPreview();
	m_previewAnim = anim::Animator();
	// Keyed by the old world's catalog ids — a new world made from this one
	// shares them, so a stale entry would show the wrong icon, not a gap.
	m_itemIcons.byType.clear();
	m_slotIcons.byType.clear();
	m_useIcons.byType.clear();
	m_itemIconPlaceholders.clear();
	m_slotIconTextures.clear();
	m_useIconTextures.clear();
	m_heldItem.reset();
	// A member's effects point at the old world's effect kinds.
	for (Character& c : m_characters) c.effects.clear();
	m_mapView.SetWorld(nullptr);
	m_mapEditor.SetWorld(nullptr);
	m_world.reset(); // its AI workers stop with it
	m_worldMap.reset();
	m_worldState = {};
	m_project = Project{};
	m_gameLoaded = false;
	log::Info("World unloaded");
}

void Game::BuildBootLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_loadQueue.Add(loc::Tr("load.echoes"), [this] {
		m_sounds.Load();
		// Build the voice pool now, split across the bank's formats, so the
		// first sounds of a game reuse voices instead of creating them in a
		// steady frame (AudioEngine::Reserve).
		std::array<std::pair<u32, u32>, 9> formats{};
		size_t formatCount = 0;
		for (const assets::SoundData* s : m_sounds.All()) {
			if (s->samples.empty()) continue;
			const std::pair<u32, u32> f{s->channels, s->sampleRate};
			if (std::find(formats.begin(), formats.begin() + formatCount, f) ==
				formats.begin() + formatCount)
				formats[formatCount++] = f;
		}
		for (size_t i = 0; i < formatCount; ++i)
			m_audio.Reserve(formats[i].first, formats[i].second,
							audio::AudioEngine::kMaxVoices / formatCount);
	}, "sounds");
	m_loadQueue.Add(loc::Tr("load.title_art"), [this] { m_ui.LoadTitleArt(); }, "title art");
	// The portrait CATALOG (text, no images) comes with the menu: a party is
	// made before any game loads, and its faces are checked against it.
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadPortraitCatalog(); },
					"portrait catalog");
}

void Game::BuildGameLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_world->AppendLoadTasks(m_loadQueue);
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadPortraits(); }, "portraits");
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadHitSplats(); }, "hit splats");
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadItemIcons(); }, "item icons");
	m_loadQueue.Add(
		loc::Tr("load.hud"),
		[this] {
			m_ui.BuildHud();
			log::Info("Game loaded: {}x{} dungeon, {} torches, {} monsters",
					  m_world->Map().Width(), m_world->Map().Height(),
					  m_world->Map().Sconces().size(), m_world->MonsterCount());
		},
		"hud");
}

void Game::BeginLevelTransition(const std::string& stem, int x, int z,
								std::optional<Direction> facing, bool stashCurrent) {
	m_world->BeginLevelLoad(stem, stashCurrent); // swap + reset per-level state now
	m_loadQueue.Clear();          // re-stage only the world rebuild (portraits /
	m_loadQueue.SetDoneLabel(loc::Tr("load.done")); // HUD persist across levels)
	m_world->AppendLoadTasks(m_loadQueue);
	m_pendingLevelX = x;
	m_pendingLevelZ = z;
	m_pendingLevelFacing = facing;
	// Ordinary transitions arrive square-on; a save load overrides this right
	// after the call (the only path that carries a free-look offset across levels).
	m_pendingLookYaw = m_pendingLookPitch = 0.0f;
	m_pendingLooking = false;
	m_pendingWorldMap = m_pendingWorldPark = false;
	m_state = AppState::LoadingLevel;
	m_stateFrameMark = m_framesRendered;
}

bool Game::RunLoadTasks() {
	const bool wasDone = m_loadQueue.Done();
	if (m_framesRendered > m_stateFrameMark) m_loadQueue.RunOne();
	const bool done = m_loadQueue.Done();
	if (done && !wasDone) LogLoadStats(); // the frame the last task landed
	return done;
}

// The load-time counterpart to the steady-state rule: staged loading is ALLOWED
// to allocate, but until now nobody had measured how much. Sorted by time, since
// that is what a player feels; the allocation columns say where the time went.
void Game::LogLoadStats(bool echoToConsole) {
	const std::vector<LoadQueue::TaskStat>& stats = m_loadQueue.Stats();
	if (stats.empty()) {
		if (echoToConsole) m_console.Print("no load has run yet");
		return;
	}
	auto say = [this, echoToConsole](std::string line) {
		if (echoToConsole) m_console.Print(line);
		log::Info("{}", line);
	};

	double totalMs = 0.0;
	u64 totalAllocs = 0, totalBytes = 0;
	for (const LoadQueue::TaskStat& s : stats) {
		totalMs += s.ms;
		totalAllocs += s.allocs;
		totalBytes += s.bytes;
	}

	// Sorted by cost, not run order — the table is read top-down for suspects.
	std::vector<const LoadQueue::TaskStat*> byCost;
	byCost.reserve(stats.size());
	for (const LoadQueue::TaskStat& s : stats) byCost.push_back(&s);
	std::ranges::sort(byCost, [](const LoadQueue::TaskStat* a, const LoadQueue::TaskStat* b) {
		return a->ms > b->ms;
	});

	const ProcessMemory mem = QueryProcessMemory();
	say(std::format("--- load: {} tasks, {:.0f} ms, {} allocs, {:.1f} MB requested "
					"(working set {:.0f} MB, peak {:.0f} MB) ---",
					stats.size(), totalMs, totalAllocs,
					static_cast<double>(totalBytes) / (1024.0 * 1024.0),
					mem.workingSetMB, mem.peakWorkingSetMB));
	for (const LoadQueue::TaskStat* s : byCost)
		say(std::format("  {:>8.1f} ms  {:>8} allocs  {:>9.2f} MB  {}", s->ms, s->allocs,
						static_cast<double>(s->bytes) / (1024.0 * 1024.0), s->name));
}

void Game::LoadPortraitCatalog() {
	m_portraitCatalog.Load(paths::Asset("portraits\\portraits.cat"));
	if (m_portraitCatalog.Empty())
		log::Warn("portraits.cat is missing or empty - members keep their ids, but "
				  "nothing can be picked");
	else
		log::Info("portraits.cat: {} portraits", m_portraitCatalog.Entries().size());
	if (PortraitPicker* picker = m_ui.Portraits()) picker->SetCatalog(m_portraitCatalog);
}

void Game::LoadPortraits() {
	// The catalog came with the boot load; a world's load re-reads it only if
	// that found nothing (a missing file stays a warning, not a crash).
	if (m_portraitCatalog.Empty()) LoadPortraitCatalog();
	// Forget what is loaded so every slot reloads (SyncPortraits drains first).
	std::ranges::fill(m_portraitIds, std::nullopt);
	SyncPortraits();
}

void Game::SyncPortraits() {
	const size_t n = m_characters.size();
	bool drained = false;
	const auto drain = [&] {
		if (!drained) m_device.WaitIdle();
		drained = true;
	};
	if (m_portraitTextures.size() > n) drain(); // a shrinking roster frees some
	m_portraitTextures.resize(n);
	m_portraitIds.resize(n);
	for (size_t i = 0; i < n; ++i) {
		Character& member = m_characters[i];
		if (m_portraitIds[i] != member.portraitId) {
			if (m_portraitTextures[i]) drain();
			m_portraitTextures[i].reset();
			if (!member.portraitId.empty()) {
				m_portraitTextures[i] = TryLoadTextureFile(
					m_device, paths::Asset("portraits\\" + member.portraitId));
				if (!m_portraitTextures[i])
					log::Warn("missing portrait {} for {} - falling back to the initial "
							  "tile (tools\\FetchPortraits.ps1 installs the set)",
							  member.portraitId, member.name);
			}
			m_portraitIds[i] = member.portraitId;
		}
		member.portrait = m_portraitTextures[i].get();
	}
}

void Game::OpenPortraitPicker(size_t member) {
	m_ui.OpenPortraitPicker(member);
	// Opening fills strings and a filter list - allocation, in what may be a
	// guarded frame (a click on the sheet). Like any overlay opening, the frame
	// is not a steady one; while it stays open SteadyStateFrame keeps it so.
	if (!m_ui.PortraitPickerOpen()) return;
	OverlayOpenedThisFrame();
	// After the excuse: a log line formats a string. tools\InGameTest.ps1 reads
	// it to know its picker sweep audited the picker, not the sheet under it.
	log::Info("portrait picker: open for {}", m_characters[member].name);
}

bool Game::SetPortrait(size_t member, const std::string& id) {
	if (member >= m_characters.size() || !m_portraitCatalog.Find(id)) return false;
	m_characters[member].portraitId = id;
	SyncPortraits();
	return true;
}

void Game::LoadHitSplats() {
	// Three severity icons, drawn over a struck member's portrait. Committed
	// source PNGs under assets/ui/. A missing icon just leaves that severity null.
	static const char* kStems[3] = {"hit_splat_small", "hit_splat_med",
									"hit_splat_hard"};
	for (int i = 0; i < 3; ++i) {
		m_hitSplatTextures[i] =
			TryLoadTextureFile(m_device, paths::Asset(std::string("ui\\") + kStems[i]));
		if (!m_hitSplatTextures[i])
			log::Warn("missing {}.png — no hit splat for that severity", kStems[i]);
		m_hitSplats.icon[i] = m_hitSplatTextures[i].get();
	}
}

// Builds a tiny solid-colour RGBA texture (a flat placeholder icon). The mip
// chain is generated on the spot (fine for a small runtime texture); sRGB so the
// tint matches the linear CategoryTint colour seen on the floor mesh.
static std::unique_ptr<gfx::Texture> MakeSolidIcon(gfx::GraphicsDevice& device,
												   const Vec4& color) {
	constexpr u32 kSize = 16;
	assets::ImageData img;
	img.width = kSize;
	img.height = kSize;
	img.pixels.resize(static_cast<size_t>(kSize) * kSize * 4);
	const auto enc = [](float c) {
		return static_cast<u8>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
	};
	const u8 rgba[4] = {enc(color.x), enc(color.y), enc(color.z), enc(color.w)};
	for (size_t p = 0; p < img.pixels.size(); p += 4) {
		img.pixels[p + 0] = rgba[0];
		img.pixels[p + 1] = rgba[1];
		img.pixels[p + 2] = rgba[2];
		img.pixels[p + 3] = rgba[3];
	}
	return std::make_unique<gfx::Texture>(device, img, /*srgb=*/true);
}

void Game::LoadItemIcons() {
	// Every item kind is built here, not on its first drop (see the method).
	m_world->PreloadItemKinds();
	// One element-tinted icon per symbol, keyed by the rune's catalog id
	// (rune_fire → rune_icon_fire). PNG only (like the splats). Drawn on the
	// cursor when a tablet is held, and in the hand slots / inventory.
	for (u32 i = 0; i < kSymbolCount; ++i) {
		const auto sym = static_cast<SpellSymbol>(i);
		const std::string id(RuneItemId(sym));
		m_runeIconTextures[i] = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\rune_icon_{}", SymbolId(sym))));
		if (!m_runeIconTextures[i])
			log::Warn("missing rune_icon_{}.png — no cursor icon", SymbolId(sym));
		m_itemIcons.byType[id] = m_runeIconTextures[i].get();
		// The Magic window's glowing rune: glyph + halo (tools/BuildRuneGlow.py).
		// Linear, not sRGB: they are white masks the draw tints.
		static_assert(kSymbolCount <= ItemIconBank::kRuneSlots);
		m_runeGlyphTextures[i] = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\rune_glyph_{}", SymbolId(sym))));
		m_runeGlowTextures[i] = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\rune_glow_{}", SymbolId(sym))));
		m_itemIcons.runeGlyph[i] = m_runeGlyphTextures[i].get();
		m_itemIcons.runeGlow[i] = m_runeGlowTextures[i].get();
	}
	// A rune in an item socket (pack, doll, cursor) is its CARVED TABLET, baked
	// from the 3D model like any item, with its school's glow laid over the
	// groove; the hand boxes and spell controls keep the glyph alone (Michael).
	// The flat rune_icon PNG above stays the fallback while a tablet is missing.
	{
		std::array<const gfx::Texture*, kSymbolCount> tablets{};
		bool all = m_world->RuneFaceUv(m_itemIcons.runeFaceLo, m_itemIcons.runeFaceHi);
		for (u32 i = 0; all && i < kSymbolCount; ++i)
			all = (tablets[i] = m_world->ItemIconFor(
					   std::string(RuneItemId(static_cast<SpellSymbol>(i))))) != nullptr;
		m_itemIcons.runeTablets = all;
		if (all)
			for (u32 i = 0; i < kSymbolCount; ++i)
				m_itemIcons.byType[std::string(RuneItemId(static_cast<SpellSymbol>(i)))] =
					tablets[i];
		else
			log::Warn("rune tablet icons not baked - runes keep their flat icon");
	}
	// Non-rune items: a model item uses its baked 3D thumbnail (rendered once by
	// DungeonWorld; the same texture feeds every slot/grid/cursor instance);
	// model-less items keep a generated solid category-tint placeholder.
	for (const CatalogEntry* defp : m_project.AllItems()) {
		const CatalogEntry& def = *defp;
		const std::string category = def.Get("category", "misc");
		if (category == "rune") continue; // runes use their element PNG above
		if (const gfx::Texture* model = m_world->ItemIconFor(def.id)) {
			m_itemIcons.byType[def.id] = model;
			continue;
		}
		m_itemIconPlaceholders.push_back(MakeSolidIcon(m_device, CategoryTint(category)));
		m_itemIcons.byType[def.id] = m_itemIconPlaceholders.back().get();
	}
	// A BURNING item's flame (Michael: the lit torch in a hand had none): where
	// it stands on the icon, and the sprites it is drawn with. White masks the
	// draw tints, so linear.
	m_itemIcons.flameAt.clear();
	m_itemIcons.flameTint.clear();
	for (const CatalogEntry* defp : m_project.AllItems()) {
		if (Vec2 uv; m_world->ItemFlameUv(defp->id, uv)) {
			m_itemIcons.flameAt[defp->id] = uv;
			log::Info("item icon {}: flame at {:.2f},{:.2f}", defp->id, uv.x, uv.y);
		}
		if (Vec3 tint; m_world->ItemFlameTint(defp->id, tint))
			m_itemIcons.flameTint[defp->id] = tint;
	}
	if (!m_flameTexture) {
		m_flameTexture = TryLoadTextureFile(m_device, paths::Asset("ui\\flame"));
		m_flameGlowTexture = TryLoadTextureFile(m_device, paths::Asset("ui\\glow_radial"));
	}
	m_itemIcons.flame = m_flameTexture.get();
	m_itemIcons.flameGlow = m_flameGlowTexture.get();
	// Carry weights + categories for every catalog item (load sum; pack check).
	m_itemWeights.byType.clear();
	m_itemCategories.byType.clear();
	m_itemCategories.capacityByType.clear();
	m_itemCategories.acceptsByType.clear();
	m_itemCategories.holdableTypes.clear();
	// Splits a whitespace/comma list (the catalog `accepts` field) into tokens.
	const auto splitList = [](const std::string& s) {
		std::vector<std::string> out;
		std::string tok;
		for (char c : s) {
			if (c == ' ' || c == '\t' || c == ',') {
				if (!tok.empty()) { out.push_back(tok); tok.clear(); }
			} else {
				tok += c;
			}
		}
		if (!tok.empty()) out.push_back(tok);
		return out;
	};
	for (const CatalogEntry* defp : m_project.AllItems()) {
		const CatalogEntry& def = *defp;
		m_itemWeights.byType[def.id] = def.GetFloat("weight", 0.0f);
		m_itemCategories.byType[def.id] = def.Get("category", "misc");
		int capacity = static_cast<int>(def.GetFloat("capacity", 0.0f));
		if (capacity > kMaxPackSlots) { // a bag's slots are fixed storage (PackSlots)
			log::Warn("item '{}': capacity {} is more than the {} slots a bag can have"
					  " - clamped", def.id, capacity, kMaxPackSlots);
			capacity = kMaxPackSlots;
		}
		m_itemCategories.capacityByType[def.id] = capacity;
		m_itemCategories.acceptsByType[def.id] = splitList(def.Get("accepts", ""));
		if (def.GetBool("holdable", false))
			m_itemCategories.holdableTypes.insert(def.id);
		if (WearSlot w{}; ParseWearSlot(def.Get("wear", ""), w))
			m_itemCategories.wearByType[def.id] = w;
	}

	// Equipment-slot outline silhouettes (slot_<type>.png), the ghost behind an
	// empty doll slot. PNG only, like the rune icons; a missing one just draws no
	// ghost for that slot.
	for (const char* type : {"head", "body", "legs", "feet", "cloak", "amulet",
							 "hand", "ring"}) {
		auto tex = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\slot_{}", type)));
		if (!tex) {
			log::Warn("missing slot_{}.png — no empty-slot outline", type);
			continue;
		}
		m_slotIconTextures.push_back(std::move(tex));
		m_slotIcons.byType[type] = m_slotIconTextures.back().get();
	}

	// Hand-use pictures: every ui/use_<verb>.png, keyed by the verb, so a new
	// picture needs a file and no list here. A verb with none just draws the
	// set tint alone.
	std::error_code ec;
	for (const auto& entry :
		 std::filesystem::directory_iterator(paths::Asset("ui"), ec)) {
		const std::filesystem::path& file = entry.path();
		const std::string stem = file.stem().string();
		if (file.extension() != ".png" || !stem.starts_with("use_")) continue;
		auto tex = TryLoadTextureFile(m_device, paths::Asset("ui\\" + stem));
		if (!tex) {
			log::Warn("could not load {}.png - no hand-use picture", stem);
			continue;
		}
		m_useIconTextures.push_back(std::move(tex));
		m_useIcons.byType[stem.substr(4)] = m_useIconTextures.back().get();
	}
}

// ============================================================================
// State transitions
// ============================================================================

void Game::ResetRoster(const std::vector<Character>* party) {
	// Element-wise when the size holds, so the storage the party-bar panels and
	// the sheet resolve into stays put. Each member takes its portrait id from
	// the new party; SyncPortraits re-points the textures, reloading only a slot
	// whose id changed. A party of another SIZE (party creation) replaces the
	// vector - the world holds a pointer to the vector, not to its members.
	const std::vector<Character> fresh = party ? *party : CreateDefaultParty();
	const bool resized = fresh.size() != m_characters.size();
	if (resized) {
		m_characters = fresh;
	} else {
		for (size_t i = 0; i < m_characters.size(); ++i) m_characters[i] = fresh[i];
	}
	SyncPortraits();
	// CreateDefaultParty seeds the derived maxima at k=1; re-derive under the
	// project's live balance knobs (fresh members are at full, so top them up).
	m_world->RecomputePartyMaxima();
	// Fresh members carry an EMPTY skill map, so the first step of the run would
	// insert "conditioning" into it — a steady-state allocation. Seed the whole
	// trainable set now, while allocating is free.
	m_world->SeedPartySkills();
	const Balance& bal = m_world->GetBalance();
	for (Character& member : m_characters) {
		member.health = member.maxHealth;
		member.stamina = member.maxStamina;
		member.mana = member.maxMana;
		// Supplies start FULL and come from the knobs, not from Character's
		// defaults — a project that raises food_max should have its new parties
		// begin at the raised value, not at whatever the header happened to say.
		member.food = bal.foodMax;
		member.water = bal.waterMax;
	}
	// The default four take the Settings palette; a created party keeps the
	// colours its members were made with (Michael: colour is the character's).
	if (!party) ApplyMemberColors();
	// The roster these are is not the roster the one-pipeline check was watching
	// (Game/DamageLedger.h) — same storage, replaced contents.
	m_world->RebaseDamageLedger();
	if (resized) m_ui.RebuildForRoster();
}

void Game::ApplyMemberColors() {
	for (size_t i = 0; i < m_characters.size() && i < kMemberColorCount; ++i)
		m_characters[i].portraitColor = m_settings.memberColors[i];
}

// Puts the party in `level` at `x,z` (-1,-1 = the level's own start cell).
//
// Returns TRUE when a level LOAD was staged, which the caller must treat as
// "we are done here — the LoadingLevel done-handler finishes the job". False
// means the world already held that level and the party was simply placed, so
// the caller carries on and finishes the new game itself.
//
// THE DISTINCTION MATTERS MORE THAN IT LOOKS. Staging a load that was not
// needed leaves the game in LoadingLevel for a beat, and the dev console is
// GATED OFF during a load — which silently broke the in-game test suite, whose
// `levelcheck` arrived while the redundant load was still in flight and was
// answered by nothing at all.
bool Game::OpenInLevel(const std::string& level, int x, int z) {
	if (m_world->CurrentLevel() != level) {
		BeginLevelTransition(level, x, z, std::nullopt, /*stashCurrent=*/false);
		return true;
	}
	const DungeonMap& map = m_world->Map();
	const int px = x >= 0 ? x : map.StartX(), pz = z >= 0 ? z : map.StartZ();
	m_world->PlacePartyAt(px, pz, m_world->ArrivalFacingAt(px, pz));
	return false;
}

void Game::StartNewGame() {
	m_world->ResetForNewGame();
	ResetWorldState();
	// Fresh members carry empty inventories + no known symbols. The party is the
	// one party creation made, once (consumed here), else the default four.
	ResetRoster(m_startParty ? &*m_startParty : nullptr);
	m_startParty.reset();
	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// WHERE THE GAME BEGINS (docs/world-map.md), in three cases:
	//
	//   1. THE HARNESS asks for a level, and the manifest says which one — not
	//      "whichever is first", so the suites do not move when the level list
	//      is reordered, and so more harness levels can join the one there is.
	//   2. A STARTER DUNGEON named in the manifest, with its level and cell.
	//      The game's opening is just another way IN, carrying its own
	//      destination exactly as a world-map doorway does, because a dungeon
	//      has no start of its own.
	//   3. Otherwise the WORLD MAP, out in the open, looking for a way down.
	//
	// A project with no world and no starter falls through to the first level,
	// which is what keeps the world an OPTIONAL tier rather than a requirement.
	m_worldState.atLocation.clear(); // begun here, not entered from anywhere
	m_worldState.onWorldMap = false;

	if (m_harnessOpensInLevel && !m_project.evalLevel.empty()) {
		if (OpenInLevel(m_project.evalLevel, -1, -1)) return;
		log::Info("New game started on the harness ground ({})",
				  m_project.evalLevel);
	} else if (!m_project.startDungeon.empty() && !m_harnessOpensInLevel) {
		const std::string level =
			m_project.startLevel.empty()
				? (m_project.levels.empty() ? std::string("level1")
											: m_project.levels.front())
				: m_project.startLevel;
		if (OpenInLevel(level, m_project.startX, m_project.startZ)) return;
		log::Info("New game started in {} ({} at {},{})", m_project.startDungeon,
				  level, m_project.startX, m_project.startZ);
	} else if (m_worldMap && !m_harnessOpensInLevel) {
		// Reveal what the party can see from where it stands — which discovers
		// a location only if one is right there. Everything else has to be
		// FOUND, and that is the design, not an oversight: a new game opens on
		// a map that is mostly fog with nowhere marked on it, and walking is
		// how you learn where the dungeons are.
		RevealAround(m_worldState.x, m_worldState.z);
		SetOnWorldMap(true);
		m_ui.ClearLog();
		m_ui.AddLogLine(loc::View("world.begin"));
		log::Info("New game started on the world map at {},{}", m_worldState.x,
				  m_worldState.z);
		return;
	} else {
		const std::string first = m_project.levels.empty()
									  ? std::string("level1")
									  : m_project.levels.front();
		if (OpenInLevel(first, -1, -1)) return;
		log::Info("New game started (already on {})", first);
	}

	m_ui.ClearLog();
	m_ui.AddLogLine(loc::View("log.descend"));
	m_ui.AddLogLine(loc::View("log.shuffle"));
	m_ui.AddLogLine(m_settings.MoveKeysHelp());

	m_ui.ResetHudStatus();
	m_state = AppState::Playing;
	log::Info("New game started");
}

bool Game::SaveGame(const std::string& name) {
	if (!m_gameLoaded) {
		log::Warn("SaveGame: no game loaded");
		return false;
	}
	// NOT INSIDE AN ENCOUNTER. What to do about a save mid-encounter is a
	// question Michael parked (docs/world-map.md), and this is not an answer to
	// it — it is the guard that stops the absence of an answer becoming a
	// corrupt file. An encounter level exists only in memory; a save naming it
	// would reload into a level that no longer exists and cannot be rebuilt.
	// Refusing is recoverable. Writing it is not.
	if (InEncounter()) {
		log::Warn("SaveGame: refusing to save inside a random encounter — the "
				  "level exists only in memory (docs/world-map.md)");
		if (m_world->onMessage) m_world->onMessage(loc::View("world.nosave"));
		return false;
	}
	SaveData data;
	data.worldName = m_project.FolderName();
	data.name = name;
	data.timestamp = std::format("{:%Y-%m-%d %H:%M:%S}",
								 std::chrono::floor<std::chrono::seconds>(
									 std::chrono::system_clock::now()));
	// An item on the cursor is party-level state — save it as such, leaving the
	// live session's held item untouched (restored to the cursor on load).
	if (m_heldItem) data.heldItem = ItemToken(*m_heldItem, m_heldItem.Charge());
	// A thrown item in the air is not saved as a flight: CaptureState writes it
	// as the floor item it will be, where it would come down, and the flight
	// carries on in the game being played. It used to be LANDED here first, and
	// a shattering flask landed is a flask going off - in or beside the party's
	// square, with the save then holding the damage (code-review C47).

	data.world = m_worldState; // the global tier (docs/world-map.md)
	// On the world map the level underneath is not where the party IS: a parked
	// one is written from the store, and a baseline never entered has no state.
	m_world->CaptureState(data, /*includeLive=*/!m_worldState.onWorldMap);
	for (const Character& member : m_characters) {
		SaveData::CharState c{member.health, member.maxHealth, member.stamina,
							  member.maxStamina, member.mana, member.maxMana,
							  member.knownSymbols};
		// equipment[] now includes the weapon hands (EquipSlot::LeftHand/RightHand).
		for (const ItemSlot& s : member.inventory.equipment)
			c.equipment.push_back(ItemToken(s.typeId, s.charge)); // `id#charge` (Inventory.h)
		// Pack row: the container ids + each pack's contents + the selected pack.
		c.selectedPack = member.inventory.selectedPack;
		for (const Pack& p : member.inventory.packs) {
			c.packTypes.push_back(p.typeId);
			std::vector<std::string> items;
			for (const ItemSlot& s : p.contents) items.push_back(ItemToken(s.typeId, s.charge));
			c.packContents.push_back(std::move(items));
		}
		// The member's remembered per-hand, per-item default uses (hand
		// left-click action) and each hand's cast-recency list.
		for (size_t hand = 0; hand < 2; ++hand) {
			member.useDefaults[hand].ForEach([&](std::string_view item, std::string_view cmd) {
				c.useDefaults[hand].emplace_back(std::string(item), std::string(cmd));
			});
			member.spellMru[hand].ForEach(
				[&](std::string_view id) { c.mruSpells[hand].emplace_back(id); });
		}
		// Spells learned by first successful cast.
		member.learnedSpells.ForEach(
			[&](std::string_view id) { c.learnedSpells.emplace_back(id); });
		// Active status effects — the EFFECT ID names the kind now (older saves
		// stored the category token; EffectBook::FindLegacy maps those forward
		// on load). The name key still rides along for readability only: an
		// effect names itself through its kind.
		for (const fx::Inst& e : member.effects)
			c.effects.push_back({std::string(e.Id()), SymbolId(e.school),
								 e.timeLeft, e.duration, e.magnitude, e.source,
								 std::string(e.NameKey())});
		// Skills, stat-creep pools, and the five attributes (they grow now).
		// Zero-XP entries are the seed (DungeonWorld::SeedPartySkills), not
		// progress — writing them would put a dozen dead lines per member in every
		// save, and the seed is rebuilt on load anyway.
		for (const auto& [id, xp] : member.skillXp)
			if (xp > 0.0f) c.skills.emplace_back(id, xp);
		// Written as NAME/value pairs still, so the save file is unchanged and
		// stays readable when the stat set moves — the array is an in-memory
		// shape, not a file format. A zero pool writes nothing, as before.
		for (int s = 0; s < kStatCount; ++s)
			if (member.statProgress[static_cast<size_t>(s)] != 0.0f)
				c.statProgress.emplace_back(std::string(kStats[static_cast<size_t>(s)].id),
											member.statProgress[static_cast<size_t>(s)]);
		c.hasAttrs = true;
		c.strength = member.strength;
		c.dexterity = member.dexterity;
		c.vitality = member.vitality;
		c.willpower = member.willpower;
		c.intelligence = member.intelligence;
		// The resource bases (v17) — the authored half of the derived maxima.
		c.hasBases = true;
		c.baseHealth = member.baseHealth;
		c.baseStamina = member.baseStamina;
		c.baseMana = member.baseMana;
		c.dead = member.dead; // the overkill flag (v18)
		c.offenseShare = member.offenseShare; // the stance (v23)
		c.hasSupplies = true; // food and water (v25)
		c.food = member.food;
		c.water = member.water;
		c.portrait = member.portraitId;
		// Who they are (party creation).
		c.name = member.name;
		c.race = member.raceId;
		c.hasColor = true;
		c.color[0] = member.portraitColor.x;
		c.color[1] = member.portraitColor.y;
		c.color[2] = member.portraitColor.z;
		c.color[3] = member.portraitColor.w;
		c.hasPace = true;
		c.pace = member.moveSpeed;
		data.characters.push_back(std::move(c));
	}
	return WriteSave(data, SaveSlotPath(name));
}

bool Game::LoadGame(const std::string& path) {
	if (!m_gameLoaded) {
		log::Warn("LoadGame: dungeon not loaded yet");
		return false;
	}
	auto data = ReadSave(path);
	if (!data) {
		log::Warn("LoadGame: could not read {}", path);
		return false;
	}

	// Rebuild the baseline (party home, fog cleared, monsters at spawn, palette
	// reset), then lay the save on top.
	m_world->ResetForNewGame();
	ResetRoster();
	// THE PARTY'S SIZE (party creation). A save that names one cuts the default
	// four down to it before anything is laid on top; the members' own lines then
	// say who they are. A save without one (older than party creation) is four.
	const bool resized = data->rosterSize >= party::kMinMembers &&
						 data->rosterSize <= party::kMaxMembers &&
						 data->rosterSize != m_characters.size();
	if (resized) m_characters.resize(data->rosterSize);
	// The global tier is stored WHOLE rather than as a diff, so it is simply
	// taken (a fresh baseline first, so a save that predates a field gets the
	// new-game value for it rather than the last session's).
	ResetWorldState();
	m_worldState = data->world;
	// Restore the cursor-held tablet (empty = nothing carried).
	if (!data->heldItem.empty()) {
		ItemSlot held;
		ItemFromToken(data->heldItem, held);
		m_heldItem.Set(held.typeId, held.charge);
	} else {
		m_heldItem.reset();
	}
	for (size_t i = 0; i < m_characters.size() && i < data->characters.size(); ++i) {
		const SaveData::CharState& c = data->characters[i];
		// WHO THEY ARE first (party creation) - each absent in an older save,
		// which keeps the default member's. The resists are not saved: they come
		// back from the race.
		if (!c.name.empty()) m_characters[i].name = c.name;
		if (!c.race.empty()) m_characters[i].raceId = c.race;
		if (c.hasColor)
			m_characters[i].portraitColor = {c.color[0], c.color[1], c.color[2], c.color[3]};
		if (c.hasPace) m_characters[i].moveSpeed = c.pace;
		ApplyRaceResists(m_characters[i]);
		m_characters[i].health = c.health;     m_characters[i].maxHealth = c.maxHealth;
		m_characters[i].stamina = c.stamina;   m_characters[i].maxStamina = c.maxStamina;
		m_characters[i].mana = c.mana;         m_characters[i].maxMana = c.maxMana;
		m_characters[i].knownSymbols = c.knownSymbols;
		// Inventory (ResetRoster gave a fresh one; lay the save's items back in).
		Inventory& inv = m_characters[i].inventory;
		for (size_t e = 0; e < c.equipment.size() && e < static_cast<size_t>(kEquipCount); ++e)
			ItemFromToken(c.equipment[e], inv.equipment[e]);
		// Restore the pack row (container ids + each pack's contents + selection).
		for (size_t p = 0; p < c.packTypes.size() && p < static_cast<size_t>(kPackRowSlots); ++p) {
			inv.packs[p].typeId = c.packTypes[p];
			const std::vector<std::string> items =
				p < c.packContents.size() ? c.packContents[p] : std::vector<std::string>{};
			PackSlots& slots = inv.packs[p].contents;
			slots.resize(items.size()); // clears; clamps at kMaxPackSlots
			if (items.size() > slots.size())
				log::Warn("save: pack {} of {} holds {} slots, more than the {} a bag can have"
						  " - the rest are dropped",
						  p, m_characters[i].name, items.size(), slots.size());
			for (size_t s = 0; s < slots.size(); ++s) ItemFromToken(items[s], slots[s]);
		}
		if (c.selectedPack >= 0 && c.selectedPack < kPackRowSlots)
			inv.selectedPack = c.selectedPack;
		// Restore the remembered per-hand default uses (ResetRoster left the
		// maps empty) and each hand's cast-recency list.
		for (size_t hand = 0; hand < 2; ++hand) {
			for (const auto& [item, cmd] : c.useDefaults[hand])
				m_characters[i].useDefaults[hand].Set(item, cmd);
			// Add appends, so the saved newest-first order is kept.
			m_characters[i].spellMru[hand].Clear();
			for (const std::string& id : c.mruSpells[hand])
				m_characters[i].spellMru[hand].Add(id);
		}
		// And the spells learned by casting (likewise reset to empty).
		for (const std::string& id : c.learnedSpells)
			m_characters[i].learnedSpells.Add(id);
		// Restore active status effects (pre-v13 saves carry none). The token
		// is an effect id; FindLegacy also accepts the pre-effects-system
		// category tokens ("ward" + school, "poison", ...). An unresolved one
		// — a newer save, or an effect this project's classes don't have — is
		// skipped, not misread.
		for (const SaveData::EffectState& e : c.effects) {
			SpellSymbol school = SpellSymbol::Fire;
			ParseSymbol(e.school, school);
			const fx::EffectKind* kind = m_world->Effects().FindLegacy(e.id, school);
			if (!kind || e.time <= 0.0f) continue;
			m_characters[i].effects.push_back({kind, school, e.magnitude, e.time,
											   std::max(e.duration, e.time),
											   e.source});
		}
		// Skills, stat-creep pools, and the grown attributes (pre-v15 saves
		// carry none — skills fresh, archetype attributes stand).
		for (const auto& [id, xp] : c.skills) m_characters[i].skillXp[id] = xp;
		// A name the table no longer knows is DROPPED rather than fatal: a stat
		// removed while the set is in flux should cost its creep pool, not the
		// save. (Every other id in a save is treated the same way.)
		for (const auto& [stat, progress] : c.statProgress)
			if (const int s = StatIndex(stat); s >= 0)
				m_characters[i].statProgress[static_cast<size_t>(s)] = progress;
		if (c.hasAttrs) {
			m_characters[i].strength = c.strength;
			m_characters[i].dexterity = c.dexterity;
			m_characters[i].vitality = c.vitality;
			m_characters[i].willpower = c.willpower;
			m_characters[i].intelligence = c.intelligence;
		}
		// Resource bases (v17): the maxima DERIVE from bases + stats, so with
		// the attributes settled, either restore the saved bases or back-solve
		// them from the saved maxima (a pre-v17 save reproduces its maxima
		// exactly under unchanged knobs). RecomputeMaxima then re-derives —
		// current values arrived above and clamp/carry as usual.
		const Balance& bal = m_world->GetBalance();
		const resource::PoolRules pools = bal.Resources();
		Character& member = m_characters[i];
		// The offense stance (v23). A pre-v23 save leaves the CharState at its
		// 1.0 default, which is exactly what those saves meant: all-out.
		member.offenseShare = c.offenseShare;
		// Food and water (v25). A save older than supplies arrives FULL — it
		// predates the mechanic, so its party has not been starving off-screen,
		// and defaulting the new fields to zero would open every existing save
		// onto four members taking starvation damage.
		member.food = c.hasSupplies ? c.food : bal.foodMax;
		member.water = c.hasSupplies ? c.water : bal.waterMax;
		if (c.hasBases) {
			member.baseHealth = c.baseHealth;
			member.baseStamina = c.baseStamina;
			member.baseMana = c.baseMana;
		} else {
			// Run the formula BACKWARDS. This subtracts resource::Contribution
			// rather than a hand-written copy of the aptitude term, so a save
			// old enough to lack bases still reproduces its maxima exactly
			// however many terms the model grows — which is the whole reason
			// that helper is exposed. The copy that used to live here was
			// written when the aptitude WAS the only term.
			const auto solve = [&](resource::Kind kind, float savedMax) {
				return savedMax - resource::Contribution(pools.For(kind),
														 member.Aptitude(kind),
														 member.PracticeLevel(kind));
			};
			member.baseHealth = solve(resource::Kind::Health, c.maxHealth);
			member.baseStamina = solve(resource::Kind::Stamina, c.maxStamina);
			member.baseMana = solve(resource::Kind::Mana, c.maxMana);
		}
		member.RecomputeMaxima(pools);
		// The exhausted latch is a live transient (not saved) — re-derive it
		// from the restored bar so a save made mid-exhaustion resumes winded.
		member.exhausted = member.stamina <= 0.0f;
		// The overkill flag rides the save; the stabilize clock is transient
		// (an unconscious member starts their safe count fresh on load).
		member.dead = c.dead;
		member.stabilize = 0.0f;
		// The portrait. Absent (an older save) keeps the default ResetRoster gave;
		// an id the catalog no longer lists is refused the same way, out loud,
		// rather than drawing the tinted initial for a portrait that was dropped.
		if (!c.portrait.empty()) {
			if (m_portraitCatalog.Find(c.portrait))
				member.portraitId = c.portrait;
			else
				log::Warn("LoadGame: {}'s portrait {} is not in portraits.cat - "
						  "keeping {}", member.name, c.portrait, member.portraitId);
		}
	}
	SyncPortraits();
	if (resized) m_ui.RebuildForRoster(); // a load runs outside the HUD's walk
	m_world->ApplyState(*data); // fills the per-level store + party pose/torch
	// Restored hit points are not writes to explain (Game/DamageLedger.h): the
	// values they replaced belong to a session that is over.
	m_world->RebaseDamageLedger();

	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// A SAVE MADE ON THE WORLD MAP. The level under it was written only if the
	// party had walked out of it (CaptureState leaves a never-entered baseline
	// out), so its presence in the save is exactly "it was parked" — and it has
	// to be parked again, or the next doorway reloads it from its file and the
	// dead stand up. Read here, before the load consumes the entry.
	const bool onWorld = m_worldState.onWorldMap && m_worldMap;
	const bool parked =
		onWorld && std::ranges::any_of(data->levels, [&](const auto& ls) {
			return ls.stem == data->currentLevel;
		});

	// Route to the saved level. If it is the one already active, restore its
	// live state inline; otherwise load it (arriving at the saved pose, without
	// stashing the throwaway baseline) and let the loader finish the restore.
	if (m_world->CurrentLevel() != data->currentLevel) {
		m_ui.ClearLog();
		BeginLevelTransition(data->currentLevel, data->partyX, data->partyZ,
							 static_cast<Direction>(data->partyFacing),
							 /*stashCurrent=*/false);
		// Carry the free-look offset across the level rebuild (PlacePartyAt would
		// otherwise leave the party square-on) — applied once the load finishes.
		m_pendingLookYaw = data->lookYaw;
		m_pendingLookPitch = data->lookPitch;
		m_pendingLooking = data->looking;
		m_pendingWorldMap = onWorld;
		m_pendingWorldPark = parked;
		log::Info("Loaded game from {} (loading {})", path, data->currentLevel);
		return true;
	}

	// Same level: ApplyState already re-layered the look offset (parked at the
	// saved angle); mirror its looking flag into the RMB tracker so it clears
	// cleanly if the button isn't actually held.
	m_looking = m_world->GetParty().IsLooking();
	m_world->ApplyActiveSnapshot(); // restore the active level's fog + entity diff
	m_ui.ClearLog(); // a loaded game starts its log afresh
	m_ui.AddLogLine(loc::View("log.descend"));
	const Party& party = m_world->GetParty();
	m_ui.ResetHudStatus();
	m_ui.SetHudStatus(party);
	m_state = AppState::Playing;
	if (onWorld) ResumeOnWorldMap(parked);
	log::Info("Loaded game from {}", path);
	return true;
}

// A stair the world raised this frame, from play or from under the sheet.
void Game::FollowLevelTransition(const DungeonWorld::LevelTransition& t) {
	if (t.toWorld) {
		// The panel first: the step onto the stair has landed, and the
		// question freezes the frame before the usual refresh.
		m_ui.SetHudStatus(m_world->GetParty());
		OfferExit(t.level); // an exit stair: ASKED, then left
	} else {
		BeginLevelTransition(t.level, t.x, t.z, t.facing);
	}
}

void Game::OpenCharacterSheet(size_t index) {
	m_audio.Play(m_sounds.click, 0.5f);
	m_ui.ShowSheet(index);
	// The sheet opens from a dungeon or from the world - remember which. But the
	// sheet's own < and > come through here too, and recording THEM would make
	// "resume" mean the sheet itself: close box and Esc then did nothing.
	if (m_state != AppState::CharacterSheet) m_resumeState = m_state;
	m_state = AppState::CharacterSheet;
}

// The landing page's Editor entry (GameUI::onEditorOnArrival). The new game it
// started gets there by any of several routes - a world switch, the first
// dungeon load, a level transition, or straight onto the world map - and all of
// them end by setting Playing or WorldMap. So rather than threading a flag
// through each, this waits for the one thing they share: the game is up, and
// nothing is still on its way (no world switch pending).
void Game::OpenEditorOnArrival() {
	if (!m_editorOnArrival || m_pendingWorld || !m_world) return;
	if (m_state == AppState::Playing) {
		m_editorOnArrival = false;
		m_mapView.Open(MapView::Mode::Editor);
		// PAUSED: monsters act off cooldowns, so a level opened for building
		// must not be fighting the party while it is looked at.
		m_mapView.SetEditorPaused(true);
		log::Info("editor on arrival: {} (paused)", m_world->CurrentLevel());
	} else if (m_state == AppState::WorldMap) {
		// A world that begins in the open: its own editor. The world map
		// simulates nothing, so there is nothing to pause.
		m_editorOnArrival = false;
		if (m_worldMap) m_worldMapView.SetMode(WorldMapView::Mode::Editor);
		log::Info("editor on arrival: the world map");
	}
}

void Game::ReturnToTitle(const char* why) {
	// A wipe lands inside the world update, often in a frame the guard armed,
	// and the line below formats a string. Reporting excuses itself.
	alloc::Excused excuse;
	log::Info("back to the title: {} (was {}, level {})", why, StateName(),
			  m_world ? m_world->CurrentLevel() : std::string("-"));
	m_mapView.Close(); // the editor too: the title draws no overlay, so an open
					   // one would only reappear over the next game
	m_editorOnArrival = false;
	m_state = AppState::Menu;
	m_ui.ResetToMainPage();
}

// The party moves as fast as its slowest member. The rule itself moved to
// DungeonWorld::ApplyPartyPace, because conditioning feeds the pace and levels
// deep inside the combat tick — where Game is not in the call chain. This
// forwards for the load / new-game / startup paths that always drove it.
void Game::ApplyPartySpeed() { m_world->ApplyPartyPace(); }

void Game::ApplyLanguage(bool rebuild) {
	if (!m_pendingLanguage.empty()) {
		m_settings.language = m_pendingLanguage;
		m_pendingLanguage.clear();
		m_settings.Save();
	}
	if (!loc::LoadFile(paths::Asset("lang\\" + m_settings.language + ".lang"))) {
		if (m_settings.language != "en")
			loc::LoadFile(paths::Asset("lang\\en.lang"));
	} else if (m_settings.language != "en") {
		// Surface translation drift: any en.lang key this language lacks
		// renders as the raw key in the UI, so name them in the log.
		loc::LogMissingKeys(paths::Asset("lang\\en.lang"));
	}
	ApplyLanguageFonts();
	if (rebuild) m_ui.RebuildForLanguage();
}

void Game::ApplyLanguageFonts() {
	for (int i = 0; i < ui::kFontRoleCount; ++i) {
		const auto role = static_cast<ui::FontRole>(i);
		std::optional<ui::FaceSpec>& base = m_langFontBase[static_cast<size_t>(i)];
		// The last language's face off first: the role is fonts.cat's again.
		if (base) {
			m_fonts.SetFace(role, *base);
			base.reset();
		}
		const std::string key = std::format("lang.font.{}", ui::FontRoleName(role));
		const std::string_view file = loc::View(key); // the key itself when absent
		if (file.empty() || file == key) continue;
		ui::FaceSpec spec;
		spec.path = paths::Asset(std::string(file));
		if (!std::filesystem::exists(spec.path)) {
			log::Warn("language {}: {} names {}, which is not installed - keeping {}",
					  m_settings.language, key, file,
					  m_fonts.Face(role).path.empty() ? "the fallback" : m_fonts.Face(role).path);
			continue;
		}
		const std::string scaleKey = key + ".scale";
		const std::string_view scale = loc::View(scaleKey);
		spec.scale = scale == scaleKey ? 1.0f : std::strtof(std::string(scale).c_str(), nullptr);
		if (spec.scale <= 0.0f) spec.scale = 1.0f;
		base = m_fonts.Face(role);
		log::Info("language {}: the {} role draws in {} (scale {:.2f})", m_settings.language,
				  ui::FontRoleName(role), file, spec.scale);
		m_fonts.SetFace(role, std::move(spec));
	}
}

const ui::FaceSpec& Game::BaseFace(ui::FontRole role) const {
	const std::optional<ui::FaceSpec>& base = m_langFontBase[static_cast<size_t>(role)];
	return base ? *base : m_fonts.Face(role);
}

void Game::DrawBusyNotice(const std::string& text, float dw, float dh) {
	const ui::Font& font = m_mapView.Font();
	const float w = font.MeasureWidth(text);
	const gfx::Rect back{(dw - w) * 0.5f - 14.0f, (dh - font.Height()) * 0.5f - 10.0f,
						 w + 28.0f, font.Height() + 20.0f};
	m_spriteBatch.DrawRect(back, {0.0f, 0.0f, 0.0f, 0.75f});
	font.Draw(m_spriteBatch, text, (dw - w) * 0.5f, (dh - font.Height()) * 0.5f,
			  m_settings.theme.accent);
}

void Game::SetQuality(Quality quality) {
	if (quality == m_settings.quality) return;
	const std::string oldTextureSuffix = m_settings.TextureSuffix();
	m_settings.quality = quality;
	const bool textureResChanged = oldTextureSuffix != m_settings.TextureSuffix();
	// The light budget follows the tier; re-point the Video tab's dropdown at it.
	m_settings.maxPointLights = GameSettings::QualityLightBudget(quality);
	m_ui.SyncMaxLights();
	m_settings.Save();
	// With no world loaded there is nothing to swap: the next one loads at
	// the tier just chosen.
	if (m_world) m_world->ApplyQuality(textureResChanged);
}

void Game::ApplyDisplaySettings() {
	// Resolve the active adapter's outputs so we can position a borderless window
	// or target a monitor for exclusive full-screen.
	const std::vector<gfx::AdapterInfo> adapters = gfx::EnumerateAdapters();
	const gfx::AdapterInfo* active = nullptr;
	for (const gfx::AdapterInfo& a : adapters)
		if (a.luid == m_device.AdapterLuid()) {
			active = &a;
			break;
		}
	const int out = m_settings.displayOutput;
	const gfx::OutputInfo* output =
		(active && out >= 0 && out < static_cast<int>(active->outputs.size()))
			? &active->outputs[static_cast<size_t>(out)]
			: nullptr;

	switch (m_settings.fullscreen) {
	case gfx::FullscreenMode::Windowed: {
		const u32 w = m_settings.displayWidth > 0 ? static_cast<u32>(m_settings.displayWidth)
												  : m_window.Width();
		const u32 h = m_settings.displayHeight > 0
						  ? static_cast<u32>(m_settings.displayHeight)
						  : m_window.Height();
		m_device.SetFullscreen(false, 0, 0, 0); // drop any exclusive state first
		m_window.SetWindowed(w, h);
		break;
	}
	case gfx::FullscreenMode::Borderless: {
		m_device.SetFullscreen(false, 0, 0, 0);
		if (output)
			m_window.SetBorderless(output->x, output->y,
								   static_cast<u32>(output->width),
								   static_cast<u32>(output->height));
		break;
	}
	case gfx::FullscreenMode::Exclusive: {
		u32 w = static_cast<u32>(m_settings.displayWidth);
		u32 h = static_cast<u32>(m_settings.displayHeight);
		if ((w == 0 || h == 0) && output) { // default to the monitor's native size
			w = static_cast<u32>(output->width);
			h = static_cast<u32>(output->height);
		}
		m_device.SetFullscreen(true, static_cast<u32>(out > 0 ? out : 0), w, h);
		break;
	}
	}
}

void Game::RestartApp() {
	// Leave any exclusive full-screen so the new process can claim the display.
	m_device.SetFullscreen(false, 0, 0, 0);
	const std::string cmd = "\"" + paths::ExecutableDir() + "\\Dungeon.exe\"";
	if (!m_restart.Start(cmd)) log::Warn("Could not relaunch the game ({})", cmd);
	m_quitRequested = true;
}

// ============================================================================
// The state machine
// ============================================================================

// Adaptive thread governor: when the frame runs over its time budget, ease every
// background worker's cadence down (freeing CPU); otherwise drift it back toward
// full speed. Off unless enabled via `governor auto`. Hysteresis comes from
// ASYMMETRIC easing — shed load fast, recover slowly — rather than a deadband
// that holds the current scale (which could pin it throttled forever once a
// transient spike dropped it). The scale change is pushed with wakeNow=false so
// the per-frame updates don't wake every worker (which would itself cause a tick
// burst). NOTE: keys off whole-frame time, which can be GPU-bound — a coarse
// heuristic, hence opt-in.
void Game::UpdateGovernor(float dt) {
	if (!m_governorAuto) return;
	const float fps = m_console.Fps();
	const float frameMs = fps > 1.0f ? 1000.0f / fps : 1000.0f;
	const float ratio = frameMs / m_governorTargetMs;
	const float desired = ratio > 1.15f ? 0.25f : 1.0f; // over budget vs recover
	// Throttle down fast (~0.17s), recover slowly (~2s): sustained load stays
	// throttled, but it always trends back to full when the pressure clears.
	const float rate = desired < m_governorScale ? dt * 6.0f : dt * 0.5f;
	m_governorScale += (desired - m_governorScale) * std::min(1.0f, rate);
	m_governorScale = std::clamp(m_governorScale, 0.25f, 1.0f);
	if (std::abs(m_governorScale - m_threads.GlobalThrottle()) > 0.005f)
		m_threads.SetGlobalThrottle(m_governorScale, /*wakeNow=*/false);
}

// Is the frame now starting one the "steady-state frames allocate nothing" rule
// actually covers? Playing, with nothing that legitimately builds or rebuilds in
// flight — and it must have been that way for a WARM-UP, because the first
// frames after a load or after an overlay closes are still settling (first-time
// icon bakes, a shadow cube filling in, a widget tree laying out).
//
// The character sheet over a level counts too. It stopped being a pause
// (2026-09-28): the world simulates under it, so its frames are gameplay
// frames, and leaving them out meant the guard saw only the one frame that
// opened it. Moving between Playing and the sheet keeps the warm-up running,
// so opening and closing it are checked as well.
//
// A RUNNING EVAL SCRIPT IS A CONSOLE SESSION, and is left out on the same terms
// as an open console. The runner executes one console line per frame, and the
// line is typed input in every sense but the keyboard: it parses into strings,
// prints, mirrors to the log, and `sweep`/`include` splice files into the queue.
// A typed command only ever runs with the console open, which this never arms,
// so a scripted one was being judged by a rule its typed twin is exempt from -
// and only once a batch had run 120 lines, which is why ladder/blast/sweep/
// resources reported it batched and never alone. Excusing the runner instead
// would have to excuse the COMMAND too (DevConsole::Execute allocates before it
// dispatches), leaving a frame that is armed but can report nothing; and the
// `step` inside it runs thousands of simulated ticks in one frame, which is no
// steady-state frame either. Simulation event paths are checked where the frame
// really is steady: AllocTest.ps1 -Wounded / -Melee / -Cast.
bool Game::SteadyStateFrame() {
	constexpr u32 kWarmupFrames = 120;
	// An open portrait picker streams thumbnails in as it scrolls: loading, not a
	// steady state (its opening frame is excused by Game::OpenPortraitPicker). The
	// `mapicons survey` overlay is a dev readout drawn over everything, labels
	// built per frame - a console session's terms, not gameplay.
	const bool quiet = GuardedState() && !m_console.IsOpen() && !EvalRunning() &&
					   !m_ui.PortraitPickerOpen() && !m_mapIconSurvey &&
					   !m_mapView.IsOpen() && !m_baking && m_pendingLanguage.empty() &&
					   !m_pendingQuality;
	m_steadyFrames = quiet ? m_steadyFrames + 1 : 0;
	return m_steadyFrames > kWarmupFrames;
}

// The app states whose frames the rule covers at all: a level being played, or
// the character sheet over one (see above). Everything else - the menus, the
// pause menu, the loads, the world map - builds and rebuilds as a matter of
// course, and is left out.
bool Game::GuardedState() const {
	return m_state == AppState::Playing ||
		   (m_state == AppState::CharacterSheet && m_resumeState == AppState::Playing);
}

// An overlay just opened, PART WAY THROUGH the frame the guard already armed.
//
// SteadyStateFrame runs at the top of Update and judges the frame on the state
// at that instant; the console and map toggles are handled further down the same
// Update, and whatever they open then DRAWS in the same frame's Render. So the
// guard was arming a frame, watching an overlay appear inside it, and reporting
// the overlay's allocations as a steady-state violation — a false alarm every
// time the console was opened, blamed on ThreadManager::SnapshotAll.
//
// Disarm instead, and reset the warm-up: a frame an overlay opened in is not a
// steady-state frame, and the frames right after it are settling. This matters
// more than the handful of bytes it was reporting — a checker that cries wolf
// teaches you to ignore it, and this one had already talked me into calling a
// real report "safe to ignore".
void Game::OverlayOpenedThisFrame() {
	alloc::ArmFrame(false);
	m_steadyFrames = 0;
}

const char* Game::StateName() const { return StateWord(m_state); }

const char* Game::StateWord(AppState state) {
	switch (state) {
	case AppState::Loading: return "loading";
	case AppState::Menu: return "menu";
	case AppState::LoadingGame: return "loadinggame";
	case AppState::LoadingLevel: return "loadinglevel";
	case AppState::Playing: return "playing";
	case AppState::WorldMap: return "worldmap";
	case AppState::Paused: return "paused";
	case AppState::CharacterSheet: return "sheet";
	}
	return "?";
}

// One `alloctest` window: spend the budget only on frames that actually armed,
// so the load, the warm-up and the console being open cost the test nothing. The
// verdict is the guard's own stats, differenced across the window.
void Game::UpdateAllocTest(float dt, bool steady) {
	m_allocTestDeadline -= dt;
	if (steady) {
		// THE HARNESS'S WINDOW OPENS WITH THE FIRST ARMED FRAME. The tally
		// restarts here, so what it reports at the verdict happened INSIDE the
		// measurement rather than in the console's frames or the warm-up, and a
		// held autocast is released, so a barrage's very first cast is measured
		// (tools\AllocTest.ps1 -Impact: a fresh monster's first burn is a cost
		// every monster pays once, and the warm-up would have swallowed it), a
		// held freeze, so a fight's first notice and first blow are (-Melee), and
		// a held autoattack, so the party's first swing is (-Swing, -OnHitTypo).
		if (m_allocTestFrames == 0 && m_world) {
			DungeonWorld::Harness& h = m_world->GetHarness();
			h.tally = {};
			h.autoCast.held = false;
			h.autoAttackHeld = false; // `autoattack hold` left it on, held
			if (h.frozenHeld) h.frozen = h.frozenHeld = false;
			MoveAction last{};
			m_allocTestActsAt = m_world->GetParty().ActCount(last);
		}
		m_allocTestRemaining -= dt;
		++m_allocTestFrames;
	}
	if (m_allocTestRemaining > 0.0f && m_allocTestDeadline > 0.0f) return;

	const alloc::GuardStats now = alloc::Stats();
	const u64 violations = now.violations - m_allocTestStart.violations;
	const u64 badFrames = now.framesViolating - m_allocTestStart.framesViolating;
	const bool timedOut = m_allocTestRemaining > 0.0f;
	// Reporting from inside a guarded frame excuses ITSELF (docs/ARCHITECTURE.md
	// "Checking the rule"). The verdict above is already taken, so this frame
	// could not change it, but unexcused it logged the report's own formatting
	// as a violation after every run.
	const alloc::Excused excuse;
	// One machine-readable line: tools\AllocTest.ps1 greps for it and nothing
	// else, so the format is part of the contract.
	// moves= is the party's Acts inside the window (Party::ActCount): -Walk's
	// proof that its key presses moved the party, and with it the movement pad.
	MoveAction lastMove{};
	const unsigned moves =
		m_world ? m_world->GetParty().ActCount(lastMove) - m_allocTestActsAt : 0u;
	const std::string line =
		std::format("alloctest RESULT={} frames={} violations={} violating_frames={} "
					"transitions={} moves={}{}",
					timedOut ? "SKIP" : (violations == 0 ? "PASS" : "FAIL"),
					m_allocTestFrames, violations, badFrames, m_allocTestTransitions, moves,
					timedOut ? " reason=never_reached_a_steady_frame" : "");
	log::Info("{}", line);
	m_console.Print(line);
	// What the harness counted over exactly those frames (reset on the first).
	if (m_world) log::Info("alloctest window {}", TallyLine());
	if (violations > 0)
		m_console.Print("call sites are in dungeon.log (each reported once per session)");
	m_allocTestRemaining = 0.0f;
}

// Out of line and noinline (see the declaration) on purpose: a frame of its own
// in every build - debug, and a release with -DDN_TRACK_ALLOCS=ON - so the
// guard's report of this violation names Game::AllocPokeOnce.
void Game::AllocPokeOnce() { m_pokeScratch = std::make_unique<u32>(m_framesRendered + 1u); }

void Game::Update(float dt) {
	const bool steady = SteadyStateFrame();
	alloc::ArmFrame(steady);
	if (m_allocTestRemaining > 0.0f) UpdateAllocTest(dt, steady);
	// `allocpoke`: a deliberate violation, so the guard can be seen to catch one.
	if (m_allocPokeRemaining > 0.0f) {
		m_allocPokeRemaining -= dt;
		m_pokeScratch = std::make_unique<u32>(m_framesRendered);
	}
	// `allocpoke once`: set while the console is open (a disarmed frame), so the
	// first `steady` frame after it is the first armed frame after a disarm.
	if (m_allocPokeOnce && steady) {
		m_allocPokeOnce = false;
		AllocPokeOnce();
	}
	// `inputpoke`: before anything reads the frame's typing. It drops WHOLE
	// LINES - a window that closes mid-line takes the rest of that line too.
	// A fragment left behind ran as a command of its own: the console's
	// type-ahead takes its selection on Enter, so a surviving prefix became a
	// whole command name (`ha` -> `haze`, seen) and the next line its argument
	// - TypingTest -SelfTest ran `save inputpoke` into the shared DungeonSaves.
	if (m_inputPokeRemaining > 0.0f || m_inputPokeMidLine) {
		auto& input = m_window.GetInput();
		const bool open = m_inputPokeRemaining > 0.0f;
		if (open) m_inputPokeRemaining -= dt;
		if (!input.TypedChars().empty())
			m_inputPokeMidLine = !input.DiscardTypedForTest(/*throughEnter=*/!open);
	}

	UpdateStates(dt);

	// The pointer's shape, from what this frame's input left it over: the
	// editor's dock edges want the resize arrow (MapView_Docks.cpp), a
	// Ctrl-hovered floating HUD panel its move or resize arrow (GameUI::
	// PanelCursor, taken whether or not it is used so it never goes stale). Set
	// once, here, so no path through UpdateStates has to remember to put it back.
	const Window::Cursor hudCursor = m_ui.TakeHudCursor();
	m_window.SetCursorShape(m_mapView.IsOpen()
								? (!ShowingWorldPage() && m_mapView.WantsResizeCursor()
									   ? Window::Cursor::SizeWE
									   : Window::Cursor::Arrow)
								: hudCursor);

	// A frame that LEFT the guarded states is a transition, not a steady-state
	// frame. SteadyStateFrame judged it on the state at the top, but pressing Esc
	// rebuilds the pause menu (a fresh widget tree, and the save scan behind its
	// Load entry), a stair step stages a level load, a party wipe returns to the
	// title - and whatever the new state is then DRAWS in this frame's Render. The
	// guard was reporting all of that as a violation of Playing: opening the pause
	// menu logged ~5000 allocations against ListSaves (2026-09-28). The same
	// reasoning as OverlayOpenedThisFrame, one level up, and checked HERE, after
	// every early return in UpdateStates, so no transition site has to remember
	// to call it. The destination's own frames are not armed anyway (they fail
	// GuardedState), so only this one frame was ever at issue. Moving INTO the
	// sheet over a level stays guarded: that is a guarded state too.
	if (steady && !GuardedState()) {
		OverlayOpenedThisFrame();
		if (m_allocTestRemaining > 0.0f) ++m_allocTestTransitions;
	}
}

void Game::RefreshPlaceStone() {
	m_placeStoneLevel = m_world ? m_world->CurrentLevel() : std::string();
	m_placeStoneRev = m_world ? m_world->EditRevision() : 0;
	// The level's own override, else its dungeon's, else none (GameUI then
	// falls back to the default material).
	std::string stone = m_world ? m_world->Map().UiStone() : std::string();
	if (stone.empty() && !m_placeStoneLevel.empty())
		if (const CatalogEntry* d = m_project.DungeonOfLevel(m_placeStoneLevel))
			stone = d->Get("ui_stone", "");
	m_ui.SetPlaceStone(stone);
}

float Game::WorldDt(float dt, float timeScale) const {
	// World dt: the dev console's `timescale`, times the REST multiplier
	// (docs/health-and-healing.md). Rest is folded in HERE, at the one place the
	// world's clock is set, rather than into any particular rate — which is what
	// makes "rest is a time multiplier, not a regen multiplier" true of the code
	// and not just of the doc. Health, supplies, effect timers, monster
	// cooldowns and the AI's own cadence all accelerate together because they
	// all read this number.
	//
	// The world takes a result this long - about a second a frame at rest - as
	// fixed ticks, not one step (DungeonWorld::AdvanceSimulation, code-review
	// C64): everything that reads it counts a timer down and acts at most once a
	// step, so one whole second was one think and one square for a monster that
	// awake gets four and two. At most kMaxTicksPerUpdate a frame, so a slow
	// frame rests a little slower.
	// (No world on the title screen: time runs at the plain rate there.)
	return dt * timeScale * (m_world ? m_world->RestTimeScale() : 1.0f);
}

void Game::UpdateStates(float dt) {
	const float wdt = WorldDt(dt, m_timeScale);
	m_time += wdt;

	ApplyPendingWorld(); // a world switch asked for last frame (see Game.h)
	OpenEditorOnArrival(); // the landing page's Editor entry, once the game is up

	// A language picked last frame applies now, before any widget updates —
	// the rebuild destroys every widget, so none may be mid-callback.
	if (!m_pendingLanguage.empty()) ApplyLanguage(true);
	// A quality tier picked last frame applies now — the frame that showed the
	// "applying" notice has presented, so the multi-second texture reload stalls
	// with that notice on screen instead of on a frozen settings page.
	if (m_pendingQuality) {
		const Quality q = *m_pendingQuality;
		m_pendingQuality.reset();
		SetQuality(q);
	}
	// A Video-tab adapter/monitor change last frame repopulates the settings page
	// now, for the same reason: the rebuild destroys the dropdown that triggered it.
	m_ui.ApplyPendingVideoRebuild();

	// The UI material follows the PLACE (more-ui-updates): re-resolved when the
	// party changes level or the editor changes anything, never every frame -
	// the lookup reads the catalogs. A settled frame only compares.
	{
		const std::string_view level =
			m_world ? std::string_view(m_world->CurrentLevel()) : std::string_view();
		const u64 rev = m_world ? m_world->EditRevision() : 0;
		if (level != m_placeStoneLevel || rev != m_placeStoneRev) RefreshPlaceStone();
	}

	{
		DN_PROFILE_ZONE_L(prof::kLevelSystem, "fonts");
		m_ui.UpdateFonts(dt);
	}
	// The resource bars run on REAL time (dt, not wdt): a heartbeat racing 60x
	// while the party rests, or freezing under the pause menu, would be wrong.
	m_ui.TickResourceBars(dt, m_world && m_world->PartyNoticed());
	if (m_previewMesh) m_previewOrbit += dt * 0.6f; // spin the editor 3D preview

	// Poll the asset bake (P4c): non-blocking, so the "baking…" dialog stays
	// responsive. A texture import runs a second step (worn meshes) before it's
	// done; on success FinishBake writes the catalog entry.
	if (m_baking && !m_bake.Running()) {
		if (m_bake.ExitCode() != 0) {
			// Surface the failure where the user is looking: the dialog stays up
			// with the exit code so the form can be fixed and retried (the full
			// baker output is in dungeon.log next to the exe).
			log::Warn("AssetBaker failed (exit {})", m_bake.ExitCode());
			m_baking = false;
			if (m_restyleBake) { m_restyleBake = false; m_typeDialog.Close(); }
			else m_assetDialog.SetError(loc::Format("newasset.err.bake",
													m_bake.ExitCode()));
		} else if (m_bakeReq.textureSet && m_bakeStep == 0) {
			m_bakeStep = 1; // textures imported — now rebake worn block meshes
			if (!StartBakeStep()) {
				m_baking = false;
				m_assetDialog.SetBusy(false);
			}
		} else if (m_restyleBake) {
			// Surface restyle rebake done: swap the new worn geometry in live
			// (if the world that asked for it is still the one loaded).
			if (m_world) m_world->ReloadDungeonBlocks();
			m_restyleBake = false;
			m_baking = false;
			m_typeDialog.Close();
			if (m_world && m_world->onMessage)
				m_world->onMessage(loc::View("map.wallstyle.applied"));
		} else {
			FinishBake();
			m_baking = false;
		}
		// Reset to defaults (relief < 0 = the texture set's own relief).
		if (!m_baking) { m_bakeWear = 1.0f; m_bakeRelief = -1.0f; }
	}

	const Input& input = m_window.GetInput();

	// The dev console toggles with `~` and overlays any state. While it is
	// open it captures input (so the party can't move) but the world keeps
	// simulating — it does NOT pause the game. The FPS sampler ticks every
	// frame regardless. While a staged load is mid-flight the world is only
	// partially built (the HUD log, meshes, and monsters arrive task by task),
	// so command EXECUTION is gated off — a `cast`/`save`/`quality` then would
	// reach into objects a later task creates (this crashed: 0xc0000005 in the
	// HUD log line a cast raises before BuildHud has run). The console itself
	// stays usable for the perf/thread panels.
	const bool loading = m_state == AppState::Loading ||
						 m_state == AppState::LoadingGame ||
						 m_state == AppState::LoadingLevel;
	const bool consoleWasOpen = m_console.IsOpen();
	if (input.WasKeyPressed(VK_OEM_3)) {
		m_console.Toggle();
		OverlayOpenedThisFrame(); // this frame is no longer a steady-state one
	}
	m_console.SetCommandsEnabled(!loading);
	// Immediately after the gate it reads, so a scripted line runs on exactly the
	// same footing as a typed one — including being held back through a load.
	PumpEvalScript(dt);
	{
		// The console is itself instrumented: it samples every graph's history
		// each frame whether open or not, and a readout that costs more than
		// what it reports would be worth knowing about.
		DN_PROFILE_ZONE_L(prof::kLevelSystem, "console");
		m_console.Update(input, dt, static_cast<float>(m_window.Width()),
						 static_cast<float>(m_window.Height()), m_device, m_spriteBatch);
	}
	UpdateGovernor(dt); // adaptive thread throttle (no-op unless `governor auto`)
	// Which of its two lives the world view is leading, recomputed every frame
	// rather than set at the transitions: as the player's OVERLAY (the M map's
	// world page in a dungeon) it offers a way back to the dungeon map and is
	// never the editor (code-review C79); as the travel screen it must not
	// offer one (there is no dungeon to go back to out there). DERIVED HERE -
	// after this frame's console and script commands, before the console's
	// early return below - so it holds on every frame the page can draw,
	// console open or not: from the play path, a `mappage world` typed at the
	// open console drew the page with the flag the travel screen had left.
	m_worldMapView.SetOverlay(BackdropState() == AppState::Playing && ShowingWorldPage());
	// The console owns the whole frame's input if it was open at the start (or
	// just opened) — so the very keystroke that closes it (Esc or `~`) never
	// also reaches the pause menu / HUD this frame. Owning input is NOT a
	// pause: a playing world keeps simulating here, and a loading state falls
	// through to its case below so the task queue keeps pumping (an open
	// console used to stall the load, holding the world half-built) — only
	// its Esc-to-quit is console-gated.
	const bool consoleOwnsInput = m_console.IsOpen() || consoleWasOpen;
	if (consoleOwnsInput && !loading) {
		if (m_state == AppState::Playing) {
			m_world->Update(input, wdt, m_time, /*acceptInput=*/false);
			Party& party = m_world->GetParty();
			m_ui.SetHudStatus(party);
		}
		return;
	}

	// `crashpoke uiclip`'s tree (Game/ClipPoke.h), walked with the real input once
	// the console has let go of it - the harness's click has to reach it.
	if (m_clipPoke)
		m_clipPoke->Update(input, static_cast<float>(m_window.Width()),
						   static_cast<float>(m_window.Height()));

	// An item's details belong to the page they were opened over. Anything that
	// takes the game off it - a pause, a wipe to the title, a level load - drops
	// them, so they are not waiting, invisible and holding the mouse, on return.
	if (m_ui.ItemDetailsOpen() && m_state != AppState::Playing &&
		m_state != AppState::CharacterSheet)
		m_ui.CloseItemDetails();
	// The portrait picker likewise - except over the party creation page, its
	// other home, on the title screen.
	if (m_ui.PortraitPickerOpen() && m_state != AppState::Playing &&
		m_state != AppState::CharacterSheet &&
		!(m_state == AppState::Menu && m_ui.PartyPageOpen()))
		m_ui.ClosePortraitPicker();

	switch (m_state) {
	case AppState::Loading:
		// No Esc handling at all: ESC NEVER QUITS, in any state (Michael,
		// 2026-08-11). See the Menu case below for why. A loading screen shows no
		// Exit button, so during a load the ways out are the console's
		// `quit`/`exit` and the window's own close button — which is independent of
		// all of this (Window.cpp's WM_CLOSE sets m_closed), so a load can never
		// become unquittable.
		if (RunLoadTasks()) m_state = AppState::Menu;
		return;

	case AppState::Menu:
		// The menu sits on baked title art; nothing in the world simulates.
		// Esc backs out of settings — and does NOTHING on the landing list, where
		// it used to QUIT (Michael, 2026-08-11). ESC NEVER QUITS, IN ANY STATE:
		// quitting is deliberate, and means an Exit entry (landing or pause) or
		// `quit`/`exit` in the console.
		//
		// It read as a crash, which is why it went. A party wipe drops you here,
		// and a reflexive Esc at a screen that had just appeared by itself killed
		// the process with no confirmation and no log line — indistinguishable
		// from the game falling over. Nothing about "back out" should be able to
		// end the process, which is why the two LOADING states lost it too rather
		// than keeping it as an abort hatch.
		//
		// (Key-bind capture still swallows Esc first, to cancel the capture.)
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive())
			m_ui.CloseSettingsPage();
		m_ui.UpdateMenu(input, dt);
		return;

	case AppState::LoadingGame:
		// (no Esc quit — see AppState::Loading)
		if (RunLoadTasks()) {
			m_gameLoaded = true;
			if (!m_pendingLoadPath.empty()) {
				const std::string path = std::exchange(m_pendingLoadPath, {});
				if (!LoadGame(path)) { // bad/corrupt file: fall back to the menu
					m_state = AppState::Menu;
					m_ui.ResetToMainPage();
				}
			} else {
				StartNewGame(); // sets AppState::Playing
			}
		}
		return;

	case AppState::LoadingLevel:
		if (RunLoadTasks()) {
			// Restore this level's saved fog/progress (if visited before — the
			// monsters now exist for the entity diff), then place the party.
			m_world->ApplyActiveSnapshot();
			int px = m_pendingLevelX, pz = m_pendingLevelZ;
			if (px < 0) { // sentinel: arrive at the new level's start cell
				px = m_world->Map().StartX();
				pz = m_world->Map().StartZ();
			}
			m_world->PlacePartyAt(px, pz,
							  m_pendingLevelFacing ? *m_pendingLevelFacing
												   : m_world->ArrivalFacingAt(px, pz));
			// Re-layer the saved free-look offset on the placed party (a save load
			// onto a different level; orthogonal for ordinary transitions). The
			// offset parks at the saved angle; mirror the looking flag into the RMB
			// tracker so it clears cleanly if the button isn't actually held.
			m_world->GetParty().SetLookState(m_pendingLookYaw, m_pendingLookPitch,
											m_pendingLooking);
			m_looking = m_pendingLooking;
			m_ui.ClearLog();
			m_state = AppState::Playing;
			// The moment a level is PLAYABLE. "Game loaded" is logged by a load
			// task, so since the world loads on demand it lands before this level's
			// own load has even begun; a script that waits on it types into a
			// console whose commands are still gated off (tools\AllocTest.ps1).
			log::Info("Level ready: {} at {},{}", m_world->CurrentLevel(), px, pz);
			if (m_pendingWorldMap) ResumeOnWorldMap(m_pendingWorldPark);
			m_pendingWorldMap = m_pendingWorldPark = false;
		}
		return;

	case AppState::Paused:
		// The world is frozen — only the pause menu (or the shared settings
		// page) updates. Esc backs out of settings (but an armed key-bind box
		// gets it first, as its cancel), or resumes play.
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive()) {
			m_audio.Play(m_sounds.click, 0.5f);
			if (!m_ui.CloseSettingsPage()) m_state = m_resumeState;
			return;
		}
		m_ui.UpdatePause(input);
		return;

	case AppState::CharacterSheet:
		// Esc resumes - to wherever the sheet was opened FROM (m_resumeState) -
		// unless a popup is up over it (an item's details, a use menu), which Esc
		// closes first.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_audio.Play(m_sounds.click, 0.5f);
			if (!m_ui.DismissPopup()) m_state = m_resumeState;
			return;
		}
		m_ui.UpdateSheet(input, dt);
		// NOT A PAUSE (Michael, 2026-09-28: only the pause menu and the editor's
		// pause button stop the game). Over a level the world goes on - monsters
		// walk and strike, effects tick, a rest keeps resting - while the INPUT
		// stays the sheet's: the party does not walk off under an open page.
		// (The world map simulates nothing, so a sheet opened there has nothing
		// to run.)
		if (m_state == AppState::CharacterSheet && m_resumeState == AppState::Playing) {
			m_world->Update(kNoInput, wdt, m_time);
			// Whatever the world did may end the sheet: a wipe returns to the
			// title (the state is no longer ours), and a transition - a pit fall
			// that was already under way - takes the party elsewhere.
			if (m_state != AppState::CharacterSheet) return;
			if (auto t = m_world->ConsumeLevelTransition()) {
				m_state = AppState::Playing;
				FollowLevelTransition(*t);
				return;
			}
			m_ui.SetHudStatus(m_world->GetParty());
			m_ui.SetResting(m_world->Resting());
		}
		return;

	case AppState::WorldMap: {
		// Travelling. The world simulates nothing — a journey is RESOLVED, not
		// simulated (docs/world-map.md) — so this state only reads input and
		// draws. Esc opens the pause menu, which resumes back HERE.
		//
		// The world settings dialog is MODAL over this screen, like every
		// editor dialog over the dungeon map: while it is up it owns the input,
		// so a key meant for one of its fields cannot also walk the party.
		if (m_worldSettingsDialog.IsOpen()) {
			m_worldSettingsDialog.Update(input, static_cast<float>(m_window.Width()),
										 static_cast<float>(m_window.Height()));
			return;
		}
		// The new-world dialog sits ABOVE the Worlds dialog that can open it.
		if (m_newWorldDialog.IsOpen()) {
			m_newWorldDialog.Update(input, static_cast<float>(m_window.Width()),
									static_cast<float>(m_window.Height()));
			return;
		}
		if (m_worldsDialog.IsOpen()) {
			m_worldsDialog.Update(input, static_cast<float>(m_window.Width()),
								  static_cast<float>(m_window.Height()));
			return;
		}
		// A doorway's question owns the input while it is up — its Esc is a
		// No, not the pause menu, and its Enter is a Yes, not a second "go in".
		if (m_ui.PromptActive()) {
			if (!m_console.IsOpen()) m_ui.UpdatePrompt(input);
			return;
		}
		if (input.WasKeyPressed(VK_ESCAPE) && !m_console.IsOpen()) {
			// In the world editor Esc puts an armed terrain brush down first,
			// as the level editor's ladder does, and only then pauses (C81).
			if (m_worldMapView.Disarm()) return;
			m_audio.Play(m_sounds.click, 0.5f);
			m_ui.ResetToMainPage();
			m_ui.RebuildPauseMenu();
			m_resumeState = m_state;
			m_state = AppState::Paused;
			return;
		}
		if (m_worldMap) {
			m_worldMapView.Update(input, *m_worldMap,
								  WorldPanel(static_cast<float>(m_window.Width()),
											 static_cast<float>(m_window.Height())));
			// The stroke closes when the button comes up, wherever the cursor
			// ended — including outside the grid, which is exactly where a drag
			// that ran off the edge would otherwise leave it open forever.
			if (m_worldStroke && !input.IsMouseDown(MouseButton::Left)) {
				m_worldStroke = false;
				m_world->CommitUndoStep(/*changed=*/true);
			}
			// The BOUND movement keys, read as compass directions: there is no
			// facing out here, so forward/back/strafe are north/south/west/east
			// and the turn keys mean nothing. Using the bindings rather than
			// hardcoded arrows keeps one set of movement keys in the game.
			const MoveKeys& k = m_settings.moveKeys;
			int dx = 0, dz = 0;
			if (input.WasKeyPressed(k.forward)) dz = -1;
			else if (input.WasKeyPressed(k.back)) dz = 1;
			else if (input.WasKeyPressed(k.strafeLeft)) dx = -1;
			else if (input.WasKeyPressed(k.strafeRight)) dx = 1;
			if ((dx || dz) && !m_console.IsOpen()) {
				if (!TravelStep(dx, dz)) m_ui.AddLogLine(loc::View("world.blocked"));
				// WALKING ONTO A DOORWAY ASKS (Michael, 2026-09-24). Asked only
				// if the step left the party on the world map — an encounter
				// rolled on that square has already taken it somewhere else.
				else if (m_state == AppState::WorldMap) OfferEntrance();
			}
			// C makes camp. Like Enter below it has no binding of its own —
			// the world map's verbs are few enough to name, and inventing a
			// bindable action per verb before there are several would be
			// guessing at a control scheme.
			if (input.WasKeyPressed('C') && !m_console.IsOpen()) Camp();
			// Enter goes IN, at whatever the party is standing on.
			if (input.WasKeyPressed(VK_RETURN) && !m_console.IsOpen()) {
				if (const WorldMap::Location* l =
						m_worldMap->LocationAt(m_worldState.x, m_worldState.z))
					EnterLocation(l->id);
				else
					m_ui.AddLogLine(loc::View("world.nothing_here"));
			}
		}
		return;
	}

	case AppState::Playing:
		break;
	}

	// --- Playing -------------------------------------------------------------
	// An exit's question FREEZES THE WORLD while it is up, the way the pause
	// menu does: nothing may walk up and hit a party that is being asked
	// whether it wants to leave. Its Esc is a No, not the pause menu.
	if (m_ui.PromptActive()) {
		if (!m_console.IsOpen()) m_ui.UpdatePrompt(input);
		return;
	}
	// The asset-creation dialog is modal over the editor: while it is up it owns
	// input and the world/overlay are frozen.
	// The asset picker sits ABOVE every dialog that opens it (the type editor and
	// the create dialog), so it comes first: while it is up it owns the mouse and
	// the keyboard (its search box types).
	// The new-world dialog, from the level editor's toolbar: modal like the rest.
	if (m_newWorldDialog.IsOpen()) {
		m_newWorldDialog.Update(input, static_cast<float>(m_window.Width()),
								static_cast<float>(m_window.Height()));
		return;
	}
	if (m_assetPicker.IsOpen()) {
		m_assetPicker.Update(input, static_cast<float>(m_window.Width()),
							 static_cast<float>(m_window.Height()), dt);
		return;
	}
	if (m_assetDialog.IsOpen()) {
		m_assetDialog.Update(input, static_cast<float>(m_window.Width()),
							 static_cast<float>(m_window.Height()), dt);
		return;
	}
	// The per-type catalog editor is likewise modal over the editor - and it can
	// open OVER the Balance dialog (its Effects tab), so it takes input first.
	if (m_typeDialog.IsOpen()) {
		m_typeDialog.Update(input, static_cast<float>(m_window.Width()),
							static_cast<float>(m_window.Height()));
		m_typeOverBalance = m_balanceDialog.IsOpen();
		return;
	}
	if (m_typeOverBalance) { // it just closed over the Balance dialog
		m_typeOverBalance = false;
		if (m_balanceDialog.IsOpen()) {
			m_balanceDialog.SetEffects(EffectRows());
			m_balanceDialog.Rebuild();
		}
	}
	// The combat-tuning dialog is likewise modal over the editor.
	if (m_balanceDialog.IsOpen()) {
		m_balanceDialog.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		return;
	}
	// The per-level settings dialog is likewise modal over the editor.
	if (m_levelSettingsDialog.IsOpen()) {
		m_levelSettingsDialog.Update(input, static_cast<float>(m_window.Width()),
									 static_cast<float>(m_window.Height()));
		return;
	}
	// The generator knobs are likewise modal over the editor.
	if (m_generateDialog.IsOpen() && !m_validateDialog.IsOpen()) {
		m_generateDialog.Update(input, static_cast<float>(m_window.Width()),
								static_cast<float>(m_window.Height()));
		return;
	}
	// The check report is likewise modal over the editor.
	if (m_validateDialog.IsOpen()) {
		m_validateDialog.Update(input, static_cast<float>(m_window.Width()),
								static_cast<float>(m_window.Height()));
		return;
	}
	// The monster-config dialog is likewise modal over the editor.
	if (m_monsterDialog.IsOpen()) {
		m_monsterDialog.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		// Drive the live preview: (re)build the Animator when the selected type/clip
		// changes, then advance it looping so Render can blit the current pose.
		const std::string& type = m_monsterDialog.SelectedType();
		const std::string& clip = m_monsterDialog.PreviewClip();
		if (clip.empty()) {
			m_previewMonMesh = nullptr;
		m_previewMonSubs.clear();
			m_previewClip.clear();
		} else if (type != m_previewType) {
			// New type: (re)build the Animator over its skeleton/clips + cache the
			// mesh/material/scale/yaw. A same-type clip switch is just a Play (below).
			const auto d = m_world->MonsterPreviewFor(type);
			m_previewMonMesh = d.mesh;
			m_previewMonMat = d.material;
			m_previewMonSubs = d.subs; // multi-material rigs preview every piece
			m_previewMonScale = d.scale;
			m_previewMonYaw = d.modelYaw;
			m_previewMonPivot = d.pivot;
			m_previewAnim = DungeonWorld::MonsterAnimator(d.skeleton, d.clips); // as in the world
			m_previewAnim.Play(clip, /*loop*/ true);
			m_previewType = type;
			m_previewClip = clip;
		} else if (clip != m_previewClip) {
			m_previewAnim.Play(clip, /*loop*/ true); // same rig, just switch clips
			m_previewClip = clip;
		}
		if (m_previewMonMesh) m_previewAnim.Update(dt);
		return;
	}
	// The multi-object inspect chooser is modal over the editor (it precedes the
	// inspector it opens).
	if (m_inspectPicker.IsOpen()) {
		m_inspectPicker.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		return;
	}
	// The per-instance edit dialogs (monster / torch / item+decoration / door /
	// button / niche / stair) are likewise modal over the editor. They share a
	// base, so one walk of InstanceInspectors() covers all seven, and the preview
	// simulation is driven off the open dialog's SPEC rather than off which
	// dialog it is — the three flags are disjoint across the six (only a monster
	// spec carries a skeleton, only a fixture's carries fire, only a loose
	// item's spins), so this is the per-type chain it replaces, minus the
	// chance of adding a seventh dialog and forgetting one of its three sites.
	if (InstanceInspector* ii = ActiveInstanceInspector()) {
		ii->Update(input, static_cast<float>(m_window.Width()),
				   static_cast<float>(m_window.Height()));
		const PreviewSpec& sp = ii->Preview();
		if (sp.skeleton) m_previewAnim.Update(dt);            // skinned idle loop
		if (sp.fire && sp.showFire) m_previewFire.Update(dt); // lit torch flame
		if (sp.spin) m_previewSpin += dt * 0.9f;              // turntable
		return;
	}
	// And the in-flight projectile inspector (read-only details + dismiss).
	if (m_projectileInspector.IsOpen()) {
		m_projectileInspector.Update(input, static_cast<float>(m_window.Width()),
									 static_cast<float>(m_window.Height()));
		return;
	}

	// Map overlay: a toggle that never pauses the world. While it is open the
	// party still walks (keyboard) — the overlay only claims the mouse for
	// panning/zooming/editing, and Esc/M closes it instead of pausing.
	// EXCEPTION: while the editor's palette filter box holds focus, typed
	// keys are ITS (an 'm' must not toggle the map, Esc only unfocuses, and
	// the party must not walk on WASD) — capture is checked before the
	// overlay update runs so a releasing Esc doesn't also act here.
	const bool typingFilter = m_mapView.IsOpen() &&
							  m_mapView.CurrentMode() == MapView::Mode::Editor &&
							  m_mapEditor.KeyboardCaptured();
	if (!typingFilter && input.WasKeyPressed('M')) {
		m_mapView.Toggle();
		ShowMapPage(MapPage::Dungeon); // a fresh open shows where you ARE
		OverlayOpenedThisFrame();      // as the console toggle above
		m_ui.CloseItemDetails();       // the map takes the mouse and the screen
		m_ui.ClosePortraitPicker();
	}

	// The editor's pause/play button freezes the world so the level can be
	// edited against a still scene: no sim time step and no party input. Never
	// set outside Editor mode (MapView::EditorPaused gates on it), and the
	// overlay clears it on close/mode-flip, so a closed editor always runs.
	const bool worldFrozen = m_mapView.EditorPaused();

	// Deferred editor-geometry rebake: undo/redo skips the expensive surface
	// rebuild while the full-screen editor hides the scene. The debt comes due
	// the moment the scene can show again (editor closed OR flipped to the
	// player map, which draws over the live scene): latch one frame so the
	// "rebuilding geometry" notice renders, then flush — the blocking rebake
	// freezes on the notice frame.
	const bool editorMapActive = m_mapView.IsOpen() &&
								 m_mapView.CurrentMode() == MapView::Mode::Editor;
	if (editorMapActive) {
		m_geomNoticeLatched = false;
	} else if (m_world->GeometryDirty()) {
		if (m_geomNoticeLatched) {
			m_world->FlushGeometry();
			m_geomNoticeLatched = false;
		} else {
			m_geomNoticeLatched = true;
		}
	}
	// (The world view's overlay flag was derived above, before the console's
	// early return - see SetOverlay there.)
	if (m_mapView.IsOpen()) {
		// The editor's level drop-down is the topmost thing on the map: an Esc
		// while it is open closes it and nothing else - not the route under it,
		// not the brush, not the editor (code-review C81).
		if (!typingFilter && input.WasKeyPressed(VK_ESCAPE) && m_mapView.CloseLevelList())
			return;
		// While laying a patrol route (grid clicks lay waypoints), keys finish/undo
		// it - ahead of the overlay's own Esc-to-close. On the editor map only
		// (RouteKeyPressed says no anywhere else).
		if (!typingFilter) {
			if (input.WasKeyPressed(VK_BACK)) RouteKeyPressed(RouteKey::Back);
			if ((input.WasKeyPressed(VK_RETURN) || input.WasKeyPressed(VK_ESCAPE)) &&
				RouteKeyPressed(RouteKey::Finish))
				return; // back to the inspector
		}
		// Esc in the editor backs out one layer at a time (the rule for Esc
		// everywhere): the level drop-down above, then a drag in progress is let
		// go, then an armed brush is put down (play-test #2: "we'll need a way to
		// 'disarm' the brush"), and only then does the map close.
		if (!typingFilter && input.WasKeyPressed(VK_ESCAPE) &&
			m_mapView.CurrentMode() == MapView::Mode::Editor) {
			if (m_mapEditor.Moving()) {
				m_mapEditor.CancelMove();
				return;
			}
			if (m_mapEditor.Disarm()) return;
		}
		if (!typingFilter && input.WasKeyPressed(VK_ESCAPE)) {
			CloseMapOverlay(); // the close boxes' path too
			return;
		}
		{
			DN_PROFILE_ZONE_L(prof::kLevelSystem, "map");
			// THE OVERLAY IS OPEN; the PAGE says which view fills it (W6).
			// m_mapView stays the open/closed flag either way — it is the
			// overlay — and only one of the two is updated, so the world page
			// cannot also pan the dungeon grid under it.
			const gfx::Rect panel = MapPanel(static_cast<float>(m_window.Width()),
											 static_cast<float>(m_window.Height()));
			if (ShowingWorldPage()) m_worldMapView.Update(input, *m_worldMap, panel);
			else m_mapView.Update(input, panel);
		}
		// After the overlay's Update, so a stroke that ENDED this frame (its undo
		// step committed on the release) is checked this frame.
		RefreshLiveIssues(input.IsMouseDown(MouseButton::Left) ||
						  input.IsMouseDown(MouseButton::Right) ||
						  input.IsMouseDown(MouseButton::Middle));
		// The world keeps simulating while the map is open (the party still
		// walks on the keyboard) — EXCEPT while the editor is PAUSED, where the
		// whole world update is skipped so every persistent bit freezes:
		// monster AI decisions (they act off cooldowns, not dt, so dt=0 alone
		// wouldn't stop a ready monster), party tweens, particles, door slides,
		// animators. Editing routes through MapEditor→DungeonWorld directly, not
		// through Update, so it works while frozen; the full-screen editor
		// renders no 3D scene, so the skipped camera/light refresh is unseen.
		// The filter box eats the keyboard when it holds focus (blank Input).
		if (!worldFrozen) {
			static const Input kNoInput;
			m_world->Update(typingFilter ? kNoInput : input, wdt, m_time);
			if (auto t = m_world->ConsumeLevelTransition()) {
				m_mapView.Close(); // a stair step starts a new level load
				// An EXIT stair leaves the dungeon rather than changing level,
				// surfacing at the location its `dest` names.
				if (t->toWorld) {
					m_ui.SetHudStatus(m_world->GetParty()); // see the play path
					OfferExit(t->level);
				} else BeginLevelTransition(t->level, t->x, t->z, t->facing);
				return;
			}
			Party& party = m_world->GetParty();
			m_ui.SetHudStatus(party);
		}
		return;
	}

	// Esc closes a popup first (an item's details, a use menu), then the
	// inventory window, before it would pause.
	if (input.WasKeyPressed(VK_ESCAPE) && m_ui.DismissPopup()) {
		m_audio.Play(m_sounds.click, 0.5f);
		return;
	}
	if (m_ui.InventoryOpen() && input.WasKeyPressed(VK_ESCAPE)) {
		m_ui.CloseInventory();
		return;
	}

	// Esc freezes the world and opens the pause menu.
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_audio.Play(m_sounds.click, 0.5f);
		m_ui.ResetToMainPage();
		m_ui.RebuildPauseMenu(); // Load entry tracks whether a save now exists
		m_resumeState = m_state;
		m_state = AppState::Paused;
		return;
	}

	// UI first so it can consume the mouse; keyboard always reaches the party.
	{
		DN_PROFILE_ZONE_L(prof::kLevelSystem, "hud");
		m_ui.UpdateHud(input, dt);
	}
	// A portrait click may have opened the character sheet — freeze now
	// rather than simulating one more frame.
	if (m_state != AppState::Playing) return;

	// Held-tablet mouse interaction in the 3D world (only when the HUD didn't
	// already claim the click). Empty-handed: a click on a floor tablet picks it
	// up onto the cursor. Holding: a click drops it on the floor. Placement onto
	// portraits / hands / inventory is handled by those widgets (P4+), which
	// consume the mouse first.
	if (!m_ui.HudMouseConsumed()) {
		const float mx = input.MouseX(), my = input.MouseY();
		const float w = static_cast<float>(m_window.Width());
		const float h = static_cast<float>(m_window.Height());
		// Every one of these is the PARTY LEADER's act (Phase 9): with nobody
		// standing to lead, the hand does nothing in the world.
		if (input.WasMousePressed(MouseButton::Left) && m_world->LeaderMember()) {
			if (m_heldItem) {
				// THROW OR DROP (Phase 10): a click on reachable floor (or an
				// open niche) lays it there; any other click throws it. A throw
				// the leader cannot make yet keeps it in the hand.
				// A torch clicked onto the EMPTY wall bracket the party faces
				// is mounted there instead.
				const float charge = m_heldItem.Charge();
				if (m_world->MountTorchAhead(*m_heldItem, mx, my, w, h, charge) ||
					m_world->DropItemAt(*m_heldItem, mx, my, w, h, charge) ||
					m_world->ThrowItem(*m_heldItem, -1, charge))
					m_heldItem.reset();
			} else if (float charge = kNoCharge;
					   const std::string* picked = m_world->TryPickItem(mx, my, w, h, &charge)) {
				OnItemFound(*picked); // quest / flag / reveal hooks
				m_heldItem.Set(*picked, charge); // into the cursor's own buffer (HeldItem)
			} else if (m_world->TakeTorchAhead(mx, my, w, h, m_heldItem)) {
				// The wall torch the party faces, off its bracket onto the cursor.
			} else if (!m_world->ToggleDoorAhead(mx, my, w, h)) {
				// No tablet, and nothing on the door ahead that the click
				// actually landed on: try the button on the wall the party
				// faces (a lever in the party's own cell).
				m_world->PressButtonFacing();
			}
		}
		// Right-mouse free-look: hold RMB and drag to swing the view. Begin on a
		// press over the 3D view (not a HUD widget); the party folds the offset
		// into a grid turn once it passes 45° (Party::AddLook). The handler below
		// runs every frame the button is down so the drag keeps tracking even if
		// the cursor wanders over the HUD.
		if (input.WasMousePressed(MouseButton::Right)) {
			m_looking = true;
			m_lookPrevX = m_lookPressX = mx;
			m_lookPrevY = m_lookPressY = my;
			m_lookStray = 0.0f;
			m_world->GetParty().BeginLook();
		}
	}
	// Free-look drag/release is tracked outside the HUD-consumed gate so a drag
	// that strays over the bar (or a release there) still resolves.
	if (m_looking && input.IsMouseDown(MouseButton::Right)) {
		// Base ~0.005 rad/pixel (a quarter turn in ~157px), scaled by the user's
		// Look Sensitivity setting (Settings → Controls).
		const float k = 0.005f * m_settings.look.sensitivity;
		const float dx = input.MouseX() - m_lookPrevX;
		const float dy = input.MouseY() - m_lookPrevY;
		m_lookPrevX = input.MouseX();
		m_lookPrevY = input.MouseY();
		m_lookStray = std::max(m_lookStray, std::hypot(input.MouseX() - m_lookPressX,
													   input.MouseY() - m_lookPressY));
		// Drag right -> view swings right (clockwise); drag down -> look down.
		m_world->GetParty().AddLook(-dx * k, -dy * k);
	} else if (m_looking) {
		m_looking = false;
		m_world->GetParty().EndLook(); // RMB up: the offset eases back to orthogonal
		// A press and release that never strayed past a few pixels was a CLICK:
		// the details of the floor item it landed on (Michael, 2026-09-30 - a
		// drag stays mouse look). The look it began moved the view by at most
		// those few pixels, and the pick uses the PRESS point, where the item was.
		constexpr float kClickSlop = 3.0f;
		if (m_lookStray <= kClickSlop) {
			const float w = static_cast<float>(m_window.Width());
			const float h = static_cast<float>(m_window.Height());
			if (const std::string* type =
					m_world->ItemTypeUnder(m_lookPressX, m_lookPressY, w, h))
				m_ui.ShowItemDetails(*type, m_itemWeights.For(*type));
		}
	}
	m_world->Update(input, wdt, m_time);
	if (auto t = m_world->ConsumeLevelTransition()) {
		FollowLevelTransition(*t);
		return;
	}

	Party& party = m_world->GetParty();
	m_ui.SetHudStatus(party);
	// The Rest button's face, from the world rather than from its own callback:
	// rest ends by itself as often as by a click, so the label has to follow the
	// state and not the input that usually causes it.
	m_ui.SetResting(m_world->Resting());
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound. Loading, Menu, and LoadingGame are 2D-only
// (title art / progress screens); Playing draws the 3D scene + HUD, WorldMap
// the overworld, and Paused / CharacterSheet draw over whichever of those two
// they were opened from (BackdropState).
// ============================================================================
void Game::Render(ID3D12GraphicsCommandList* list) {
	m_renderer.NewFrame(m_device.FrameIndex());
	m_spriteBatch.NewFrame(m_device.FrameIndex());
	if (m_world) m_world->NewFrame(m_device.FrameIndex()); // none on the title screen

	// The full-screen editor map covers everything, so skip the 3D scene (and
	// the HUD below) while it is up — nothing else needs drawing behind it.
	const bool editorMap = m_state == AppState::Playing && m_mapView.IsOpen() &&
						   m_mapView.CurrentMode() == MapView::Mode::Editor;
	// Whose picture is behind this frame's 2D: the state's own, or - under the
	// pause menu and the sheet - the one they were opened from (C365).
	const AppState backdrop = BackdropState();
	m_drewScene = false;

	// The offscreen 3D preview feeds from the asset dialog's picked model (P4b)
	// or the dev `preview` command (P4a). Render() redirects the OM, so rebind
	// the back buffer for the 2D pass.
	const gfx::Mesh* pvMesh = nullptr;
	gfx::MaterialParams pvMat;
	float pvOrbit = 0.0f;
	float pvScale = 1.0f;
	float pvAspect = 1.0f;           // pane width/height, so a tall pane doesn't distort
	std::span<const Mat4> pvPalette; // skinning palette for the animated monster preview
	gfx::ParticleBatch* pvParticles = nullptr; // torch preview flame/smoke
	std::span<const gfx::ParticleInstance> pvBillboards;
	std::span<const gfx::PreviewSubmesh> pvSubs; // per-instance dialog (multi-material)
	const Vec3* pvFitMin = nullptr;             // auto-fit AABB (small items)
	const Vec3* pvFitMax = nullptr;
	Vec3 pvPivot{}; // the point a grounded model stands and turns on (a rig's root)
	float pvViewTilt = 0.0f; // a fitted model looked down on (a floor feature)
	const Mat4* pvOrient = nullptr; // a fitted model's pose: set = a Y-only turntable
	static const Mat4 kUpright = Mat4Identity();
	if (m_assetPicker.IsOpen() && m_assetPicker.HasPreview()) {
		// The picker is above the type editor and above the create dialog, so it
		// claims the shared preview RT first. The model (or, for a texture set,
		// the wall block wearing it) is fitted to the pane and turns upright on a
		// turntable, or tumbles if it lies flat.
		pvSubs = m_assetPicker.PreviewParts();
		pvPalette = m_assetPicker.PreviewPalette(); // a rig in its idle
		pvOrbit = m_assetPicker.Orbit();
		pvFitMin = m_assetPicker.PreviewFitMin();
		pvFitMax = m_assetPicker.PreviewFitMax();
		if (m_assetPicker.PreviewStands()) pvOrient = &kUpright;
		pvViewTilt = m_assetPicker.PreviewViewTilt();
		// Seen from above (or below), a square cell turns through its DIAGONAL:
		// fit that (1/sqrt 2), or its corners leave the pane every quarter turn.
		if (pvViewTilt != 0.0f) pvScale = 0.7f;
		const gfx::Rect pv = m_assetPicker.PreviewRect(static_cast<float>(m_device.Width()),
														static_cast<float>(m_device.Height()));
		pvAspect = pv.h > 0.0f ? pv.w / pv.h : 1.0f;
	} else if (m_assetDialog.IsOpen() && m_assetDialog.HasPreview()) {
		pvMesh = &m_assetDialog.PreviewMesh();
		pvMat = m_assetDialog.PreviewMaterial();
		pvOrbit = m_assetDialog.Orbit();
		pvPivot = m_assetDialog.PreviewPivot();
	} else if (m_monsterDialog.IsOpen() && m_previewMonMesh) {
		// The monster-config dialog's live animation: a fixed front-on view (the
		// mesh faces +Z / the camera is at -Z, so ~π turns it toward the camera),
		// rendered at the (tall) preview pane's aspect so it isn't squashed.
		// Every piece draws (a multi-material rig previews bones+armor+weapons).
		const gfx::Rect pv = m_monsterDialog.PreviewRect(
			static_cast<float>(m_device.Width()), static_cast<float>(m_device.Height()));
		pvSubs = m_previewMonSubs;
		pvScale = m_previewMonScale;
		pvOrbit = kPi + m_previewMonYaw; // face the camera + the model's facing fixup
		pvPivot = m_previewMonPivot;
		pvAspect = pv.h > 0.0f ? pv.w / pv.h : 1.0f;
		pvPalette = m_previewAnim.Palette();
	} else if (InstanceInspector* ii = ActiveInstanceInspector(); ii && ii->HasPreview()) {
		// A per-instance edit dialog's live preview, read generically from its spec:
		// mesh(es) (animated for a monster), rendered at the pane's aspect, front-on,
		// with the torch's flame/smoke overlaid when lit.
		const PreviewSpec& sp = ii->Preview();
		const gfx::Rect pv = ii->PreviewRect(static_cast<float>(m_device.Width()),
											 static_cast<float>(m_device.Height()));
		pvSubs = sp.subs;
		pvScale = sp.scale;
		pvOrbit = kPi + sp.yaw + (sp.spin ? m_previewSpin : 0.0f);
		pvPivot = sp.pivot;
		pvAspect = pv.h > 0.0f ? pv.w / pv.h : 1.0f;
		if (sp.skeleton) pvPalette = m_previewAnim.Palette();
		if (sp.autoFit) {
			pvFitMin = &sp.fitMin;
			pvFitMax = &sp.fitMax;
		}
		if (sp.fire && sp.showFire) { // lit torch: overlay flame/smoke
			m_previewFireScratch.clear();
			m_previewFire.AppendParticles(m_previewFireScratch);
			pvParticles = &m_previewParticles;
			pvBillboards = m_previewFireScratch;
		}
	} else if (m_previewMesh) {
		pvMesh = m_previewMesh.get();
		pvMat = m_previewMaterial;
		pvOrbit = m_previewOrbit;
	}
	const bool devPreviewFullscreen = m_previewMesh && !m_assetDialog.IsOpen() &&
									  !m_monsterDialog.IsOpen();

	if (!pvSubs.empty()) { // per-instance dialog preview (one or many submeshes)
		if (pvParticles) pvParticles->NewFrame(m_device.FrameIndex());
		m_modelPreview.Render(list, m_renderer, pvSubs, pvScale, pvOrbit, pvAspect, pvPalette,
							  pvParticles, pvBillboards, pvFitMin, pvFitMax, pvOrient, &pvPivot,
							  pvViewTilt);
		m_device.BindBackBuffer(list);
	} else if (pvMesh) {
		m_modelPreview.Render(list, m_renderer, *pvMesh, pvMat, pvScale, pvOrbit, pvAspect,
							  pvPalette, nullptr, {}, &pvPivot);
		m_device.BindBackBuffer(list);
	}
	// The 3D scene draws during play and under the pause menu (frozen) and the
	// character sheet (live) opened from play - BackdropState, so the two opened
	// from the WORLD MAP draw the world map instead and spend no 3D frame on the
	// parked level behind it (code-review C365). Loading and Menu are 2D-only.
	// The full-screen dev preview replaces it; the editor map and dialog skip it
	// too.
	else if (backdrop == AppState::Playing && !editorMap) {
		m_drewScene = true;
		{
			DN_PROFILE_ZONE_L(prof::kLevelSystem, "icons");
			m_world->UpdateItemIcons(list, m_spriteBatch); // 3D item icons (static + spin)
			m_world->UpdateMapIcons(list, m_spriteBatch);  // map marker icons (one-shot)
		}
		{
			DN_PROFILE_ZONE_L(prof::kLevelSystem, "shadows");
			DN_GPU_ZONE(m_device.Gpu(), list, "gpu.shadows");
			m_world->RenderShadowMaps(list);
		}
		// The scene renders linear HDR into the post target; Resolve runs the
		// bloom chain + ACES composite and leaves the back buffer bound for
		// the 2D pass below.
		{
			DN_PROFILE_ZONE_L(prof::kLevelSystem, "scene");
			DN_GPU_ZONE(m_device.Gpu(), list, "gpu.scene");
			m_postProcess.BeginScene(list);
			m_world->RenderScene(list);
		}
		{
			DN_PROFILE_ZONE_L(prof::kLevelSystem, "post");
			DN_GPU_ZONE(m_device.Gpu(), list, "gpu.post");
			m_postProcess.Resolve(list);
		}
	} else if (editorMap) {
		// The editor covers the scene, but its map overlay draws the baked
		// marker icons — keep the bakes running (a kind placed from the palette
		// bakes on the next frame; item icons also feed the map's item markers).
		// The bakes rebind the back buffer themselves when they ran.
		m_world->UpdateItemIcons(list, m_spriteBatch);
		m_world->UpdateMapIcons(list, m_spriteBatch);
	} else if (m_state == AppState::CharacterSheet && m_world) {
		// The sheet over the world map: no scene, but its pack shows item icons,
		// and the animated ones re-bake every frame (the 3D block does this for
		// a sheet opened in a level).
		m_world->UpdateItemIcons(list, m_spriteBatch);
	}
	// The item details dialog's turning model (docs/ui-updates-plan.md P3), drawn
	// AFTER the scene - the dialog sits over a live world, so unlike the editor
	// dialogs above it must not replace the scene pass. It borrows the editor's
	// ModelPreview target, and so only while no editor preview holds it (an
	// editor dialog and a play dialog are never both up; this makes it certain).
	if (m_ui.ItemDetailsOpen() && pvSubs.empty() && !pvMesh && !editorMap) {
		const ItemDetailsDialog& dlg = *m_ui.DetailsDialog();
		if (!dlg.PreviewSubs().empty()) {
			const gfx::Rect pv = dlg.PreviewRect();
			m_modelPreview.Render(list, m_renderer, dlg.PreviewSubs(), dlg.PreviewScale(),
								  kPi + dlg.Spin(), pv.h > 0.0f ? pv.w / pv.h : 1.0f, {},
								  nullptr, {}, &dlg.FitMin(), &dlg.FitMax(), &dlg.Pose());
			m_device.BindBackBuffer(list);
		}
	}
	// The asset picker's model tiles bake in the same phase (they need the
	// command list). Only the DRAW is recorded here — the picker made the mesh
	// and the target back in Update, because creating a render target drains the
	// GPU and doing that mid-recording corrupted the next bake in the frame.
	if (m_assetPicker.IsOpen()) {
		bool baked = false;
		for (const AssetPicker::PendingBake& bake : m_assetPicker.PendingBakes(2)) {
			m_world->BakeIconFor(list, m_spriteBatch, bake.parts, bake.lo, bake.hi,
								*bake.target, bake.palette, bake.tilt, bake.yaw);
			m_assetPicker.MarkBaked(bake.name);
			baked = true;
		}
		// A bake redirects the output merger at its own 256px target; leaving it
		// there sends the 2D pass into the last icon baked (a squashed copy of
		// the whole screen, which is exactly what the tiles showed). Same
		// epilogue UpdateMapIcons has.
		if (baked) m_device.BindBackBuffer(list);
	}

	// 2D pass.
	DN_PROFILE_ZONE_L(prof::kLevelSystem, "ui2d");
	DN_GPU_ZONE(m_device.Gpu(), list, "gpu.ui2d");
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	// The BACKDROP'S picture first, then - for the pause menu and the sheet -
	// their own page over it under a dark wash. Switching on BackdropState rather
	// than m_state is what puts the world map, not the parked dungeon, under the
	// two when they were opened from it (C365). `drawn` is set in the case that
	// DREW the picture, never from `backdrop`: the `backdrop` readout must be
	// able to say a frame drew nothing behind the pause menu.
	std::optional<AppState> drawn;
	switch (backdrop) {
	case AppState::Loading:
		m_ui.RenderLoadingScreen(m_loadQueue);
		drawn = AppState::Loading;
		break;
	case AppState::Menu:
		m_ui.RenderMenuOverlay();
		drawn = AppState::Menu;
		break;
	case AppState::LoadingGame:
		m_ui.RenderGameLoadingScreen(m_loadQueue);
		drawn = AppState::LoadingGame;
		break;
	case AppState::LoadingLevel:
		m_ui.RenderGameLoadingScreen(m_loadQueue);
		drawn = AppState::LoadingLevel;
		break;
	case AppState::Playing: {
		// Under the pause menu and the sheet the 3D scene alone stands behind
		// their wash, as it always has - no HUD, no map. That scene IS this
		// backdrop's picture, so it counts as drawn only if the 3D pass ran.
		if (m_state != AppState::Playing) {
			if (m_drewScene) drawn = AppState::Playing;
			break;
		}
		drawn = AppState::Playing;
		const float dw = static_cast<float>(m_device.Width());
		const float dh = static_cast<float>(m_device.Height());
		if (editorMap) {
			// Full-screen editor — drawn alone, no HUD or scene behind it.
			m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
		} else {
			m_ui.RenderHud();
			if (m_mapView.IsOpen()) {
				// Player map: dim the scene behind the 80% panel, over the HUD.
				m_spriteBatch.DrawRect({0, 0, dw, dh}, {0, 0, 0, 0.45f});
				if (ShowingWorldPage()) {
					m_worldMapView.Render(m_spriteBatch, m_settings.theme,
										  *m_worldMap, m_worldState,
										  MapPanel(dw, dh));
					m_worldViewFrame = m_framesRendered + 1; // for `worldview`
				} else {
					m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
				}
			}
		}
		// The deferred-rebake notice (see Update): the frame the blocking
		// FlushGeometry freezes on, so the pause reads as work, not a hang.
		if (m_geomNoticeLatched) DrawBusyNotice(loc::Tr("map.rebuilding"), dw, dh);
		m_ui.RenderConfirmOverlay(); // "Leave the Crypt?", when an exit asks
		break;
	}
	case AppState::WorldMap:
		if (m_worldMap) {
			m_worldMapView.Render(m_spriteBatch, m_settings.theme, *m_worldMap,
								  m_worldState,
								  WorldPanel(static_cast<float>(m_device.Width()),
											 static_cast<float>(m_device.Height())));
			drawn = AppState::WorldMap;
			m_worldViewFrame = m_framesRendered + 1; // for `worldview`
		}
		// "Enter the Crypt?", when a step asks - on the travel screen itself,
		// never under the pause menu (whose own confirm draws in its overlay).
		if (m_state == AppState::WorldMap) m_ui.RenderConfirmOverlay();
		break;
	case AppState::Paused:
	case AppState::CharacterSheet:
		// Never a backdrop of their own: BackdropState resolves both to where
		// they were opened from. (Only a sheet opened over the pause menu from
		// the console gets here, and draws its page over nothing - so `drawn`
		// stays empty.)
		break;
	}
	m_drawnBackdrop = drawn;
	m_backdropFrame = m_framesRendered + 1; // this frame, counted at the end
	if (m_state == AppState::Paused) m_ui.RenderPauseOverlay();
	else if (m_state == AppState::CharacterSheet) m_ui.RenderCharacterSheetOverlay();
	const float dw = static_cast<float>(m_device.Width());
	const float dh = static_cast<float>(m_device.Height());
	// The item details dialog, over the HUD or the sheet it was opened from, and
	// its model blitted into the pane (rendered before the 2D pass, above).
	if (m_ui.ItemDetailsOpen() &&
		(m_state == AppState::Playing || m_state == AppState::CharacterSheet)) {
		m_ui.RenderItemDetails();
		const ItemDetailsDialog& dlg = *m_ui.DetailsDialog();
		if (!dlg.PreviewSubs().empty() && !editorMap) {
			const gfx::Rect pv = dlg.PreviewRect();
			m_spriteBatch.DrawSprite(pv, {0, 0, 1, 1}, m_modelPreview.Srv(), {1, 1, 1, 1});
			// A burning item burns here too: its head projected through the very
			// framing the image was rendered with, the flame drawn over it. Sized
			// to the torch as the hand icon sizes it - the fitted model spans
			// about half the pane at the dialog's burning scale, so the flame
			// stands 0.30 of it and the glow spreads 0.36.
			Vec2 uv;
			if (const Vec3* head = dlg.FlameHead();
				head && m_flameTexture && pv.h > 0.0f &&
				gfx::ModelPreview::Project(*head, dlg.PreviewScale(), kPi + dlg.Spin(),
										   pv.w / pv.h, &dlg.FitMin(), &dlg.FitMax(),
										   &dlg.Pose(), uv))
				DrawFlame(m_spriteBatch, {pv.x + uv.x * pv.w, pv.y + uv.y * pv.h}, pv.h * 0.30f,
						  pv.h * 0.36f, 0.0f, *m_flameTexture, m_flameGlowTexture.get(),
						  dlg.FlameTint());
		}
	}
	// The portrait picker, over the sheet (or the HUD, from the console).
	if (m_ui.PortraitPickerOpen() &&
		(m_state == AppState::Playing || m_state == AppState::CharacterSheet))
		m_ui.RenderPortraitPicker();
	if (m_assetDialog.IsOpen()) {
		// The asset dialog overlays the editor; it draws its own frame, then we
		// blit the rendered preview model into its preview pane.
		m_assetDialog.Render(m_spriteBatch, dw, dh);
		if (m_assetDialog.HasPreview())
			m_spriteBatch.DrawSprite(m_assetDialog.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	} else if (devPreviewFullscreen) {
		// Dev `preview` command: dim the frame and blit the model full-screen.
		m_spriteBatch.DrawRect({0, 0, dw, dh}, {0.04f, 0.04f, 0.06f, 1.0f});
		const float s = std::min(dw, dh) * 0.85f;
		m_spriteBatch.DrawSprite({(dw - s) * 0.5f, (dh - s) * 0.5f, s, s},
								 {0, 0, 1, 1}, m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	if (m_monsterDialog.IsOpen()) { // modal over the editor, like the asset dialog
		m_monsterDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
		if (m_previewMonMesh) // blit the live animation into the preview pane
			m_spriteBatch.DrawSprite(m_monsterDialog.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	if (m_balanceDialog.IsOpen()) // modal over the editor, like the others
		m_balanceDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_levelSettingsDialog.IsOpen())
		m_levelSettingsDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	// Modal over the WORLD screen rather than the editor map, but drawn in the
	// same place as every other dialog: after the state switch, so whichever
	// screen is up is already behind it.
	if (m_worldSettingsDialog.IsOpen())
		m_worldSettingsDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_worldsDialog.IsOpen())
		m_worldsDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_newWorldDialog.IsOpen()) // over the Worlds dialog that may have opened it
		m_newWorldDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_generateDialog.IsOpen())
		m_generateDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_validateDialog.IsOpen())
		m_validateDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_typeDialog.IsOpen())
		m_typeDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	// The asset picker draws OVER the type editor it was opened from, and blits
	// the selected asset's preview into its own pane (the asset dialog's seam).
	if (m_assetPicker.IsOpen()) {
		m_assetPicker.Render(m_spriteBatch, dw, dh);
		if (m_assetPicker.HasPreview())
			m_spriteBatch.DrawSprite(m_assetPicker.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	// The per-instance edit dialogs, each drawn (panel + controls) THEN, once all
	// are drawn, the 3D preview blitted into the active one's pane — the blit must
	// come last so a dialog's backing box never covers it.
	for (InstanceInspector* ii : InstanceInspectors())
		if (ii->IsOpen()) ii->Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_projectileInspector.IsOpen())
		m_projectileInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (InstanceInspector* ii = ActiveInstanceInspector(); ii && ii->HasPreview())
		m_spriteBatch.DrawSprite(ii->PreviewRect(dw, dh), {0, 0, 1, 1}, m_modelPreview.Srv(),
								 {1, 1, 1, 1});
	if (m_inspectPicker.IsOpen()) // multi-object chooser, modal over the editor
		m_inspectPicker.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_mapIconSurvey && m_world) DrawMapIconSurvey(dw, dh); // dev `mapicons survey`
	if (m_console.IsOpen())
		m_console.Render(m_spriteBatch, m_device, static_cast<float>(m_device.Width()),
						 static_cast<float>(m_device.Height()));
	// The pending quality swap's notice. Drawn LAST and outside the per-state
	// switch on purpose: the tier can be picked from the landing page (Menu),
	// the pause menu (Paused) or the dev console over any state, and this is the
	// frame the reload will stall on — nothing may cover it.
	if (m_pendingQuality) DrawBusyNotice(loc::Tr("settings.applying"), dw, dh);
	m_spriteBatch.End();

	// Every UIContext that rendered has had its turn at the armed overlap audit;
	// this is the only place that knows the frame is over. No-op unless armed.
	ui::inspect::EndOverlapAuditFrame();
	++m_framesRendered;
}

// HEADLESS (`-headless`): the end-of-frame bookkeeping without the drawing.
//
// This exists because of ONE line above — `++m_framesRendered`. The staged
// loader gates on it (`RunLoadTasks`: a task runs only once the state's screen
// has been PRESENTED at least once, so a multi-second bake never happens on a
// frame nobody has seen). Skip Render naively and that counter never moves, the
// load queue never advances, and a headless run sits on the loading screen
// forever, reporting nothing, looking exactly like a hang.
//
// So the counter is frame accounting that merely LIVED in the render pass. The
// increment stays where it is for the normal path — moving it would change what
// "presented" means for everyone to fix a case that has no screen — and headless
// says the same thing in its own words. The overlap audit is ended too, for the
// same reason it is ended there: something has to say the frame is over, and
// `uioverlap` armed in a headless run must not wait forever for a pass that will
// never come. (It will report nothing, of course — a widget that never drew has
// no ink. `/check-ingame` deliberately does NOT run headless.)
void Game::EndHeadlessFrame() {
	ui::inspect::EndOverlapAuditFrame();
	++m_framesRendered;
}

} // namespace dungeon::game
