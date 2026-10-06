// ============================================================================
// Game/Game_Inspect.cpp — split out of Game.cpp to keep files small (see Game.h).
// The editor Select-tool inspector dispatch, and the harness's hands for the
// inspectors, the patrol route and the editor's other dialogs.
// ============================================================================
#include "Game/Game.h"

#include "Core/Loc.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace dungeon::game {
std::array<InstanceInspector*, 7> Game::InstanceInspectors() {
	return {&m_entityInspector, &m_fixtureInspector, &m_propInspector,
			&m_doorInspector,   &m_buttonInspector,  &m_nicheInspector,
			&m_stairInspector};
}

FlagChoices Game::FlagChoiceList() const {
	// Each flag labelled with its SCOPE, so picking "sealed" in the crypt does
	// not quietly pick the barrow's flag of the same look.
	FlagChoices out;
	for (const CatalogEntry& e : m_project.flags.Entries()) {
		const std::string d = e.Get("dungeon", "");
		const CatalogEntry* dungeon = d.empty() ? nullptr : m_project.dungeons.Find(d);
		out.emplace_back(e.id, loc::Format("map.flag.label", e.Display(),
										   d.empty() ? loc::Tr("map.flag.world")
													 : dungeon ? dungeon->Display() : d));
	}
	return out;
}

InstanceInspector* Game::ActiveInstanceInspector() {
	for (InstanceInspector* ii : InstanceInspectors())
		if (ii->IsOpen()) return ii;
	return nullptr;
}

void Game::CloseInspectors() {
	for (InstanceInspector* ii : InstanceInspectors()) ii->Close();
	m_projectileInspector.Close();
	m_inspectPicker.Close();
	m_inspectTargets.clear();
}

void Game::ForgetMonsterPreview() {
	m_previewType.clear();
	m_previewClip.clear();
	m_previewMonMesh = nullptr;
	m_previewMonSubs.clear();
}

bool Game::RouteKeyPressed(RouteKey key) {
	if (!m_mapView.IsOpen() || m_mapView.CurrentMode() != MapView::Mode::Editor ||
		!m_mapEditor.LayingRoute())
		return false;
	const u32 id = m_mapEditor.RouteId();
	if (key == RouteKey::Back) {
		m_world->RemoveLastPatrolWaypoint(id);
		return true;
	}
	m_mapEditor.EndRoute();
	// Back to the inspector, built afresh from the live monster: its waypoints
	// as they now are, and a preview of the kind as it now is.
	InspectTarget t{InspectTarget::Kind::Monster};
	t.runtimeId = id;
	OpenInspectorFor(t);
	return true;
}

// --- the harness's hands (`editor inspector`, `editor route`) ----------------

void Game::InspectorCommand(const std::vector<std::string>& args) {
	// args[0] is "inspector"; the verb, if any, follows.
	const std::string verb = args.size() >= 2 ? args[1] : std::string("status");
	InstanceInspector* open = ActiveInstanceInspector();
	const bool monster = open == &m_entityInspector;
	// A press on the dialog's n-th drop-down (opened by its next Update), or
	// bare, whether one is open - what its Esc asks first (code-review C81).
	if (verb == "popup") {
		if (!open) {
			m_console.Print("editor inspector popup: no inspector is open");
		} else if (args.size() >= 3) {
			open->OpenPopup(std::atoi(args[2].c_str()));
			m_console.Print(std::format("editor inspector popup: pressing #{}", args[2]));
		} else {
			m_console.Print(std::format("editor inspector popup: {}",
										open->PopupOpen() ? "open" : "shut"));
		}
		return;
	}
	if (verb != "status") {
		if (!open) {
			// Not a refusal: "nothing to press" is often the answer a script is
			// checking for (a route that ended, a world that changed).
			m_console.Print(std::format("editor inspector {}: no inspector is open", verb));
			return;
		}
		if (verb == "esc") {
			open->Cancel();
		} else if (verb == "save") {
			open->ClickSave();
		} else if (verb == "delete") {
			// The footer's Delete: exactly the inspected object, the targeted
			// removal the erase ladder is not (code-review C355's judge).
			if (!open->ClickDelete()) {
				m_console.Refuse("editor inspector delete: this inspector offers no Delete");
				return;
			}
		} else if (verb == "tab" && args.size() >= 3) {
			open->SelectTab(std::atoi(args[2].c_str()));
		} else if (monster && verb == "editroute") {
			m_entityInspector.ClickEditRoute();
		} else if (monster && verb == "clearroute") {
			m_entityInspector.ClickClearRoute();
		} else if (open == &m_doorInspector && verb == "open" && args.size() >= 3 &&
				   (args[2] == "on" || args[2] == "off")) {
			m_doorInspector.ClickOpen(args[2] == "on"); // the Open checkbox
		} else if (monster && verb == "archetype" && args.size() >= 3) {
			int found = -1;
			for (int i = 0; i < static_cast<int>(std::size(ai::kArchetypeNames)); ++i)
				if (args[2] == ai::kArchetypeNames[i]) found = i;
			if (found < 0) {
				m_console.Refuse(std::format("editor inspector: no archetype '{}'", args[2]));
				return;
			}
			m_entityInspector.PickArchetype(static_cast<ai::Archetype>(found));
		} else if (monster && verb == "leash" && args.size() >= 3) {
			// The Leash slider, set to a value: squares from the anchor.
			char* end = nullptr;
			const float range = std::strtof(args[2].c_str(), &end);
			if (end == args[2].c_str() || *end != '\0' || !(range >= 0.0f) ||
				range > EntityInspector::kLeashMax) {
				m_console.Refuse(std::format("editor inspector: leash '{}' is not 0..{:g}", args[2],
											 EntityInspector::kLeashMax));
				return;
			}
			m_entityInspector.SetLeash(range);
		} else {
			m_console.RefuseUsage();
			return;
		}
		// A click queued its rebuild for the next Update, which does not run
		// while the console is up: do it now, so what is read next is the view
		// the click produced.
		open->ApplyPending();
		open = ActiveInstanceInspector();
	}
	if (!open) {
		m_console.Print("editor inspector: closed");
		return;
	}
	if (open == &m_doorInspector) {
		// The door's: its Open checkbox (the AUTHORED state, what Save writes)
		// beside the leaf as it stands in play and whether it is wrecked - the
		// two an Open that did not take sets apart (code-review C356).
		const DoorInspector::Config& c = m_doorInspector.Current();
		DungeonWorld::DoorEdit door;
		const bool here = m_world->DoorSettings(c.x, c.z, door);
		m_console.Print(std::format(
			"editor inspector: door {},{} open={} live={} broken={} tab {}", c.x, c.z,
			c.open ? 1 : 0, here ? (door.live ? "1" : "0") : "-",
			here ? (door.broken ? "1" : "0") : "-", open->ActiveTab()));
		return;
	}
	if (open != &m_entityInspector) {
		m_console.Print(std::format("editor inspector: open (not a monster) tab {}",
									open->ActiveTab()));
		return;
	}
	// The monster's: which one (and whether it still lives), the tab, the
	// waypoint count the Patrol tab shows, and the behaviour - with the spell
	// the monster OPENED with (what a load gave it; the working copy may have
	// defaulted it), the one Save would write and the one the Caster row SHOWS.
	const EntityInspector::Config& c = m_entityInspector.Current();
	const int arch = static_cast<int>(c.archetype);
	m_console.Print(std::format(
		"editor inspector: monster {} {} {} tab {} waypoints {} archetype {} opened '{}' "
		"spell '{}' shown '{}'",
		c.runtimeId, c.type, m_world->MonsterPatrol(c.runtimeId) ? "live" : "gone",
		m_entityInspector.ActiveTab(), c.patrolCount,
		arch >= 0 && arch < static_cast<int>(std::size(ai::kArchetypeNames))
			? ai::kArchetypeNames[arch]
			: "?",
		m_entityInspector.Opened().spell, c.spell, m_entityInspector.ShownSpell()));
}

bool Game::PickInspectTarget(const std::string& kind) {
	using K = InspectTarget::Kind;
	static constexpr std::pair<const char*, K> kNames[] = {
		{"monster", K::Monster},	   {"sconce", K::Sconce},	{"brazier", K::Brazier},
		{"door", K::Door},			   {"button", K::Button},	{"decoration", K::Decoration},
		{"item", K::Item},			   {"niche", K::Niche},		{"stair", K::Stair},
		{"projectile", K::Projectile}};
	const auto named = std::find_if(std::begin(kNames), std::end(kNames),
									[&](const auto& n) { return kind == n.first; });
	if (named == std::end(kNames)) return false;
	for (size_t i = 0; i < m_inspectTargets.size(); ++i)
		if (m_inspectTargets[i].kind == named->second) {
			// What the chooser's row does - the picker closes as a click on it would.
			m_inspectPicker.Close();
			if (m_inspectPicker.onPick) m_inspectPicker.onPick(static_cast<int>(i));
			return true;
		}
	return false;
}

void Game::RouteCommand(const std::vector<std::string>& args) {
	// args[0] is "route".
	if (args.size() >= 3 && args[1] == "key") {
		// A key as the map's Update hears it: the route's only on the editor map.
		const bool back = args[2] == "back";
		if (!back && args[2] != "enter" && args[2] != "esc") {
			m_console.RefuseUsage();
			return;
		}
		const bool taken = RouteKeyPressed(back ? RouteKey::Back : RouteKey::Finish);
		m_console.Print(std::format("editor route key {}: {}", args[2],
									taken ? "taken" : "not the route's"));
	} else if (args.size() >= 3) {
		// A grid click while laying: the left press MapView hands the editor.
		if (!m_mapEditor.LayingRoute()) {
			m_console.Print("editor route: no route is being laid");
			return;
		}
		m_mapEditor.Paint(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()),
						  /*dragging*/ false);
	} else if (args.size() >= 2 && args[1] != "status") {
		m_console.RefuseUsage();
		return;
	}
	// What the EDITOR believes, not what the world can find: a route left
	// naming a monster that is gone must read as laid, or this could not see it.
	if (!m_mapEditor.LayingRoute()) {
		m_console.Print("editor route: none");
		return;
	}
	const u32 id = m_mapEditor.RouteId();
	const auto* route = m_world->MonsterPatrol(id);
	m_console.Print(std::format("editor route: laying monster {} ({} waypoint(s))", id,
								route ? route->size() : 0));
}

void Game::EditorDialogCommand(const std::vector<std::string>& args) {
	// args[0] names the control; its verb, if any, follows. Each ends by saying
	// where the control stands, read between `presskey esc` lines by a check of
	// the Esc ladders (code-review C81, tools/EvalScripts/dialogesc.eval).
	const std::string& what = args[0];
	const std::string verb = args.size() >= 2 ? args[1] : std::string("status");
	const auto popupWord = [](bool open) { return open ? "open" : "shut"; };
	// Up as the editor, as each of these is reached - WITHOUT a mode flip when
	// it already is one, since SetMode closes the level drop-down.
	const auto toEditor = [this] {
		if (!m_mapView.IsOpen()) m_mapView.Open(MapView::Mode::Editor);
		else if (m_mapView.CurrentMode() != MapView::Mode::Editor)
			m_mapView.SetMode(MapView::Mode::Editor);
	};

	// THE LEVEL SETTINGS DIALOG: the toolbar's Level button on the viewed level,
	// a number typed into its row, a press on its material list.
	if (what == "levelsettings") {
		if (verb == "open") {
			toEditor();
			if (m_mapView.onLevelSettings) m_mapView.onLevelSettings();
		} else if (verb == "dust" || verb == "haze" || verb == "ambient") {
			const size_t row = verb == "dust" ? 0 : verb == "haze" ? 1 : 2;
			if (args.size() < 3 || !m_levelSettingsDialog.TypeNumber(row, args[2])) {
				m_console.Refuse("editor levelsettings: the dialog is not open (or no value)");
				return;
			}
		} else if (verb == "popup" && args.size() >= 3) {
			if (!m_levelSettingsDialog.IsOpen()) {
				m_console.Refuse("editor levelsettings: the dialog is not open");
				return;
			}
			m_levelSettingsDialog.OpenPopup(std::atoi(args[2].c_str()));
			m_console.Print(std::format("editor levelsettings popup: pressing #{}", args[2]));
			return;
		} else if (verb != "status" && verb != "popup") {
			m_console.RefuseUsage();
			return;
		}
		if (!m_levelSettingsDialog.IsOpen()) {
			m_console.Print("editor levelsettings: closed");
			return;
		}
		m_console.Print(std::format(
			"editor levelsettings: open {} dust {:.3f} haze {:.3f} ambient {:.3f} popup {}",
			m_levelSettingsDialog.Level(), m_levelSettingsDialog.Dust(),
			m_levelSettingsDialog.Haze(), m_levelSettingsDialog.Ambient(),
			popupWord(m_levelSettingsDialog.PopupOpen())));
		return;
	}

	// THE CREATE DIALOG: a palette section's "+ New..." row (a catalog key), and
	// a press on its form's lists.
	if (what == "newasset") {
		if (verb == "popup" && args.size() >= 3) {
			if (!m_assetDialog.IsOpen()) {
				m_console.Refuse("editor newasset: the dialog is not open");
				return;
			}
			m_assetDialog.OpenPopup(std::atoi(args[2].c_str()));
			m_console.Print(std::format("editor newasset popup: pressing #{}", args[2]));
			return;
		}
		if (verb != "status" && verb != "popup") {
			const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(verb);
			// A pure-data section's row makes a type outright (the type editor
			// opens on it) rather than this dialog, so it is no way here.
			if (cat == MapEditor::PaletteCat::Count || MapEditor::CategoryAuthorable(cat) ||
				!m_mapEditor.onNewAsset) {
				m_console.Refuse(std::format(
					"editor newasset: '{}' is not a section that opens the create dialog", verb));
				return;
			}
			toEditor();
			m_mapEditor.onNewAsset(cat);
		}
		m_console.Print(m_assetDialog.IsOpen()
							? std::format("editor newasset: open popup {}",
										  popupWord(m_assetDialog.PopupOpen()))
							: std::string("editor newasset: closed"));
		return;
	}

	// THE LEVEL DROP-DOWN on the toolbar, and what an Esc would back out of
	// after it: the armed brush, then the map.
	if (what == "levellist") {
		if (verb == "open") {
			toEditor();
			m_mapView.PressLevelPick();
		} else if (verb != "status") {
			m_console.RefuseUsage();
			return;
		}
		const MapEditor::PaletteCat armed = m_mapEditor.ArmedCat();
		m_console.Print(std::format(
			"editor levellist: {} map {} armed {}", popupWord(m_mapView.LevelListOpen()),
			!m_mapView.IsOpen()                                  ? "closed"
			: m_mapView.CurrentMode() == MapView::Mode::Editor ? "editor"
															   : "player",
			armed == MapEditor::PaletteCat::Count
				? std::string("-")
				: std::format("{}:{}", MapEditor::CategoryCatalogKey(armed),
							  m_mapEditor.ArmedId())));
		return;
	}

	// A palette row's click with no square under it: arm its brush.
	if (what == "arm") {
		if (args.size() < 3) {
			m_console.RefuseUsage();
			return;
		}
		toEditor();
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(args[1]);
		if (cat == MapEditor::PaletteCat::Count || !m_mapEditor.Arm(cat, args[2])) {
			m_console.Refuse(
				std::format("editor arm: no palette row '{}' in '{}'", args[2], args[1]));
			return;
		}
		m_console.Print(std::format("editor arm: armed {}:{}", args[1], m_mapEditor.ArmedId()));
		return;
	}
	m_console.RefuseUsage();
}

void Game::OpenInspectorFor(const InspectTarget& t) {
	const int cx = m_inspectCellX, cz = m_inspectCellZ;
	switch (t.kind) {
	case InspectTarget::Kind::Monster: {
		EntityInspector::Config c;
		c.runtimeId = t.runtimeId;
		if (!m_world->MonsterInstanceById(t.runtimeId, c.type, c.asleep, c.leashRange,
										 c.archetype, c.keepRange, c.fleeBelow, c.spell, c.facing))
			return;
		if (const auto* r = m_world->MonsterPatrol(c.runtimeId))
			c.patrolCount = static_cast<int>(r->size());
		m_world->MonsterThreatById(t.runtimeId, c.threat, c.threatLock);
		// Preview: the type's mesh + an idle animation (front-on). Build the animator
		// now (the spec carries the skeleton/clips the render loop reads).
		PreviewSpec pv;
		if (m_world->MonsterModelAvailable(c.type)) {
			const auto d = m_world->MonsterPreviewFor(c.type);
			pv.subs = d.subs; // one entry, or one per primitive (multi-material)
			pv.scale = d.scale;
			pv.yaw = d.modelYaw;
			pv.pivot = d.pivot;
			pv.skeleton = d.skeleton;
			pv.clips = d.clips;
			pv.idleClip = d.idleClip;
			m_previewAnim = DungeonWorld::MonsterAnimator(d.skeleton, d.clips); // as in the world
			if (!pv.idleClip.empty()) m_previewAnim.Play(pv.idleClip, /*loop*/ true);
		}
		// Delete: by runtimeId, so it takes THIS monster even if several share
		// the cell or it walks off mid-dialog.
		m_entityInspector.onDelete = [this, id = t.runtimeId] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveMonsterByRuntimeId(id));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		m_entityInspector.Open(c, m_world->SpellIds(), std::move(pv));
		break;
	}
	case InspectTarget::Kind::Sconce: {
		// Facing choices: the cell's solid walls, minus walls held by OTHER sconces
		// here (but always including this torch's own current wall).
		std::vector<Direction> occupied = m_world->SconcesAt(cx, cz);
		std::vector<Direction> walls;
		for (Direction d : m_world->SolidWallsAt(cx, cz)) {
			const bool takenByOther =
				d != t.wall && std::find(occupied.begin(), occupied.end(), d) != occupied.end();
			if (!takenByOther) walls.push_back(d);
		}
		FixtureInspector::Config fc;
		fc.x = cx;
		fc.z = cz;
		fc.wall = t.wall;
		if (!m_world->TorchSettings(cx, cz, t.wall, fc.lit, fc.brightness, fc.turbidity,
									fc.flameColor))
			return; // gone since the picker listed it
		fc.kindColor = m_world->FixtureLightColor(m_world->SconceTypeAt(cx, cz, t.wall));
		OpenFixtureInspector(fc, walls,
							 m_world->FixturePreviewOf(m_world->SconceTypeAt(cx, cz, t.wall)));
		break;
	}
	case InspectTarget::Kind::Brazier: {
		FixtureInspector::Config fc;
		fc.brazier = true;
		fc.x = cx;
		fc.z = cz;
		if (!m_world->BrazierSettings(cx, cz, fc.lit, fc.brightness, fc.turbidity,
									  fc.flameColor))
			return; // gone since the picker listed it
		fc.kindColor = m_world->FixtureLightColor(m_world->BrazierTypeAt(cx, cz));
		OpenFixtureInspector(fc, /*walls*/ {},
							 m_world->FixturePreviewOf(m_world->BrazierTypeAt(cx, cz)));
		break;
	}
	case InspectTarget::Kind::Door: {
		DoorInspector::Config c;
		c.x = cx;
		c.z = cz;
		DungeonWorld::DoorEdit edit;
		if (!m_world->DoorSettings(cx, cz, edit))
			return; // gone since the picker listed it
		c.open = edit.open;
		c.key = edit.key;
		c.flag = edit.flag;
		c.name = edit.name;
		c.opener = edit.opener;
		c.openerSide = edit.openerSide;
		c.easeIn = edit.easeIn;
		c.easeOut = edit.easeOut;
		c.openerEaseIn = edit.openerEaseIn;
		c.openerEaseOut = edit.openerEaseOut;
		// Selectable keys: items.cat entries with category=key.
		std::vector<std::pair<std::string, std::string>> keys;
		for (const CatalogEntry& e : m_project.items.Entries())
			if (e.Get("category", "") == "key") keys.emplace_back(e.id, e.Display());
		// Selectable openers: door entries carrying a `style`, which is what
		// makes an entry an opener rather than a leaf, a frame or a trim.
		std::vector<std::pair<std::string, std::string>> openers;
		for (const CatalogEntry& e : m_project.doors.Entries())
			if (e.Find("style")) openers.emplace_back(e.id, e.Display());
		// What the TYPE would give, so the two "Default (...)" rows can name it.
		const CatalogEntry* type = m_project.doors.Find(m_world->DoorTypeAt(cx, cz));
		const CatalogEntry* typeOpener =
			m_project.doors.Find(CatalogGet(type, "opener", ""));
		std::string typeOpenerName =
			typeOpener ? typeOpener->Display() : loc::Tr("map.door.opener_none");
		std::string typeSide =
			loc::Tr(CatalogGet(type, "opener_side", "left") == "right"
						? "map.door.side_right"
						: "map.door.side_left");
		// The speed slider shows the EFFECTIVE seconds, so an unoverridden door
		// opens the dialog on its type's number rather than on a placeholder the
		// user would then have to guess past. onApply turns it back into an
		// override only where it differs — see DoorInspector::Config.
		c.typeSeconds = type ? type->GetFloat("open_seconds", 0.7f) : 0.7f;
		c.seconds = edit.seconds > 0.0f ? edit.seconds : c.typeSeconds;
		PreviewSpec pv;
		pv.subs = m_world->DoorPreviewSubs(cx, cz);
		m_doorInspector.onDelete = [this, cx, cz] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveDoorAt(cx, cz));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		m_doorInspector.Open(c, std::move(keys), std::move(openers),
							 std::move(typeOpenerName), std::move(typeSide),
							 FlagChoiceList(), std::move(pv));
		break;
	}
	case InspectTarget::Kind::Button: {
		ButtonInspector::Config c;
		c.x = cx;
		c.z = cz;
		DungeonWorld::ButtonEdit edit;
		if (!m_world->ButtonSettings(cx, cz, edit))
			return; // gone since the picker listed it
		c.target = edit.target;
		c.needs = edit.needs;
		c.sets = edit.sets;
		c.op = edit.op;
		PreviewSpec pv;
		pv.subs = m_world->ButtonPreviewSubs(cx, cz);
		// A button can target a door OR a niche name — offer both.
		std::vector<std::string> targets = m_world->DoorNames();
		for (std::string& n : m_world->NicheNames()) targets.push_back(std::move(n));
		m_buttonInspector.onDelete = [this, cx, cz] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveButtonAt(cx, cz));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		m_buttonInspector.Open(c, std::move(targets), FlagChoiceList(), std::move(pv));
		break;
	}
	case InspectTarget::Kind::Decoration: {
		PropInspector::Config c;
		c.kind = PropInspector::Config::Kind::Decoration;
		c.handle = t.handle;
		c.type = t.type;
		c.facing = m_world->DecorationFacing(t.handle);
		PreviewSpec pv;
		pv.subs = m_world->DecorationPreviewSubs(t.handle);
		// Delete removes exactly the inspected prop (undo-bracketed like a
		// brush edit; the world is frozen while the modal is up, so the index
		// handle stays valid).
		m_propInspector.onDelete = [this, handle = t.handle] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveDecorationByIndex(handle));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		// "Map arrow" beside the Facing dropdown: the TYPE's facing_arrow flag
		// (a column/pot has no meaningful facing to point out). A toggle edits
		// the live kind and the catalog entry immediately — a type-level edit,
		// deliberately outside this instance's Save/Revert.
		const std::string typeId = m_world->DecorationTypeByIndex(t.handle);
		m_propInspector.facingExtra = InstanceInspector::FacingExtra{
			loc::Tr("map.insp.arrow"), m_world->DecorationShowsFacing(typeId),
			[this, typeId](bool show) {
				m_world->SetDecorationFacingArrow(typeId, show);
				CatalogEntry entry;
				if (const CatalogEntry* e = m_project.decorations.Find(typeId))
					entry = *e;
				else
					entry.id = typeId;
				std::erase_if(entry.fields, [](const serialize::Field& f) {
					return f.key == "facing_arrow";
				});
				if (!show) entry.Set("facing_arrow", "0"); // default 1 stays implicit
				m_project.decorations.Add(std::move(entry)); // add-or-replace by id
				if (!m_project.Save())
					log::Warn("facing-arrow toggle: failed to save project catalogs");
			}};
		m_propInspector.Open(c, std::move(pv));
		break;
	}
	case InspectTarget::Kind::Item: {
		PropInspector::Config c;
		c.kind = PropInspector::Config::Kind::Item;
		c.handle = t.handle;
		c.type = t.type;
		c.facing = m_world->ItemFacing(t.handle);
		// Items are small/loose — auto-fit + spin them on a turntable (vs the
		// grounded head-on view props use).
		PreviewSpec pv;
		pv.subs = m_world->ItemPreviewSubs(t.handle, pv.fitMin, pv.fitMax);
		pv.autoFit = true;
		pv.spin = true;
		m_previewSpin = 0.0f;
		m_propInspector.onDelete = [this, handle = t.handle] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveItemById(handle));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		m_propInspector.facingExtra.reset(); // items draw no map arrow anyway
		m_propInspector.Open(c, std::move(pv));
		break;
	}
	case InspectTarget::Kind::Projectile: {
		ProjectileInfo p;
		if (!m_world->ProjectileById(t.runtimeId, p))
			return; // landed since the picker listed it
		ProjectileInspector::Config c;
		c.id = p.id;
		// A shot targeting the PARTY was fired by a monster, and vice versa.
		c.side = loc::Tr(p.target == TargetSide::Party ? "map.proj.frommonster"
													   : "map.proj.fromparty");
		// The type names itself through the book — a project type the engine
		// has never heard of still reads correctly here.
		c.dmgType = loc::Tr(m_world->DamageTypes().NameKey(p.atk.type));
		c.damage = p.atk.damage;
		c.accuracy = p.atk.attackBonus;
		c.speed = p.speed;
		c.rangeLeft = p.rangeLeft;
		// What it will leave behind when it lands or expires, on ONE line (the card
		// keeps a fixed row count — see ProjectileInspector::Config::payload).
		// Effects are named by their AUTHORED id rather than a localized name,
		// unlike the damage type above: an effect names itself per INSTANCE
		// (EffectKind::NameKey takes an Inst, so a ward can name itself by school)
		// and nothing has landed yet — and the id is what the builder wrote in
		// `on_hit`, which is what they came here to check. A lone effect gets its
		// numbers spelled out; several would not fit, so those list ids only.
		// The words round the id are the language's (code-review C107): the
		// numbers are formatted here, the rate, duration and chance said by keys.
		const std::span<const fx::Proc> procs = p.payload.Procs();
		if (procs.size() == 1) {
			const fx::Proc& proc = procs.front();
			c.payload = proc.id.View();
			if (proc.magnitude > 0.0f || proc.duration > 0.0f)
				c.payload = loc::Format("map.proj.payload.dot", c.payload,
										std::format("{:.1f}", proc.magnitude),
										std::format("{:.1f}", proc.duration));
			if (proc.chance < 1.0f)
				c.payload = loc::Format("map.proj.payload.chance", c.payload,
										std::format("{:.0f}", proc.chance * 100.0f));
		} else {
			for (const fx::Proc& proc : procs)
				c.payload = c.payload.empty()
								? std::string(proc.id.View())
								: loc::Format("map.joined", c.payload, proc.id.View());
		}
		m_projectileInspector.onRemove = [this, id = p.id] {
			if (m_world->RemoveProjectile(id) && m_world->onMessage)
				m_world->onMessage(loc::View("map.proj.removed"));
		};
		m_projectileInspector.Open(c);
		break;
	}
	case InspectTarget::Kind::Niche: {
		const WallNiche* n = m_world->NicheOn(t.nicheX, t.nicheZ, t.wall);
		if (!n) return; // gone since the picker listed it
		NicheInspector::Config c;
		c.x = t.nicheX;
		c.z = t.nicheZ;
		c.wall = t.wall;
		c.type = n->type;
		c.hidden = n->hidden;
		c.name = n->name;
		// Selectable niche shapes from wallfeatures.cat.
		std::vector<std::pair<std::string, std::string>> types;
		for (const CatalogEntry& e : m_project.wallfeatures.Entries())
			types.emplace_back(e.id, e.Display());
		m_nicheInspector.onDelete = [this, x = t.nicheX, z = t.nicheZ, w = t.wall] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveNiche(x, z, w));
			if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
		};
		// Faces it may move to: the cell's SOLID walls that aren't already carved,
		// plus the one it currently occupies (so the dropdown can show itself).
		std::vector<Direction> walls;
		for (const Direction d : {Direction::North, Direction::East,
								  Direction::South, Direction::West}) {
			if (d == t.wall) { walls.push_back(d); continue; }
			if (m_world->Map().IsWalkable(t.nicheX + DirDX(d), t.nicheZ + DirDZ(d)))
				continue; // no rock to carve into
			if (m_world->NicheOn(t.nicheX, t.nicheZ, d)) continue; // face already carved
			walls.push_back(d);
		}
		m_nicheInspector.Open(c, std::move(types), std::move(walls));
		break;
	}
	case InspectTarget::Kind::Stair: {
		StairLink s;
		if (!m_world->StairSettings(cx, cz, s)) return; // gone since the picker listed it
		StairInspector::Config c;
		c.x = cx;
		c.z = cz;
		const CatalogEntry* type = m_project.stairs.Find(s.type);
		c.typeName = type ? type->Display() : s.type;
		c.dest = s.destLevel;
		// An exit's dest names a world-map location, not a level.
		c.destIsLevel = std::find(m_project.levels.begin(), m_project.levels.end(),
								  s.destLevel) != m_project.levels.end();
		c.destX = s.destX;
		c.destZ = s.destZ;
		c.facing = s.facing;
		c.flag = s.flag;
		PreviewSpec pv;
		pv.subs = m_world->StairPreviewSubs(cx, cz);
		// Delete takes BOTH halves, as the middle-click erase does.
		m_stairInspector.onDelete = [this, cx, cz] {
			m_world->BeginUndoStep();
			m_world->CommitUndoStep(m_world->RemoveStairAt(cx, cz));
		};
		// An exit's choices: every doorway on the world map, in file order. A
		// project with no world yet offers only "nowhere yet".
		std::vector<std::string> locations;
		if (m_worldMap)
			for (const WorldMap::Location& l : m_worldMap->Locations())
				locations.push_back(l.id);
		m_stairInspector.Open(c, std::move(locations), FlagChoiceList(), std::move(pv));
		break;
	}
	}
}

void Game::OpenFixtureInspector(const FixtureInspector::Config& fc,
								const std::vector<Direction>& walls,
								const DungeonWorld::FixturePreviewData& sp) {
	// Preview: the fixture prop mesh + a flame/smoke overlay when lit.
	PreviewSpec pv;
	pv.subs = sp.subs;
	pv.scale = sp.scale;
	pv.fire = true;
	pv.flameHeight = sp.flameHeight;
	pv.flameScale = sp.flameScale;
	pv.showFire = fc.lit;
	m_previewFire = FireEffect({0.0f, sp.flameHeight * sp.scale, 0.0f}, pv.flameScale, 1234);
	m_previewFire.SetFlameColor(fc.flameColor, HasFlameColor(fc.flameColor));
	// Delete: a sconce goes by its FACE, since a cell can ring itself with one
	// per wall and the cell-wide call would pick arbitrarily. A brazier stands
	// on the floor and has no face, so it takes the cell form.
	m_fixtureInspector.onDelete = [this, fc] {
		m_world->BeginUndoStep();
		m_world->CommitUndoStep(fc.brazier ? m_world->RemoveFixtureAt(fc.x, fc.z)
										  : m_world->RemoveFixtureAtFace(fc.x, fc.z, fc.wall));
		if (m_world->onMessage) m_world->onMessage(loc::View("map.erase.removed"));
	};
	m_fixtureInspector.Open(fc, walls, std::move(pv));
}


} // namespace dungeon::game
