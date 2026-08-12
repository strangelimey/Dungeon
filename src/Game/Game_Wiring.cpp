// ============================================================================
// Game/Game_Wiring.cpp — split out of Game.cpp to keep files small (see Game.h).
// Module callback wiring (the on* handlers), called from the ctor.
// ============================================================================
#include "Game/Game.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/AssetUtil.h"

#include <cmath> // fabs — "is this slider still on the type's value?"
#include <string>
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
	m_ui.onTorchPalette = [this](int index) { m_world.SetTorchPalette(index); };
	// The sheet's defense breakdown: only the world can resolve worn items,
	// balance knobs and the live evasion formula.
	m_ui.defenseFor = [this](const Character& c) { return m_world.DefenseFor(c); };
	m_ui.defenseWith = [this](const Character& c, const std::string& id) {
		return m_world.DefenseWith(c, id);
	};
	// The stance slider under a member's hands (docs/damage-system.md). Clamped
	// to 0..1 HERE rather than in the widget: the model deliberately allows
	// MORE than 1 (over-exertion), and a drag is simply not the way to reach
	// it — spending past everything you have should cost something, and that
	// mechanism is still to be designed. The dev `guard` command has no clamp.
	m_ui.onGuardChange = [this](size_t member, float share) {
		if (member < m_characters.size())
			m_characters[member].offenseShare = std::clamp(share, 0.0f, 1.0f);
	};
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
	// Create runs AssetBaker on the picked source (P4c); the dialog stays open in
	// a "baking…" state until Update sees the subprocess finish.
	m_assetDialog.onCreate = [this](const AssetDialog::CreateRequest& req) {
		// Installed / Duplicate bind an asset that is already baked, so the type
		// exists the moment its catalog entry does — no subprocess, no wait. The
		// one exception: a pool TEXTURE set adopted as a surface may never have
		// been used as one, so its worn block mesh still has to be baked (the
		// import path's second step, entered directly).
		const bool needsWornBake =
			req.source == AssetDialog::Source::Installed && req.textureSet;
		if (!req.NeedsBake() && !needsWornBake) {
			CreateCatalogEntry(req);
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
			for (const DamageTypeBook::Entry& e : m_world.DamageTypes().Entries())
				ids.push_back(e.id);
			return ids;
		}
		case FieldKind::CatalogRef: {
			std::vector<std::string> ids;
			if (const Catalog* c = m_project.CatalogForKey(spec.options))
				for (const CatalogEntry& e : c->Entries()) ids.push_back(e.id);
			return ids;
		}
		default: return {};
		}
	};
	// Save: merge the touched fields into the catalog, then apply. A surface
	// whose look changed needs its worn meshes re-baked before it shows.
	m_typeDialog.onSave = [this](const TypeEditorDialog::Config& cfg) {
		WriteTypeFields(cfg);
		if (!cfg.rebake) {
			// Nothing BAKED is stale, so the change can just take effect. A
			// surface's per-draw knobs (parallax depth, metallic/roughness) push
			// straight at the live scene; a prop's are baked into its cached
			// KIND at load, so that kind is dropped and its instances re-spawned.
			if (MapEditor::SurfaceCat(MapEditor::CatForCatalogKey(cfg.catalogKey)))
				m_world.RefreshSurfaceMaterials();
			else
				m_world.ReloadTypeKind(cfg.catalogKey, cfg.id);
			if (m_world.onMessage)
				m_world.onMessage(loc::Format("map.type.saved", cfg.id));
			return;
		}
		const CatalogEntry* e = m_project.CatalogForKey(cfg.catalogKey)
									? m_project.CatalogForKey(cfg.catalogKey)->Find(cfg.id)
									: nullptr;
		// An unset `relief` passes -1, leaving the baker's per-kind amplitude —
		// the depth every surface was baked at before the field existed.
		StartRestyleBake(cfg.catalogKey, CatalogGet(e, "texture", cfg.id),
						 e ? e->GetFloat("wear", 1.0f) : 1.0f,
						 e ? e->GetFloat("relief", -1.0f) : -1.0f);
		if (m_restyleBake) m_typeDialog.SetBusy(true); // bake launched
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
		OpenMonsterConfig(cfg.id);
	};
	// Rename / delete: the owner sweeps every level (and the cross-catalog
	// references) and refuses with a reason the dialog shows.
	m_typeDialog.onRename = [this](const std::string& id, const std::string& newId,
								   std::string& problem) {
		return RenameType(m_typeDialog.CatalogKey(), id, newId, problem);
	};
	m_typeDialog.onDelete = [this](const std::string& id, std::string& problem) {
		return DeleteType(m_typeDialog.CatalogKey(), id, problem);
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
			DungeonWorld::DoorEdit door; // presence check only
			if (m_world.DoorSettings(cx, cz, door)) {
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
			t.type = display(m_project.FindItem(type), type);
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
	// the key dropdown authors the key= param (locks the party's click), and the
	// opener rows author opener=/opener_side= (which re-resolve the live door's
	// hand-hold — see SetDoorSettings).
	m_doorInspector.onApply = [this](const DoorInspector::Config& c) {
		DungeonWorld::DoorEdit e;
		e.open = c.open;
		e.key = c.key;
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
		m_world.SetDoorSettings(c.x, c.z, e);
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
	// The Face dropdown moves it to another wall of the same cell, treasure and all.
	m_nicheInspector.onRemount = [this](int x, int z, Direction from, Direction to) {
		return m_world.RemountNiche(x, z, from, to);
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
