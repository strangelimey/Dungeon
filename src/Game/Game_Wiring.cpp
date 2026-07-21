// ============================================================================
// Game/Game_Wiring.cpp — split out of Game.cpp to keep files small (see Game.h).
// Module callback wiring (the on* handlers), called from the ctor.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Graphics/DisplayEnum.h"
#include "Graphics/Texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace dungeon::game {
void Game::WireModuleCallbacks() {
	// Wire the modules together: world feedback goes to the HUD log, UI
	// actions drive the state machine.
	m_world.onMessage = [this](const std::string& line) { m_ui.AddLogLine(line); };
	// Lines about a specific member arrive with their identity color; the log
	// tints them so each character's doings read at a glance.
	m_world.onMemberMessage = [this](const std::string& line, const Vec4& color) {
		m_ui.AddLogLine(line, color);
	};
	// The party fell: end the run back at the title (Start New Game resets the
	// roster + monsters in place).
	m_world.onPartyWipe = [this] {
		m_state = AppState::Menu;
		m_ui.ResetToMainPage();
	};
	m_ui.onStartNewGame = [this] {
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
	m_ui.onQuit = [this] { m_quitRequested = true; };
	m_ui.onResume = [this] { m_state = AppState::Playing; };
	m_ui.onLoadSave = [this](const std::string& path) {
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
	m_ui.onSaveSlot = [this](const std::string& name) {
		SaveGame(name);
		m_state = AppState::Playing; // resume play after saving from the pause menu
	};
	m_ui.onOpenSheet = [this](size_t index) { OpenCharacterSheet(index); };
	// Sheet "All" button: leave the sheet and bring up the combined party
	// backpacks (over the live world) for cross-character item swaps.
	m_ui.onShowPartyInventory = [this] {
		m_state = AppState::Playing;
		m_ui.OpenInventory();
	};
	m_ui.onQualitySelected = [this](int index) {
		SetQuality(static_cast<Quality>(index));
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
	m_ui.onTorchPalette = [this](int index) { m_world.SetTorchPalette(index); };
	m_ui.onMoveAction = [this](MoveAction action) {
		// A pit fall swallows movement (the keyboard path gates in
		// DungeonWorld::Update; this is the HUD arrow-button path).
		if (!m_world.Falling()) m_world.GetParty().Act(action);
	};
	m_ui.onHandAttack = [this](size_t member, size_t hand, const std::string& verb) {
		m_world.PartyAttack(member, hand, verb);
	};
	// The hand right-click menu reads an item's commands from the world's item
	// kinds (single source — ItemKindFor parses category/command + rune defaults).
	m_ui.itemCommands = [this](const std::string& id) {
		return m_world.ItemCommands(id);
	};
	// The hand menu's Magic group enumerates the recipe table (filtered by the
	// member's vocabulary in GameUI); a picked "cast:<id>" default casts through
	// the world's façade — the same vocab/mana gates as the dev `cast` command.
	m_ui.spellDefs = [this] { return m_world.SpellDefs(); };
	m_ui.onCastSpell = [this](size_t member, const std::string& id, size_t hand) {
		m_world.CastSpellById(member, id, static_cast<int>(hand));
	};
	// The spellbook panel casts a HAND-BUILT symbol sequence: an exact recipe
	// match casts (vocab/mana gated), anything else fizzles with its log line.
	// The book is member-driven (its selector row); `hand` arrives as
	// kBookHands so the cast credits both hands' quick-cast MRU.
	m_ui.onCastSequence = [this](size_t member, size_t hand,
								 const std::vector<SpellSymbol>& seq) {
		m_world.CastSpell(member, seq, static_cast<int>(hand));
	};
	m_ui.onKeysChanged = [this] {
		m_world.GetParty().SetKeys(m_settings.moveKeys);
	};
	m_ui.onLookChanged = [this] {
		m_world.GetParty().SetLook(m_settings.look);
	};
	m_ui.onHeadBobChanged = [this] {
		m_world.GetParty().SetHeadBob(m_settings.headBob);
	};
	// Recorded only — the rebuild would destroy the dropdown mid-callback;
	// Update applies it first thing next frame.
	m_ui.onLanguageSelected = [this](const std::string& code) {
		m_pendingLanguage = code;
	};

	// Editor: a palette "+ New" opens the asset-creation dialog for that category
	// (Walls/Floors/Ceilings import a texture folder; the rest import a model).
	m_mapEditor.onNewAsset = [this](MapEditor::PaletteCat cat) {
		const char* key = MapEditor::CategoryCatalogKey(cat);
		if (!*key) return; // belt-and-braces: every category names a catalog
		m_assetDialog.Open(loc::Tr(MapEditor::CategoryNameKey(cat)), key,
						   MapEditor::CategoryTextureSet(cat), m_settings.theme);
	};
	// Create runs AssetBaker on the picked source (P4c); the dialog stays open in
	// a "baking…" state until Update sees the subprocess finish.
	m_assetDialog.onCreate = [this](const AssetDialog::CreateRequest& req) {
		if (req.name.empty() || req.sourcePath.empty()) {
			log::Warn("asset create: need a name and a source");
			return;
		}
		m_bakeReq = req;
		m_bakeStep = 0;
		if (StartBakeStep()) {
			m_baking = true;
			m_assetDialog.SetBusy(true);
		} else {
			log::Warn("asset create: could not launch AssetBaker");
		}
	};

	// Right-click a monster in the palette → open its animation config dialog.
	m_mapEditor.onConfigure = [this](MapEditor::PaletteCat cat, const std::string& id) {
		// Surfaces (walls/floors/ceilings): open the geometry-style dialog. Columns
		// is a wall-block feature, so only walls show that knob.
		if (cat == MapEditor::PaletteCat::Walls || cat == MapEditor::PaletteCat::Floors ||
			cat == MapEditor::PaletteCat::Ceilings) {
			const std::string key = MapEditor::CategoryCatalogKey(cat);
			const Catalog* catlg = m_project.CatalogForKey(key);
			const CatalogEntry* e = catlg ? catlg->Find(id) : nullptr;
			m_wallStyleDialog.Open(id, key, e ? e->Display() : id,
								   CatalogGet(e, "texture", id),
								   cat == MapEditor::PaletteCat::Walls,
								   e ? e->GetFloat("wear", 1.0f) : 1.0f,
								   e ? e->GetBool("columns", true) : true);
			return;
		}
		if (cat != MapEditor::PaletteCat::Monsters) return;
		// Guard the force-load: a catalog id whose <model>.gltf is missing would
		// abort in LoadModelOrDie. Warn and skip instead of crashing the editor.
		if (!m_world.MonsterModelAvailable(id)) {
			log::Warn("monster config: '{}' has no loadable model — skipped", id);
			return;
		}
		const CatalogEntry* e = m_project.monsters.Find(id);
		const std::string display = e ? e->Display() : id;
		DungeonWorld::AnimSupport supported;
		DungeonWorld::AnimClips clips;
		m_world.MonsterAnimConfig(id, supported, clips);
		ai::Archetype archetype;
		float keepRange, fleeBelow;
		std::string spell;
		ThreatTuning threat;
		m_world.MonsterBehaviorConfig(id, archetype, keepRange, fleeBelow, spell, threat);
		m_monsterDialog.Open(id, display, supported, clips, archetype, keepRange, fleeBelow,
							 spell, threat, m_world.MonsterClipNames(id), m_world.SpellIds());
		m_previewType.clear(); // force the preview animator to (re)build on first frame
		m_previewClip.clear();
		m_previewMonMesh = nullptr;
		m_previewMonSubs.clear();
	};
	// Live-apply on every edit; persist on Save.
	m_monsterDialog.onApply = [this](const MonsterConfigDialog::Config& c) {
		m_world.ApplyMonsterAnimConfig(c.type, c.supported, c.clips);
		m_world.ApplyMonsterBehavior(c.type, c.archetype, c.keepRange, c.fleeBelow, c.spell,
									 c.threat);
	};
	m_monsterDialog.onSave = [this](const MonsterConfigDialog::Config& c) {
		m_world.ApplyMonsterAnimConfig(c.type, c.supported, c.clips);
		m_world.ApplyMonsterBehavior(c.type, c.archetype, c.keepRange, c.fleeBelow, c.spell,
									 c.threat);
		WriteMonsterAnim(c);
	};

	// Surface Style Save: persist the type's wear/columns, then rebake + reload
	// the worn mesh (geometry is baked, so no live preview). The async bake shows
	// a "baking…" notice on the dialog until the Update poll closes it.
	m_wallStyleDialog.onSave = [this](const std::string& id, const std::string& catalogKey,
									  const std::string& texture, float wear, bool columns) {
		WriteWallStyle(catalogKey, id, wear, columns);
		StartRestyleBake(catalogKey, texture, wear, columns);
		if (m_restyleBake) m_wallStyleDialog.SetBusy(true); // bake launched
		else m_wallStyleDialog.Close();                     // failed to launch
	};

	// Per-instance inspector: Select-click a placed monster → edit its .ent overrides.
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
		for (const auto& [id, type] : m_world.MonstersAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Monster};
			t.runtimeId = id;
			m_inspectTargets.push_back(t);
			labels.push_back(display(m_project.monsters.Find(type), type));
		}
		for (Direction wall : m_world.SconcesAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Sconce};
			t.wall = wall;
			m_inspectTargets.push_back(t);
			labels.push_back(loc::Format("map.fix.torchwall", loc::Tr(FacingLocKey(wall))));
		}
		if (m_world.BrazierAt(cx, cz)) {
			m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Brazier});
			labels.push_back(loc::Tr("map.key.brazier"));
		}
		{
			bool open;
			std::string key, name;
			if (m_world.DoorSettings(cx, cz, open, key, name)) {
				m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Door});
				labels.push_back(loc::Tr("map.key.door"));
			}
		}
		{
			std::string target;
			if (m_world.ButtonSettings(cx, cz, target)) {
				m_inspectTargets.push_back(InspectTarget{InspectTarget::Kind::Button});
				labels.push_back(loc::Tr("map.key.button"));
			}
		}
		// Niche faces touching this cell — its own walls, or (clicking the wall
		// block) the niches carved into it from adjacent floor cells. One labeled
		// target per face, so a dead-end's several niches each pick individually.
		for (const DungeonWorld::NicheFace& f : m_world.NicheFacesAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Niche};
			t.nicheX = f.x;
			t.nicheZ = f.z;
			t.wall = f.wall;
			m_inspectTargets.push_back(t);
			labels.push_back(loc::Format("map.niche.atwall", loc::Tr(FacingLocKey(f.wall))));
		}
		for (const auto& [index, type] : m_world.DecorationsAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Decoration};
			t.handle = index;
			t.type = display(m_project.decorations.Find(type), type);
			m_inspectTargets.push_back(t);
			labels.push_back(t.type);
		}
		for (const auto& [id, type] : m_world.ItemsAt(cx, cz)) {
			InspectTarget t{InspectTarget::Kind::Item};
			t.handle = id;
			t.type = display(m_project.items.Find(type), type);
			m_inspectTargets.push_back(t);
			labels.push_back(t.type);
		}
		// In-flight projectiles passing through the cell (transient combat
		// content — freeze the world with the pause button to catch a fast one).
		for (const ProjectileInfo& p : m_world.ProjectilesAt(cx, cz)) {
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
		if (m_world.onMessage) m_world.onMessage(loc::Tr("map.route.hint"));
	};
	m_entityInspector.onClearRoute = [this](u32 id) { m_world.ClearPatrol(id); };
	m_mapEditor.onRouteWaypoint = [this](u32 id, int cx, int cz) {
		m_world.AddPatrolWaypoint(id, cx, cz);
	};
	m_entityInspector.onApply = [this](const EntityInspector::Config& c) {
		m_world.ApplyMonsterInstance(c.runtimeId, c.asleep, c.leashRange, c.archetype,
									 c.keepRange, c.fleeBelow, c.spell, c.facing);
	};
	m_entityInspector.onSave = [this](const EntityInspector::Config& c) {
		m_world.ApplyMonsterInstance(c.runtimeId, c.asleep, c.leashRange, c.archetype,
									 c.keepRange, c.fleeBelow, c.spell, c.facing);
		if (!m_world.SaveLevel())
			log::Warn("entity inspector: failed to save level .ent");
		else if (m_world.onMessage)
			m_world.onMessage(loc::Format("map.insp.saved", c.type));
	};

	// Torch (sconce) inspector: the Facing dropdown re-mounts it live, the body
	// edits its light/smoke settings live; Save persists.
	m_fixtureInspector.onRemount = [this](int x, int z, Direction from, Direction to) {
		return m_world.RemountSconce(x, z, from, to);
	};
	m_fixtureInspector.onSettings = [this](int x, int z, Direction wall, bool brazier, bool lit,
										   float brightness, float turbidity) {
		if (brazier) m_world.SetBrazierSettings(x, z, lit, brightness, turbidity);
		else m_world.SetTorchSettings(x, z, wall, lit, brightness, turbidity);
		// (the dialog flips its own preview spec's showFire on the Lit toggle)
	};
	m_fixtureInspector.onSave = [this] {
		if (!m_world.SaveLevel()) log::Warn("fixture inspector: failed to save level");
	};

	// Door inspector: Open flips the live panel + the record's authored state;
	// the key dropdown authors the key= param (locks the party's click).
	m_doorInspector.onApply = [this](const DoorInspector::Config& c) {
		m_world.SetDoorSettings(c.x, c.z, c.open, c.key, c.name);
	};
	m_doorInspector.onSave = [this] {
		if (!m_world.SaveLevel()) log::Warn("door inspector: failed to save level");
	};

	// Button inspector: the Target dropdown wires the lever to a door name.
	m_buttonInspector.onApply = [this](const ButtonInspector::Config& c) {
		m_world.SetButtonSettings(c.x, c.z, c.target);
	};
	m_buttonInspector.onSave = [this] {
		if (!m_world.SaveLevel()) log::Warn("button inspector: failed to save level");
	};

	// Niche inspector: apply the shape/secret/name live, then persist (a niche is
	// STATIC .map data, so Save writes the map layer, not the .ent).
	m_nicheInspector.onApply = [this](const NicheInspector::Config& c) {
		m_world.SetNichePropsAt(c.x, c.z, c.wall, c.name, c.hidden, c.type);
	};
	m_nicheInspector.onSave = [this] {
		if (m_world.SaveAllLevels().empty())
			log::Warn("niche inspector: failed to save map");
	};

	// Item/decoration inspector: apply the facing edit to the right live object.
	m_propInspector.onApply = [this](const PropInspector::Config& c) {
		if (c.kind == PropInspector::Config::Kind::Decoration)
			m_world.SetDecorationFacing(c.handle, c.facing);
		else
			m_world.SetItemFacing(c.handle, c.facing);
	};
	m_propInspector.onSave = [this] {
		if (!m_world.SaveLevel()) log::Warn("prop inspector: failed to save level");
	};
}


} // namespace dungeon::game
