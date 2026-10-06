// ============================================================================
// Game/Game_Wiring.cpp — split out of Game.cpp to keep files small (see Game.h).
// Module callback wiring (the on* handlers): WireModuleCallbacks once from the
// ctor, WireWorldCallbacks each time LoadWorld builds a world.
// ============================================================================
#include "Game/Game.h"

#include "Assets/WornSets.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath> // fabs — "is this slider still on the type's value?"
#include <string>
#include <utility>

namespace dungeon::game {
namespace {
// The clean SURFACE BLOCKS (AssetBaker's block family; *_block.gltf). No catalog
// draws them, so the asset picker dresses each in the project's first type of
// its surface and views it from where a player meets that surface.
struct SurfaceBlock {
	const char* model;
	DungeonWorld::PoolModelLook::Mount mount;
	Catalog Project::*surfaces;
};
constexpr SurfaceBlock kSurfaceBlocks[] = {
	{"floor_block", DungeonWorld::PoolModelLook::Mount::Floor, &Project::floors},
	{"wall_block", DungeonWorld::PoolModelLook::Mount::Wall, &Project::walls},
	{"ceiling_block", DungeonWorld::PoolModelLook::Mount::Ceiling, &Project::ceilings},
};
} // namespace

// The callbacks that live ON the world object, so they are wired each time a
// world is built (LoadWorld) rather than once: world feedback goes to the HUD.
void Game::WireWorldCallbacks() {
	m_world->onMessage = [this](std::string_view line) {
		m_ui.AddLogLine(line);
		// The full-screen editor draws no HUD, so its reports would otherwise go
		// nowhere visible: show them on the editor's own message line, and put
		// them in dungeon.log, where a harness (and a play-test) can read them.
		// Editor frames are not guarded (SteadyStateFrame), so the log's
		// formatting is free to allocate here.
		if (m_mapView.IsOpen() && m_mapView.CurrentMode() == MapView::Mode::Editor) {
			m_mapView.ShowStatus(line);
			log::Info("editor: {}", line);
		}
	};
	// Lines about a specific member arrive with their identity color; the log
	// tints them so each character's doings read at a glance.
	m_world->onMemberMessage = [this](std::string_view line, const Vec4& color) {
		m_ui.AddLogLine(line, color);
	};
	// The party fell: end the run back at the title (Start New Game resets the
	// roster + monsters in place).
	m_world->onPartyWipe = [this] { ReturnToTitle("the party fell"); };
}

void Game::WireModuleCallbacks() {
	// Wire the modules together: UI actions drive the state machine. (The
	// world's own callbacks: WireWorldCallbacks.)
	m_ui.onStartNewGame = [this] {
		// A new game in the RESIDENT world — or, with none loaded (the title
		// screen, or the harness's cold `reset`), in the default world.
		if (!m_world && !LoadWorld(m_defaultWorld)) {
			log::Warn("new game: the default world '{}' could not be opened",
					  m_defaultWorld);
			return;
		}
		if (m_gameLoaded) {
			StartNewGame();
		} else {
			// First start: the boot load only fetched menu essentials, so
			// the dungeon loads now behind its own progress screen.
			BuildGameLoadTasks();
			m_state = AppState::LoadingGame;
			m_stateFrameMark = m_framesRendered;
		}
	};
	// The worlds "Start New Game" offers. A `-project` launch offers only its
	// own: the world is already chosen for that run (it is how a harness or a
	// test scenario opens one), so the list must not stand between it and the
	// game — the keystroke harnesses press Enter there and expect to be playing.
	m_ui.onListWorlds = [this] {
		std::vector<GameUI::WorldChoice> worlds;
		const std::string root = paths::Asset("projects");
		for (const std::string& folder : Project::List(root)) {
			if (m_worldFromCommandLine && folder != m_project.FolderName()) continue;
			worlds.push_back({folder, Project::ReadName(Project::FolderFor(root, folder))});
		}
		// BY TITLE, ignoring case: the folder sort is byte order, which put
		// "Test-World" above "Dungeon Demo" because capitals sort first.
		const auto key = [](const GameUI::WorldChoice& w) {
			std::string k = w.display.empty() ? w.folder : w.display;
			for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return k;
		};
		std::sort(worlds.begin(), worlds.end(),
				  [&](const GameUI::WorldChoice& a, const GameUI::WorldChoice& b) {
					  return key(a) < key(b);
				  });
		return worlds;
	};
	m_ui.onStartNewGameIn = [this](const std::string& folder) { StartNewGameIn(folder); };
	// Party creation (docs/party-creation-plan.md phase 3): the world first, then
	// the page; the page's Start builds the party and starts the game.
	m_ui.onOpenPartyCreation = [this](const std::string& folder) { OpenPartyCreation(folder); };
	m_ui.onStartParty = [this](const std::vector<party::MemberSpec>& specs, std::string& why) {
		return StartWithParty(specs, why);
	};
	// The title screen's Settings edit only the new-member colours; a paused
	// game's edit its party too (phase 4).
	m_ui.partyInPlay = [this] { return m_gameLoaded && m_state != AppState::Menu; };
	m_ui.onEditorOnArrival = [this](bool on) { m_editorOnArrival = on; };
	m_ui.onQuit = [this] { m_quitRequested = true; };
	m_ui.onResume = [this] { m_state = m_resumeState; };
	// The pause menu's Return to Main Menu: the trip a party wipe makes, taken
	// on purpose. The dungeon stays resident (m_gameLoaded), so the title's
	// Continue / Load / Start New Game reset it in place like any other.
	m_ui.onReturnToMain = [this] { ReturnToTitle("return to main menu"); };
	m_ui.onLoadSave = [this](const std::string& path) {
		// A SAVE BELONGS TO A WORLD (SaveGame.h): load that world first when it
		// is not the one resident — deferred, since this fires from inside a
		// menu's widget walk and the switch destroys the world.
		const std::optional<SaveHeader> head = ReadSaveHeader(path);
		if (!head) return; // missing, or refused (ReadSaveHeader logged why)
		if (!m_world || head->world != m_project.FolderName()) {
			// Not under a running bake, which lands in whatever world is loaded
			// (code-review C234; SwitchWorld's rule, asked the same way).
			if (const std::string busy = WorldSwitchRefusal(); !busy.empty()) {
				log::Warn("load '{}' refused: {}", path, busy);
				return;
			}
			m_pendingWorld = PendingWorld{head->world, path};
			return;
		}
		if (m_gameLoaded) {
			LoadGame(path); // dungeon resident (pause-menu Load): apply now
		} else {
			// Landing-page Load/Continue on a cold start: stage the dungeon
			// load first, then apply the save when it finishes (see Update).
			m_pendingLoadPath = path;
			BuildGameLoadTasks();
			m_state = AppState::LoadingGame;
			m_stateFrameMark = m_framesRendered;
		}
	};
	m_ui.saveWorld = [this] { return SaveListWorld(); };
	m_ui.onSaveSlot = [this](const std::string& name) {
		// Resume EITHER WAY: a refused save (inside a random encounter) must
		// not also strand the player on the save page with no explanation —
		// SaveGame has already said why through the message log.
		SaveGame(name);
		m_state = m_resumeState; // resume after saving from the pause menu
	};
	m_ui.onOpenSheet = [this](size_t index) { OpenCharacterSheet(index); };
	// A pick in the portrait picker (docs/portraits-plan.md).
	m_ui.onSetPortrait = [this](size_t member, const std::string& id) {
		return SetPortrait(member, id);
	};
	m_ui.onChangePortrait = [this](size_t member) { OpenPortraitPicker(member); };
	// Sheet "All" button: leave the sheet and bring up the party window - every
	// member on the sheet's tab, over the live world (Game/PartyWindow.h).
	m_ui.onShowPartyInventory = [this] {
		m_state = m_resumeState;
		m_ui.OpenInventory(m_ui.SheetMode()); // on the tab the sheet was showing
	};
	// Quality: recorded, not applied — the swap blocks for seconds, so Update
	// runs it next frame with the "applying" notice already on screen. A
	// re-pick of the live tier is dropped so the notice never flashes for a
	// no-op (SetQuality would early-out anyway).
	m_ui.onQualitySelected = [this](int index) {
		const Quality q = static_cast<Quality>(index);
		if (q != m_settings.quality) m_pendingQuality = q;
	};
	// Video tab frame-rate cap: live (just a present-interval change on the
	// device), persisted immediately like the language/key binds.
	m_ui.onFrameLimitSelected = [this](int index) {
		m_settings.presentInterval = kPresentIntervals[index];
		m_device.SetPresentInterval(m_settings.presentInterval);
		m_settings.Save();
	};
	// Video tab Apply: a monitor/resolution/mode change rebuilds the swapchain in
	// place; an adapter change can't (the device is bound to its GPU), so it
	// persists the choice and relaunches.
	m_ui.onVideoApply = [this] {
		m_settings.Save();
		ApplyDisplaySettings();
	};
	m_ui.onAdapterRestart = [this] {
		m_settings.Save();
		RestartApp();
	};
	// The sheet's defense breakdown: only the world can resolve worn items,
	// balance knobs and the live evasion formula.
	// The sheet is rebuilt on a LANGUAGE SWITCH, which can happen on the title
	// screen with no world resident yet - so its reads answer "nothing" then.
	m_ui.defenseFor = [this](const Character& c) {
		return m_world ? m_world->DefenseFor(c) : DefenseReadout{};
	};
	m_ui.defenseWith = [this](const Character& c, const std::string& id) {
		return m_world ? m_world->DefenseWith(c, id) : DefenseReadout{};
	};
	// The stance slider under a member's hands (docs/damage-system.md). Its
	// stance runs PAST 1, as far as exert_max: over-exertion costs something now
	// (a per-swing bill out of stamina and then health — DungeonWorld::
	// SpendExertion), so charging into it is a legitimate choice rather than a
	// free win. The clamp lives here rather than in the widget so the balance
	// knob stays the one authority on how far the stance goes; the dev `guard`
	// command still has no clamp at all. The widget also READS the knob live
	// (exertMax), because it shows over-exertion as a percentage of the way to
	// it - a charge to 100% is a charge to exert_max.
	m_ui.onGuardChange = [this](size_t member, float share) {
		if (member < m_characters.size())
			m_characters[member].offenseShare =
				std::clamp(share, 0.0f, m_world->GetBalance().exertMax);
	};
	m_ui.exertMax = [this] { return m_world ? m_world->GetBalance().exertMax : 1.0f; };
	// The party leader is the world's (it passes when a member falls, and it is
	// saved with the party); the HUD only reads it and asks to change it.
	m_ui.partyLeader = [this] { return m_world ? m_world->Leader() : 0; };
	m_ui.onPickLeader = [this](size_t member) {
		if (m_world) m_world->SetLeader(static_cast<int>(member));
	};
	m_ui.onMoveAction = [this](MoveAction action) {
		// A pit fall swallows movement (the keyboard path gates in
		// DungeonWorld::Update; this is the HUD arrow-button path).
		if (!m_world->Falling()) m_world->GetParty().Act(action);
	};
	m_ui.moveCounter = [this](MoveAction& last) -> unsigned {
		return m_world ? m_world->GetParty().ActCount(last) : 0u;
	};
	m_ui.onHandAttack = [this](size_t member, size_t hand, std::string_view verb) {
		m_world->PartyAttack(member, hand, verb);
	};
	m_ui.onHandThrow = [this](size_t member, const std::string& item, float charge) {
		return m_world->ThrowItem(item, static_cast<int>(member), charge);
	};
	m_ui.torchActFor = [this](const std::string& item) {
		return m_world ? static_cast<int>(m_world->TorchActFor(item)) : 0;
	};
	m_ui.onTorchAct = [this](size_t member, size_t hand, bool light) {
		if (light) m_world->KindleTorch(member, static_cast<int>(hand));
		else m_world->PutOutTorch(member, static_cast<int>(hand));
	};
	// The hand right-click menu reads an item's commands from the world's item
	// kinds (single source — ItemKindFor parses category/command + rune defaults).
	// The return type is spelled out: deduced, it would be a COPY, and the
	// std::function's reference would dangle.
	m_ui.itemCommands = [this](const std::string& id) -> const std::vector<std::string>& {
		return m_world->ItemCommands(id);
	};
	// ...and whether it is a rune tablet, from the same kinds (code-review C347).
	m_ui.itemRune = [this](std::string_view id, SpellSymbol& symbol) {
		return m_world && m_world->ItemRune(id, symbol);
	};
	// The hand menu's Magic group enumerates the recipe table (filtered by the
	// member's vocabulary in GameUI); a picked "cast:<id>" default casts through
	// the world's façade — the same vocab/mana gates as the dev `cast` command.
	// No world (a language switch on the title screen rebuilds the sheet, which
	// bakes its spell list): no spells.
	m_ui.spellDefs = [this] {
		return m_world ? m_world->SpellDefs() : std::span<const std::unique_ptr<Spell>>{};
	};
	m_ui.onCastSpell = [this](size_t member, std::string_view id, size_t hand) {
		m_world->CastSpellById(member, id, static_cast<int>(hand));
	};
	// The spellbook panel casts a HAND-BUILT symbol sequence: an exact recipe
	// match casts (vocab/mana gated), anything else fizzles with its log line.
	// The book is member-driven (its selector row); `hand` arrives as
	// kBookHands so the cast credits both hands' quick-cast MRU.
	m_ui.onCastSequence = [this](size_t member, size_t hand,
								 std::span<const SpellSymbol> seq) {
		MagicSystem::CastOutcome outcome = MagicSystem::CastOutcome::Cast;
		m_world->CastSpell(member, seq, static_cast<int>(hand), &outcome);
		return outcome == MagicSystem::CastOutcome::NoMana; // keep the spell built
	};
	// Eating and drinking: the world owns the catalogs and the two supply
	// meters, so it does the arithmetic and reports what it actually restored.
	m_ui.onConsume = [this](size_t member, const std::string& id) {
		if (member >= m_characters.size()) return resource::Refill{};
		return m_world->ConsumeItem(m_characters[member], id);
	};
	m_ui.consumeLeaves = [this](const std::string& id) -> std::string_view {
		return m_world ? m_world->ConsumeLeaves(id) : std::string_view{};
	};
	// The item details dialog's two questions (docs/ui-updates-plan.md P3).
	m_ui.itemDetails = [this](const std::string& id, ItemDetails& out) {
		return m_world && m_world->ItemDetailsFor(id, out);
	};
	m_ui.itemPreview = [this](const std::string& id, std::span<gfx::PreviewSubmesh> out,
							  Vec3& fitMin, Vec3& fitMax, Mat4& pose) -> size_t {
		return m_world ? m_world->ItemPreviewForType(id, out, fitMin, fitMax, pose) : 0;
	};
	m_ui.onToggleRest = [this] { m_world->SetResting(!m_world->Resting()); };
	m_ui.onKeysChanged = [this] {
		if (m_world) m_world->GetParty().SetKeys(m_settings.moveKeys);
	};
	// (With no world loaded these are only settings; LoadWorld hands them to
	// the party the world builds.)
	m_ui.onLookChanged = [this] {
		if (m_world) m_world->GetParty().SetLook(m_settings.look);
	};
	m_ui.onHeadBobChanged = [this] {
		if (m_world) m_world->GetParty().SetHeadBob(m_settings.headBob);
	};
	// Recorded only — the rebuild would destroy the dropdown mid-callback;
	// Update applies it first thing next frame.
	m_ui.onLanguageSelected = [this](const std::string& code) {
		m_pendingLanguage = code;
		m_pendingLanguageScripted = false; // the player's pick: saved
	};

	// Editor: a palette "+ New" opens the asset-creation dialog for that category
	// (Walls/Floors/Ceilings import a texture folder; the rest import a model).
	// --- the world editor (W3, docs/world-editor-plan.md) -------------------
	// The VIEW decides what and where; the OWNER decides whether it is allowed
	// and what it costs. That split is why painting goes through a callback
	// rather than the view holding a world it could write to.
	m_worldMapView.onPaint = [this](int x, int z, const std::string& terrainId) {
		if (!m_worldMap) return;
		// Already that terrain? Then this is a drag passing back over a cell it
		// painted, and neither the undo history nor the dirty flag should hear
		// about it.
		if (m_worldMap->TerrainAt(x, z).id == terrainId) return;
		// ONE STEP PER STROKE: the first changed cell opens it, the mouse
		// release closes it (below). The dungeon editor makes the same bargain
		// and for the same reason — a drag that left forty undo steps would be
		// forty presses of Ctrl+Z to take back one gesture.
		if (!m_worldStroke) {
			m_world->BeginUndoStep();
			m_worldStroke = true;
		}
		m_worldMap->SetTerrainAt(x, z, terrainId);
	};
	m_worldMapView.onInspect = [this](int x, int z) {
		if (!m_worldMap) return;
		// Right-click on a DOORWAY opens the settings dialog on that doorway —
		// the same gesture the dungeon editor's right-click makes, one tier up.
		// Right-click on bare ground has nothing to inspect, so it reports what
		// is there instead (the world has no per-cell object to open).
		const WorldMap::Terrain& t = m_worldMap->TerrainAt(x, z);
		const WorldMap::Location* l = m_worldMap->LocationAt(x, z);
		if (l) {
			OpenWorldSettings(l->id);
			return;
		}
		m_ui.AddLogLine(
			loc::FormatLine("world.inspect", std::format("{},{}", x, z), t.id));
	};
	m_worldMapView.canUndo = [this](bool redo) {
		return redo ? m_world->CanRedo() : m_world->CanUndo();
	};
	m_worldMapView.onTool = [this](WorldMapView::Tool tool) {
		switch (tool) {
		case WorldMapView::Tool::Worlds:
			m_worldsDialog.Open(m_project.FolderName());
			break;
		case WorldMapView::Tool::NewWorld: m_newWorldDialog.Open(); break;
		case WorldMapView::Tool::Settings: OpenWorldSettings({}); break;
		case WorldMapView::Tool::Save:
			// The WORLD alone, not savemap: the band is the world screen's, and
			// a Save there that also rewrote every level would be doing three
			// things one of which you asked for.
			m_ui.AddLogLine(loc::View(SaveWorld() ? "map.world.saved"
												  : "map.world.savefailed"));
			break;
		case WorldMapView::Tool::Undo: m_world->Undo(); break;
		case WorldMapView::Tool::Redo: m_world->Redo(); break;
		case WorldMapView::Tool::None: break;
		}
	};
	// --- the player map's two pages (W6) ------------------------------------
	// Each view offers the way ACROSS and Game does the flip, so neither has to
	// know how to draw the other's grid. `hasWorld` hides the toggle in a
	// project that is all dungeon rather than dimming it.
	// (m_mapView.hasWorld is set per world, in LoadWorld.)
	m_mapView.onShowWorld = [this] { ShowMapPage(MapPage::World); };
	m_worldMapView.onShowDungeon = [this] { ShowMapPage(MapPage::Dungeon); };
	// The player map's close box, on both pages - Esc's path, not a second one.
	m_mapView.onClose = [this] { CloseMapOverlay(); };
	m_worldMapView.onClose = [this] { CloseMapOverlay(); };
	WireWorldSettingsDialog();
	// The worlds dialog is the `worlds` command's three verbs with a face, and
	// calls the SAME two functions — so the console and the dialog cannot
	// disagree about what a name is allowed to be or what switching does.
	m_worldsDialog.onList = [] { return Project::List(paths::Asset("projects")); };
	m_worldsDialog.onCreate = [this](const std::string& n) { return CreateWorld(n); };
	m_worldsDialog.onSwitch = [this](const std::string& n) { return SwitchWorld(n); };
	m_worldsDialog.canDelete = [this](const std::string& n) {
		return WorldDeleteRefusal(n);
	};
	m_worldsDialog.onDescribe = [this](const std::string& n) { return DescribeWorld(n); };
	m_worldsDialog.onDelete = [this](const std::string& n) { return DeleteWorld(n); };
	m_worldsDialog.onNewWorld = [this] { m_newWorldDialog.Open(); };

	// The new-world dialog (P4): the same CreateWorld the console's `worlds new`
	// calls, so the two cannot disagree about what a world starts with. A world
	// made while the Worlds dialog is up below it lands in that list, armed.
	m_newWorldDialog.onCreate = [this](const std::string& n, const NewWorldSpec& spec) {
		std::string problem;
		const std::string made = CreateWorld(n, spec, &problem);
		if (!made.empty() && m_worldsDialog.IsOpen()) m_worldsDialog.Created(made);
		return std::pair{made, problem};
	};
	m_newWorldDialog.onSwitch = [this](const std::string& n) {
		m_worldsDialog.Close(); // the switch ends this world; nothing to go back to
		return SwitchWorld(n);
	};
	m_newWorldDialog.onLevels = [this] { return m_project.levels; };
	m_newWorldDialog.onTags = [this] { return WizardTags(); };
	// The LIBRARY's styles (Phase 7): a new world starts in one of the shared
	// ones, since its own world has none yet to offer.
	m_newWorldDialog.onStyles = [this] {
		std::vector<std::pair<std::string, std::string>> out;
		for (const CatalogEntry& s : m_library.styles.Entries()) out.push_back({s.id, s.Display()});
		return out;
	};

	m_mapEditor.onNewAsset = [this](MapEditor::PaletteCat cat) {
		// PURE-DATA CATEGORIES SKIP THE ASSET DIALOG. A dungeon has no texture
		// to import and no model to bind, so offering "Import new / Use
		// installed / Duplicate" would be three answers to a question nobody
		// asked. They get an entry with a fresh id and the TYPE EDITOR open on
		// it — where the title is already a click-to-rename affordance, with
		// the reference sweep behind it. One naming mechanism, not two.
		if (MapEditor::CategoryAuthorable(cat)) {
			if (const std::string id = CreateAuthoredType(cat); !id.empty())
				OpenTypeEditor(cat, id);
			return;
		}
		OpenCreateDialog(cat, AssetDialog::Source::Import);
	};
	// The type editor's Duplicate: the same create dialog, opened on a copy of
	// the entry being edited. It is still an explicit Create — the clone goes
	// through id validation and the schema-seeded writer like any other new type.
	m_typeDialog.onDuplicate = [this](const TypeEditorDialog::Config& cfg) {
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(cfg.catalogKey);
		if (cat == MapEditor::PaletteCat::Count) return;
		OpenCreateDialog(cat, AssetDialog::Source::Duplicate, cfg.id);
	};
	// What a catalog entry binds — the create dialog's Duplicate mode resolves
	// the id it is copying down to a texture set / model so it can preview it.
	m_assetDialog.fieldOfType = [this](const std::string& key, const std::string& id,
									   const std::string& field) {
		const Catalog* cat = m_project.CatalogForKey(key);
		const CatalogEntry* e = cat ? cat->Find(id) : nullptr;
		return e ? e->Get(field, "") : std::string();
	};
	// "Use installed" on a surface: a set painted as another kind is refused in
	// the form, before Create (the rule is AdoptSurfaceSet's; onCreate asks it
	// again, for a pick gone stale and any way in that skips the form).
	m_assetDialog.installedRefusal = [this](const std::string& key, const std::string& asset) {
		return AdoptSurfaceSet(key, asset).refusal;
	};
	// A MODEL pick, judged by what the type it makes would load (code-review
	// C301): "Use installed" binds the picked name, a Duplicate copies its source
	// whole. The rule is UnloadableModelReason's; CreateCatalogEntry asks it
	// again of the entry it builds.
	m_assetDialog.modelRefusal = [this](const std::string& key, AssetDialog::Source source,
										const std::string& asset) {
		CatalogEntry e;
		if (source == AssetDialog::Source::Duplicate) {
			const Catalog* cat = m_project.CatalogForKey(key);
			const CatalogEntry* src = cat ? cat->Find(asset) : nullptr;
			if (!src) return std::string(); // Validate already says there is no source
			e = *src;
		} else {
			e.id = asset;
			e.Set("model", asset);
		}
		return UnloadableModelReason(key, e);
	};
	// The typed id against the RELATED catalogs (code-review C302): one item id
	// in two item catalogs, or one prop id in two prop catalogs, is refused in
	// the form; CreateCatalogEntry asks again.
	m_assetDialog.idRefusal = [this](const std::string& key, const std::string& id) {
		return RelatedIdRefusal(key, id);
	};
	// Create runs AssetBaker on the picked source (P4c); the dialog stays open in
	// a "baking…" state until Update sees the subprocess finish.
	m_assetDialog.onCreate = [this](const AssetDialog::CreateRequest& req) {
		// Installed / Duplicate bind an asset that is already baked, so the type
		// exists the moment its catalog entry does — no subprocess, no wait. The
		// one exception: a pool TEXTURE set adopted as a surface that has never
		// been painted as one has no worn block mesh yet, which is baked (the
		// import path's second step, entered directly). One whose meshes exist
		// is used as it is, and one painted as ANOTHER kind is refused - the
		// meshes are one file per set, shared by every type (AdoptSurfaceSet,
		// code-review C407). The dialog refuses it before Create, as judged when
		// the set was picked; this asks again AT Create, for a pick gone stale (a
		// type renamed onto the set since) and any other way in - and the form
		// stays open showing why.
		bool needsWornBake = false;
		if (req.source == AssetDialog::Source::Installed && req.textureSet) {
			const SurfaceAdopt adopt = AdoptSurfaceSet(req.catalogKey, req.asset);
			if (!adopt.refusal.empty()) {
				log::Warn("create {} '{}' from '{}' refused: {}", req.catalogKey, req.name,
						  req.asset, adopt.refusal);
				m_assetDialog.SetError(adopt.refusal);
				return;
			}
			needsWornBake = adopt.bake;
		}
		if (!req.NeedsBake() && !needsWornBake) {
			// Refused (a model the category could not load, C301): the form
			// stays open saying why, as for the surface check above.
			if (const std::string refused = CreateCatalogEntry(req); !refused.empty())
				m_assetDialog.SetError(refused);
			return;
		}
		m_bakeReq = req;
		m_bakeStep = needsWornBake ? 1 : 0;
		if (StartBakeStep()) {
			m_baking = true;
			m_assetDialog.SetBusy(true);
		} else {
			m_assetDialog.SetError(loc::Tr("newasset.err.launch"));
		}
	};

	// Right-click ANY palette row → the per-type catalog editor (the form comes
	// from CatalogSchema, so every category is served by one dialog).
	m_mapEditor.onConfigure = [this](MapEditor::PaletteCat cat, const std::string& id) {
		OpenTypeEditor(cat, id);
	};
	// The type editor's dropdowns for asset/reference fields.
	m_typeDialog.optionsFor = [this](const FieldSpec& spec) -> std::vector<std::string> {
		switch (spec.kind) {
		case FieldKind::TextureSet: return InstalledTextureSets();
		case FieldKind::Model: return InstalledModels();
		case FieldKind::DamageType: {
			// What the game will actually accept, asked of the registry that
			// accepts it — so a project type appears here the moment it is
			// authored, and a removed one stops being offered.
			std::vector<std::string> ids;
			for (const DamageTypeBook::Entry& e : m_world->DamageTypes().Entries())
				ids.push_back(e.id);
			return ids;
		}
		case FieldKind::CatalogRef:
		case FieldKind::CatalogRefPick: {
			std::vector<std::string> ids;
			// Two lists that are not a catalog: an item's quest hook names a
			// quest AND a stage ("<quest>:<stage>"), and its reveal a world-map
			// location.
			if (std::string_view(spec.options) == kOptQuestStages) {
				// Split as written, NOT through ParseTags: that lowercases, and a
				// stage is matched by its exact name.
				for (const CatalogEntry& q : m_project.quests.Entries()) {
					const std::string st = q.Get("stages", "");
					for (size_t i = 0; i < st.size();) {
						while (i < st.size() && st[i] == ' ') ++i;
						const size_t b = i;
						while (i < st.size() && st[i] != ' ') ++i;
						if (i > b) ids.push_back(q.id + ":" + st.substr(b, i - b));
					}
				}
				return ids;
			}
			if (std::string_view(spec.options) == kOptUiStones) return InstalledUiStones();
			if (std::string_view(spec.options) == kOptLocations) {
				if (m_worldMap)
					for (const WorldMap::Location& l : m_worldMap->Locations())
						ids.push_back(l.id);
				return ids;
			}
			if (const Catalog* c = m_project.CatalogForKey(spec.options))
				for (const CatalogEntry& e : c->Entries()) {
					// A hidden entry is internal (the palette never offers it),
					// so a list of things to paint with does not offer it either.
					if (spec.kind == FieldKind::CatalogRefPick && CatalogBool(&e, "hidden", false))
						continue;
					ids.push_back(e.id);
				}
			return ids;
		}
		default: return {};
		}
	};
	// A reference list of SURFACE types (a theme's members) shows each as
	// the palette does - name and texture swatch - by asking the palette for it.
	m_typeDialog.faceFor = [this](const FieldSpec& spec,
								  const std::string& id) -> TypeEditorDialog::RefFace {
		// A MONSTER is named with its power, so a style's list says how strong
		// each choice is while it is being chosen (Phase 2's one number).
		if (std::string_view(spec.options) == "monsters") {
			const CatalogEntry* e = m_project.monsters.Find(id);
			if (!e) return {};
			return {loc::Format("map.type.monsterpower", e->Display(),
								std::format("{:.1f}", m_world->MonsterPower(*e)))};
		}
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(spec.options);
		if (!MapEditor::SurfaceCat(cat)) return {};
		// The list is the whole catalogue, most of it not loaded by this level:
		// a thumbnail first (the dialog is built in Update, where that is safe),
		// for swatchFor to find when the row draws.
		m_mapEditor.LoadSurfaceSwatch(cat, id);
		return {m_mapEditor.SurfaceItem(cat, id).label};
	};
	// ...and its swatch, LOOKED UP each time the row draws (code-review C235):
	// the level's loaded albedo is the world's, and a quality change replaces it.
	m_typeDialog.swatchFor = [this](const FieldSpec& spec, const std::string& id) {
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(spec.options);
		return MapEditor::SurfaceCat(cat) ? m_mapEditor.SurfaceSwatch(cat, id) : ui::Swatch{};
	};
	// A monster's `power` is derived unless overridden: the row shows what the
	// stats come to (the SAVED stats - an unsaved change to them shows after
	// Save). And a surface's `relief`: unset, it bakes at its texture SET's own
	// relief (Assets/WornSets.h, the record `AssetBaker models` uses too), read
	// off the set the dialog names NOW so a re-picked texture shows its own.
	// Only those two; every other optional Float keeps its own wording.
	m_typeDialog.derivedFor = [this](const FieldSpec& f) -> std::optional<float> {
		const std::string& key = m_typeDialog.CatalogKey();
		if (std::string_view(f.key) == "relief") {
			const std::optional<assets::WornKind> kind =
				key == "walls"      ? std::optional(assets::WornKind::Wall)
				: key == "floors"   ? std::optional(assets::WornKind::Floor)
				: key == "ceilings" ? std::optional(assets::WornKind::Ceiling)
									: std::nullopt;
			if (!kind) return std::nullopt;
			// As onSave names it: the `texture` field, else the type's own id.
			const std::string* tex = serialize::Find(m_typeDialog.Fields(), "texture");
			const std::string set = tex && !tex->empty() ? *tex : m_typeDialog.Id();
			return assets::WornSetFor(set, *kind).relief;
		}
		if (key != "monsters" || std::string_view(f.key) != "power")
			return std::nullopt;
		const CatalogEntry* e = m_project.monsters.Find(m_typeDialog.Id());
		if (!e) return std::nullopt;
		return static_cast<float>(m_world->DerivedPower(*e));
	};
	// Save: merge the touched fields into the catalog, then apply. A surface
	// whose look changed needs its worn meshes re-baked before it shows.
	m_typeDialog.onSave = [this](const TypeEditorDialog::Config& cfg) -> std::string {
		// A MODEL THE CATEGORY CANNOT LOAD is refused before anything is written
		// (code-review C301): the reload below - or the next level load - opens
		// it through LoadModelOrDie. The entry is judged as the Save would write
		// it, so a model cleared back to the id, or never fixed, counts as well.
		// So is a terrain glyph the world could not be read with (C345).
		if (const std::string why = TypeSaveRefusal(cfg.catalogKey, MergedTypeEntry(cfg));
			!why.empty()) {
			log::Warn("type editor: save of {} '{}' refused: {}", cfg.catalogKey, cfg.id, why);
			return why;
		}
		WriteTypeFields(cfg);
		if (!cfg.rebake) {
			// Nothing BAKED is stale, so the change can just take effect. A
			// surface's per-draw knobs (parallax depth, metallic/roughness) push
			// straight at the live scene; a prop's are baked into its cached
			// KIND at load, so that kind is dropped and its instances re-spawned.
			if (MapEditor::SurfaceCat(MapEditor::CatForCatalogKey(cfg.catalogKey)))
				m_world->RefreshSurfaceMaterials();
			// A theme is referenced, not copied: every square painted
			// with it takes the new definition, on every level that has one.
			else if (cfg.catalogKey == "themes")
				m_world->RefreshTheme(cfg.id);
			// A light profile is looked up by id every frame: re-reading the
			// catalog is the whole reload (lighting-updates Phase 2).
			else if (cfg.catalogKey == "lights" || cfg.catalogKey == "trails")
				m_world->ReloadLightProfiles();
			// A terrain is the world map's, handed to it by WriteTypeFields
			// (SyncWorldTerrains); no level holds a kind of it to reload.
			else if (cfg.catalogKey != "terrain") {
				// The reload frees this kind's mesh and respawns EVERY object
				// with a new id - keeping what the editor and play did to the
				// level (HoldActiveState / RestoreHeldState; C311), and only for
				// a category whose kinds are cached (ReloadTypeKind; an item's
				// kind is rebuilt in place instead, nothing respawned) - and an
				// open inspector, or the monster dialog, borrows that mesh for
				// its preview and names its object by the old id. A mouse cannot
				// get here with one open (each is modal); `typeset` can (C232).
				CloseInspectors();
				if (m_monsterDialog.IsOpen()) {
					m_monsterDialog.Close();
					ForgetMonsterPreview();
				}
				// The item details dialog too, for an item: its preview was filled
				// once, at Open, with the kind's own meshes and textures, and the
				// rebuild in place below frees that model (a quality swap closes
				// it for the same reason, SetQuality). `typeset` reaches here with
				// it open; the next frame drew freed meshes.
				if (Project::IsItemCatalog(cfg.catalogKey)) m_ui.CloseItemDetails();
				m_world->ReloadTypeKind(cfg.catalogKey, cfg.id);
				// An ITEM's kind was rebuilt in place (code-review C302), so the
				// UI's banks that mirror it follow (C330) - its icon, weight,
				// holdable and wear - and the light it names is checked.
				if (Project::IsItemCatalog(cfg.catalogKey)) {
					RefreshItemBanks();
					m_world->ReloadLightProfiles();
				}
			}
			if (m_world->onMessage)
				m_world->onMessage(loc::FormatLine("map.type.saved", cfg.id));
			return std::string();
		}
		const CatalogEntry* e = m_project.CatalogForKey(cfg.catalogKey)
									? m_project.CatalogForKey(cfg.catalogKey)->Find(cfg.id)
									: nullptr;
		// An unset `relief` passes -1: the baker takes the texture SET's own
		// (Assets/WornSets.h), the depth `AssetBaker models` bakes it at, so a
		// save that touched only `texture` or `wear` cannot reshape the set.
		StartRestyleBake(cfg.catalogKey, CatalogGet(e, "texture", cfg.id),
						 e ? e->GetFloat("wear", 1.0f) : 1.0f,
						 e ? e->GetFloat("relief", -1.0f) : -1.0f);
		if (m_restyleBake) m_typeDialog.SetBusy(true); // bake launched
		return std::string();
	};
	// A `texture` / `model` field opens the POOL BROWSER rather than a dropdown.
	// The picker knows nothing about catalogs: it is handed the current value and
	// hands back the pick, which goes straight into the field that asked.
	m_typeDialog.onPickAsset = [this](bool textures, const std::string& current,
									  std::function<void(const std::string&)> apply) {
		m_pickApply = std::move(apply);
		m_assetPicker.Open(textures ? AssetPicker::Mode::Textures
									: AssetPicker::Mode::Models,
						   current,
						   loc::Tr(textures ? "map.type.texture" : "map.type.model"),
						   m_settings.theme);
	};
	// The create dialog's "Use installed" field browses the same pool the same way.
	m_assetDialog.onPickAsset = [this](bool textures, const std::string& current,
									   std::function<void(const std::string&)> apply) {
		m_pickApply = std::move(apply);
		m_assetPicker.Open(textures ? AssetPicker::Mode::Textures
									: AssetPicker::Mode::Models,
						   current,
						   loc::Tr(textures ? "map.type.texture" : "map.type.model"),
						   m_settings.theme);
	};
	m_assetPicker.onChoose = [this](const std::string& picked) {
		if (m_pickApply) m_pickApply(picked);
		m_pickApply = nullptr;
	};
	// "In use" = bound by some entry in some catalog of this project. Asked once
	// per open, so walking every catalog is cheap enough to keep honest.
	m_assetPicker.usedAssets = [this] {
		std::vector<std::string> out;
		const char* fields[] = {"texture", "model", "part2_texture", "part2_model"};
		for (const Catalog* cat : m_project.AllCatalogs())
			for (const CatalogEntry& e : cat->Entries())
				for (const char* field : fields) {
					const std::string v = e.Get(field, "");
					if (!v.empty() && std::ranges::find(out, v) == out.end())
						out.push_back(v);
				}
		return out;
	};
	// The set a model is drawn in: the first catalog entry drawing that model
	// names it, by the world's own rule (ModelFileOf / TextureOf: `model` and
	// `texture` each default to the entry's id; a second part pairs part2_model
	// with part2_texture). A model no entry uses falls back to a set of its own
	// name, the import convention.
	m_assetPicker.textureFor = [this](const std::string& model) {
		// A FEATURE has no set of its own: it is stamped in place of a surface
		// block and wears that cell's texture. Show it in the project's first
		// type of that surface, which is what it will look like in a level.
		auto firstOf = [](const Catalog& surfaces) {
			const auto& entries = surfaces.Entries();
			return entries.empty() ? std::string()
								   : entries.front().Get("texture", entries.front().id);
		};
		for (const SurfaceBlock& b : kSurfaceBlocks)
			if (model == b.model) return firstOf(m_project.*b.surfaces);
		for (const CatalogEntry& e : m_project.surfacefeatures.Entries())
			if (e.Get("model", e.id) == model)
				return firstOf(e.Get("surface", "floor") == "ceiling" ? m_project.ceilings
																	  : m_project.floors);
		for (const CatalogEntry& e : m_project.wallfeatures.Entries())
			if (e.Get("model", e.id) == model) return firstOf(m_project.walls);
		for (const Catalog* cat : m_project.AllCatalogs())
			for (const CatalogEntry& e : cat->Entries()) {
				// A rune names no model: every rune is the shared tablet in its
				// own set (ItemKindFor's isRune path), so the tablet shows in the
				// first rune's.
				const std::string drawn =
					e.Get("model", e.Get("category", "") == "rune" ? "rune_tablet" : e.id);
				if (drawn == model) return e.Get("texture", e.id);
				const std::string part2 = e.Get("part2_model", "");
				if (!part2.empty() && part2 == model) return e.Get("part2_texture", "");
				// A fixture's bare bracket, its torch taken, wears the fixture's set;
				// an opener's mount (the chain's socket) the opener's.
				if (e.Get("empty_model", "") == model) return e.Get("texture", e.id);
				if (e.Find("style") && e.Get("mount", "") == model) return e.Get("texture", e.id);
			}
		return model;
	};
	// The first item (items / weapons / armor, catalog order) drawn with this
	// model that fills it with a liquid.
	m_assetPicker.liquidFor = [this](const std::string& model) -> const CatalogEntry* {
		for (const CatalogEntry* e : m_project.AllItems())
			if (e->Get("model", e->id) == model && e->Find("liquid_color")) return e;
		return nullptr;
	};
	// A part that only makes sense ON something: a door's TRIM (its straps, its
	// bosses) is shown on the leaf it is drawn with, an OPENER on the mount it
	// hangs from (doors.cat). "" = it stands alone.
	m_assetPicker.contextFor = [this](const std::string& model) {
		for (const CatalogEntry& e : m_project.doors.Entries()) {
			if (e.Get("trim", "") == model) return e.Get("model", e.id);
			if (e.Find("style") && e.Get("model", e.id) == model) return e.Get("mount", "");
		}
		return std::string();
	};
	// A feature's mount, from the catalog that stamps it (a ceiling feature is
	// left Free: its geometry rises into the vault, not behind a plane at 0).
	m_assetPicker.mountFor = [this](const std::string& model) {
		using Mount = DungeonWorld::PoolModelLook::Mount;
		for (const SurfaceBlock& b : kSurfaceBlocks)
			if (model == b.model) return b.mount;
		for (const CatalogEntry& e : m_project.surfacefeatures.Entries())
			if (e.Get("model", e.id) == model)
				return e.Get("surface", "floor") == "ceiling" ? Mount::Ceiling : Mount::Floor;
		for (const CatalogEntry& e : m_project.wallfeatures.Entries())
			if (e.Get("model", e.id) == model) return Mount::Wall;
		// A fixture hung ON a wall face - its model and its bare bracket.
		for (const CatalogEntry& e : m_project.fixtures.Entries())
			if (e.Get("mount", "") == "wall" &&
				(e.Get("model", e.id) == model || e.Get("empty_model", "") == model))
				return Mount::WallFixture;
		// A pit or stairwell replaces the floor block it opens (`hole = floor`),
		// as a floor feature does. A ceiling hole that is SCENERY (`hole =
		// ceiling`, `traverse = 0` - the pit's pair) is that well turned over; a
		// staircase up stands on the floor and is left Free.
		for (const CatalogEntry& e : m_project.stairs.Entries()) {
			if (e.Get("model", e.id) != model) continue;
			const std::string hole = e.Get("hole", "none");
			if (hole == "floor") return Mount::Floor;
			if (hole == "ceiling" && e.Get("traverse", "1") == "0") return Mount::CeilingWell;
		}
		return Mount::Free;
	};
	// A rig's idle: the first `anim_idle` clip of a monster drawn with it.
	m_assetPicker.idleClipFor = [this](const std::string& model) {
		for (const CatalogEntry& e : m_project.monsters.Entries()) {
			if (e.Get("model", e.id) != model) continue;
			const std::string idle = e.Get("anim_idle", "");
			const size_t end = idle.find_first_of(" \t,");
			if (!idle.empty()) return idle.substr(0, end);
		}
		return std::string();
	};
	// Provenance, for the details pane: what the editor's own imports recorded.
	m_assetPicker.sourceOf = [this](const std::string& name) {
		// Texture sets install resolution-tagged, and that is the key the
		// manifest uses (RecordImport); models keep their bare name.
		for (const std::string& key : {name + "_2k", name})
			if (const CatalogEntry* e = m_project.imports.Find(key))
				return e->Get("source", "");
		return std::string();
	};
	// Monsters keep their specialised dialog for animation + behaviour (it
	// REWRITES those rows, so the schema deliberately leaves them out); the type
	// editor's extra button is the way through to it.
	m_typeDialog.onExtra = [this](const TypeEditorDialog::Config& cfg) {
		if (cfg.catalogKey == "styles") {
			// SAVE TO LIBRARY takes the style as edited: its fields are written
			// to the world first, so what reaches the library is what the
			// dialog showed rather than the version before this edit.
			WriteTypeFields(cfg);
			std::vector<StyleLibrary::Copy> copied;
			SaveStyleToLibrary(cfg.id, copied);
			return;
		}
		OpenMonsterConfig(cfg.id);
	};
	// A library style's row adds it to the world.
	m_mapEditor.onAddStyle = [this](const std::string& id) { AddStyleFromLibrary(id); };
	// Rename / delete: the owner sweeps every level (and the cross-catalog
	// references) and refuses with a reason the dialog shows.
	m_typeDialog.onRename = [this](const std::string& id, const std::string& newId,
								   std::string& problem) {
		return RenameType(m_typeDialog.CatalogKey(), id, newId, problem);
	};
	m_typeDialog.onDelete = [this](const std::string& id, std::string& problem) {
		return DeleteType(m_typeDialog.CatalogKey(), id, problem);
	};
	// The typed confirmation's two questions — asked only where typedDelete is
	// set (a dungeon: OpenTypeEditor decides), so every other category keeps its
	// two-click delete and its sweep.
	m_typeDialog.canDelete = [this](const std::string& id) {
		return m_typeDialog.CatalogKey() == "dungeons" ? DungeonDeleteRefusal(id)
													   : std::string();
	};
	m_typeDialog.onDescribe = [this](const std::string& id) {
		return DescribeDungeon(id);
	};

	// Live-apply on every edit; persist on Save.
	m_monsterDialog.onApply = [this](const MonsterConfigDialog::Config& c) {
		m_world->ApplyMonsterAnimConfig(c.type, c.supported, c.clips);
		m_world->ApplyMonsterBehavior(c.type, c.archetype, c.keepRange, c.fleeBelow, c.spell,
									 c.threat);
	};
	m_monsterDialog.onSave = [this](const MonsterConfigDialog::Config& c) {
		m_world->ApplyMonsterAnimConfig(c.type, c.supported, c.clips);
		m_world->ApplyMonsterBehavior(c.type, c.archetype, c.keepRange, c.fleeBelow, c.spell,
									 c.threat);
		WriteMonsterAnim(c);
	};

	// Per-instance inspector: Select-click a placed monster → edit its .ent overrides.
	// A Way Out was just placed: ask where it leads (play-test #1, Michael's
	// pick: a dialog). It opens already pointing at the likeliest answer - the
	// doorway that lands on this level, else any doorway into this dungeon -
	// when it landed pointing nowhere, so Save accepts the obvious and Esc
	// keeps it too (the default is applied BEFORE the dialog snapshots).
	m_mapEditor.onExitPlaced = [this](int cx, int cz) {
		StairLink s;
		if (!m_world->StairSettings(cx, cz, s)) return;
		if (s.destLevel == "-" && m_worldMap) {
			const std::string& level = m_world->CurrentLevel();
			const CatalogEntry* dungeon = m_project.DungeonOfLevel(level);
			std::string pick;
			for (const WorldMap::Location& l : m_worldMap->Locations())
				if (l.level == level) {
					pick = l.id;
					break;
				}
			if (pick.empty() && dungeon)
				for (const WorldMap::Location& l : m_worldMap->Locations())
					if (l.Dungeon() == dungeon->id) {
						pick = l.id;
						break;
					}
			if (!pick.empty()) m_world->SetExitDest(cx, cz, pick);
		}
		m_inspectTargets.clear();
		m_inspectCellX = cx;
		m_inspectCellZ = cz;
		OpenInspectorFor(InspectTarget{InspectTarget::Kind::Stair});
		// For the harness: that the question was ASKED, and what it defaulted to.
		m_world->StairSettings(cx, cz, s);
		log::Info("way out at {},{}: stair inspector {}, leads to '{}'", cx, cz,
				  m_stairInspector.IsOpen() ? "open" : "NOT open", s.destLevel);
	};
	m_mapEditor.onInspect = [this](int cx, int cz) {
		// Gather EVERY inspectable object on the cell: stacked monsters, then wall
		// torches (each on its own wall). One target per object.
		m_inspectTargets.clear();
		m_inspectCellX = cx;
		m_inspectCellZ = cz;
		std::vector<std::string> labels;
		auto display = [](const CatalogEntry* e, const std::string& id) {
			return e ? e->Display() : id;
		};
		for (const auto& [id, type] : m_world->MonstersAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Monster};
			t.runtimeId = id;
			m_inspectTargets.push_back(t);
			labels.push_back(display(m_project.monsters.Find(type), type));
		}
		for (Direction wall : m_world->SconcesAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Sconce};
			t.wall = wall;
			m_inspectTargets.push_back(t);
			labels.push_back(loc::Format("map.fix.torchwall", loc::Tr(FacingLocKey(wall))));
		}
		if (m_world->BrazierAt(cx, cz)) {
			m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Brazier});
			labels.push_back(loc::Tr("map.key.brazier"));
		}
		{
			DungeonWorld::DoorEdit door; // presence check only
			if (m_world->DoorSettings(cx, cz, door)) {
				m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Door});
				labels.push_back(loc::Tr("map.key.door"));
			}
		}
		{
			DungeonWorld::ButtonEdit button; // presence check only
			if (m_world->ButtonSettings(cx, cz, button)) {
				m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Button});
				labels.push_back(loc::Tr("map.key.button"));
			}
		}
		{
			StairLink stair;
			if (m_world->StairSettings(cx, cz, stair)) {
				m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Stair});
				labels.push_back(display(m_project.stairs.Find(stair.type), stair.type));
			}
		}
		// Niche faces touching this cell — its own walls, or (clicking the wall
		// block) the niches carved into it from adjacent floor cells. One labeled
		// target per face, so a dead-end's several niches each pick individually.
		for (const DungeonWorld::NicheFace& f : m_world->NicheFacesAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Niche};
			t.nicheX = f.x;
			t.nicheZ = f.z;
			t.wall = f.wall;
			m_inspectTargets.push_back(t);
			labels.push_back(loc::Format("map.niche.atwall", loc::Tr(FacingLocKey(f.wall))));
		}
		for (const auto& [index, type] : m_world->DecorationsAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Decoration};
			t.handle = index;
			t.type = display(m_project.decorations.Find(type), type);
			m_inspectTargets.push_back(t);
			labels.push_back(t.type);
		}
		for (const auto& [id, type] : m_world->ItemsAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Item};
			t.handle = id;
			t.type = display(m_project.FindItem(type), type);
			m_inspectTargets.push_back(t);
			labels.push_back(t.type);
		}
		// In-flight projectiles passing through the cell (transient combat
		// content — freeze the world with the pause button to catch a fast one).
		for (const ProjectileInfo& p : m_world->ProjectilesAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Projectile};
			t.runtimeId = p.id;
			m_inspectTargets.push_back(t);
			labels.push_back(loc::Tr("map.proj.title"));
		}
		if (m_inspectTargets.empty()) return;
		if (m_inspectTargets.size() == 1) { // exactly one — skip the chooser
			OpenInspectorFor(m_inspectTargets.front());
			return;
		}
		m_inspectPicker.Open(loc::Format("map.pick.title", cx, cz), labels);
	};
	// Picking a row from the chooser opens that object's inspector.
	m_inspectPicker.onPick = [this](int i) {
		if (i >= 0 && i < static_cast<int>(m_inspectTargets.size()))
			OpenInspectorFor(m_inspectTargets[static_cast<size_t>(i)]);
	};
	// Patrol-route authoring: Edit hands the grid to the editor (route-laying mode);
	// each grid click appends a waypoint; Clear wipes the route.
	m_entityInspector.onEditRoute = [this](u32 id) {
		m_mapEditor.BeginRoute(id);
		if (m_world->onMessage) m_world->onMessage(loc::View("map.route.hint"));
	};
	m_entityInspector.onClearRoute = [this](u32 id) { m_world->ClearPatrol(id); };
	m_mapEditor.onRouteWaypoint = [this](u32 id, int cx, int cz) {
		m_world->AddPatrolWaypoint(id, cx, cz);
	};
	m_entityInspector.onApply = [this](const EntityInspector::Config& c) {
		m_world->ApplyMonsterInstance(c.runtimeId, c.asleep, c.leashRange, c.archetype,
									 c.keepRange, c.fleeBelow, c.spell, c.facing);
	};
	m_entityInspector.onSave = [this](const EntityInspector::Config& c) {
		m_world->ApplyMonsterInstance(c.runtimeId, c.asleep, c.leashRange, c.archetype,
									 c.keepRange, c.fleeBelow, c.spell, c.facing);
		if (!m_world->SaveLevel())
			log::Warn("entity inspector: failed to save level .ent");
		else if (m_world->onMessage)
			m_world->onMessage(loc::FormatLine("map.insp.saved", c.type));
	};

	// Torch (sconce) inspector: the Facing dropdown re-mounts it live, the body
	// edits its light/smoke settings live; Save persists.
	m_fixtureInspector.onRemount = [this](int x, int z, Direction from, Direction to) {
		return m_world->RemountSconce(x, z, from, to);
	};
	m_fixtureInspector.onSettings = [this](int x, int z, Direction wall, bool brazier, bool lit,
										   float brightness, float turbidity,
										   const Vec3& flameColor) {
		if (brazier) m_world->SetBrazierSettings(x, z, lit, brightness, turbidity, flameColor);
		else m_world->SetTorchSettings(x, z, wall, lit, brightness, turbidity, flameColor);
		// The dialog's preview flame takes the colour too.
		m_previewFire.SetFlameColor(flameColor, HasFlameColor(flameColor));
		// (the dialog flips its own preview spec's showFire on the Lit toggle)
	};
	m_fixtureInspector.onSave = [this] {
		if (!m_world->SaveLevel()) log::Warn("fixture inspector: failed to save level");
	};

	// Door inspector: Open flips the record's authored state, and the live panel
	// follows when it can - never shutting a wrecked leaf, and a close with
	// anyone in the doorway refused outright (SetDoorSettings, code-review C356),
	// which is why the box reads back what was kept; the key
	// dropdown authors the key= param (locks the party's click), and the
	// opener rows author opener=/opener_side= (which re-resolve the live door's
	// hand-hold — see SetDoorSettings).
	m_doorInspector.onApply = [this](DoorInspector::Config& c) {
		DungeonWorld::DoorEdit e;
		e.open = c.open;
		e.key = c.key;
		e.flag = c.flag;
		e.name = c.name;
		e.opener = c.opener;
		e.openerSide = c.openerSide;
		e.easeIn = c.easeIn;
		e.easeOut = c.easeOut;
		e.openerEaseIn = c.openerEaseIn;
		e.openerEaseOut = c.openerEaseOut;
		// The slider carries the EFFECTIVE seconds, so an override is authored
		// only where it DIFFERS from the type's — which is what keeps a door
		// nobody touched inheriting, and keeps the record minimal like every
		// other field here. Half a hundredth, because SetDoorSettings writes two
		// decimals and anything finer could not survive the round trip anyway.
		e.seconds = std::fabs(c.seconds - c.typeSeconds) < 0.005f ? 0.0f : c.seconds;
		m_world->SetDoorSettings(c.x, c.z, e);
		DungeonWorld::DoorEdit kept;
		if (m_world->DoorSettings(c.x, c.z, kept)) c.open = kept.open;
	};
	m_doorInspector.onSave = [this] {
		if (!m_world->SaveLevel()) log::Warn("door inspector: failed to save level");
	};

	// Button inspector: the Target dropdown wires the lever to a door name; the
	// flag rows say what it waits on and what a press does to a flag.
	m_buttonInspector.onApply = [this](const ButtonInspector::Config& c) {
		DungeonWorld::ButtonEdit e;
		e.target = c.target;
		e.needs = c.needs;
		e.sets = c.sets;
		e.op = c.op;
		m_world->SetButtonSettings(c.x, c.z, e);
	};
	m_buttonInspector.onSave = [this] {
		if (!m_world->SaveLevel()) log::Warn("button inspector: failed to save level");
	};

	// Niche inspector: apply the shape/secret/name live, then persist (a niche is
	// STATIC .map data, so Save writes the map layer, not the .ent).
	m_nicheInspector.onApply = [this](const NicheInspector::Config& c) {
		m_world->SetNichePropsAt(c.x, c.z, c.wall, c.name, c.hidden, c.type);
	};
	// The Face dropdown moves it to another wall of the same cell, treasure and all.
	m_nicheInspector.onRemount = [this](int x, int z, Direction from, Direction to) {
		return m_world->RemountNiche(x, z, from, to);
	};
	m_nicheInspector.onSave = [this] {
		if (m_world->SaveAllLevels().empty())
			log::Warn("niche inspector: failed to save map");
	};

	// Stair inspector: turn the stair live, persist the map (a stair is static
	// .map data, like a niche), or go to the far end.
	m_stairInspector.onApply = [this](const StairInspector::Config& c) {
		m_world->SetStairFacing(c.x, c.z, c.facing);
		m_world->SetStairFlag(c.x, c.z, c.flag);
		if (!c.destIsLevel) m_world->SetExitDest(c.x, c.z, c.dest);
	};
	m_stairInspector.onSave = [this] {
		if (m_world->SaveAllLevels().empty()) log::Warn("stair inspector: failed to save map");
	};
	m_stairInspector.onGoTo = [this](const StairInspector::Config& c) {
		m_mapView.SetViewLevel(c.dest);
		m_mapEditor.SelectCell(c.destX, c.destZ);
	};
	// A palette row's link (a quest item's placement): the same trip.
	m_mapEditor.onGoTo = [this](const std::string& level, int cx, int cz) {
		m_mapView.SetViewLevel(level);
		m_mapEditor.SelectCell(cx, cz);
	};

	// Item/decoration inspector: apply the facing edit to the right live object.
	m_propInspector.onApply = [this](const PropInspector::Config& c) {
		if (c.kind == PropInspector::Config::Kind::Decoration)
			m_world->SetDecorationFacing(c.handle, c.facing);
		else
			m_world->SetItemFacing(c.handle, c.facing);
	};
	m_propInspector.onSave = [this] {
		if (!m_world->SaveLevel()) log::Warn("prop inspector: failed to save level");
	};
}


} // namespace dungeon::game
