// ============================================================================
// Game/Game_Inspect.cpp — split out of Game.cpp to keep files small (see Game.h).
// The editor Select-tool inspector dispatch.
// ============================================================================
#include "Game/Game.h"

#include "Core/Loc.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace dungeon::game {
std::array<InstanceInspector*, 6> Game::InstanceInspectors() {
	return {&m_entityInspector, &m_fixtureInspector, &m_propInspector,
			&m_doorInspector,   &m_buttonInspector,  &m_nicheInspector};
}

InstanceInspector* Game::ActiveInstanceInspector() {
	for (InstanceInspector* ii : InstanceInspectors())
		if (ii->IsOpen()) return ii;
	return nullptr;
}

void Game::OpenInspectorFor(const InspectTarget& t) {
	const int cx = m_inspectCellX, cz = m_inspectCellZ;
	switch (t.kind) {
	case InspectTarget::Kind::Monster: {
		EntityInspector::Config c;
		c.runtimeId = t.runtimeId;
		if (!m_world.MonsterInstanceById(t.runtimeId, c.type, c.asleep, c.leashRange,
										 c.archetype, c.keepRange, c.fleeBelow, c.spell, c.facing))
			return;
		if (const auto* r = m_world.MonsterPatrol(c.runtimeId))
			c.patrolCount = static_cast<int>(r->size());
		m_world.MonsterThreatById(t.runtimeId, c.threat, c.threatLock);
		m_inspectCfg = c; // remembered so route-laying can reopen the inspector
		// Preview: the type's mesh + an idle animation (front-on). Build the animator
		// now (the spec carries the skeleton/clips the render loop reads).
		PreviewSpec pv;
		if (m_world.MonsterModelAvailable(c.type)) {
			const auto d = m_world.MonsterPreviewFor(c.type);
			pv.subs = d.subs; // one entry, or one per primitive (multi-material)
			pv.scale = d.modelScale;
			pv.yaw = d.modelYaw;
			pv.skeleton = d.skeleton;
			pv.clips = d.clips;
			if (d.clips && !d.clips->empty()) pv.idleClip = d.clips->front().name;
			m_previewAnim = anim::Animator(d.skeleton, d.clips);
			if (!pv.idleClip.empty()) m_previewAnim.Play(pv.idleClip, /*loop*/ true);
		}
		m_inspectPreview = pv; // cached so route-laying can re-pass it on reopen
		// Delete: by runtimeId, so it takes THIS monster even if several share
		// the cell or it walks off mid-dialog.
		m_entityInspector.onDelete = [this, id = t.runtimeId] {
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveMonsterByRuntimeId(id));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		m_entityInspector.Open(c, m_world.SpellIds(), std::move(pv));
		break;
	}
	case InspectTarget::Kind::Sconce: {
		// Facing choices: the cell's solid walls, minus walls held by OTHER sconces
		// here (but always including this torch's own current wall).
		std::vector<Direction> occupied = m_world.SconcesAt(cx, cz);
		std::vector<Direction> walls;
		for (Direction d : m_world.SolidWallsAt(cx, cz)) {
			const bool takenByOther =
				d != t.wall && std::find(occupied.begin(), occupied.end(), d) != occupied.end();
			if (!takenByOther) walls.push_back(d);
		}
		FixtureInspector::Config fc;
		fc.x = cx;
		fc.z = cz;
		fc.wall = t.wall;
		if (!m_world.TorchSettings(cx, cz, t.wall, fc.lit, fc.brightness, fc.turbidity))
			return; // gone since the picker listed it
		OpenFixtureInspector(fc, walls,
							 m_world.FixturePreviewOf(m_world.SconceTypeAt(cx, cz, t.wall)));
		break;
	}
	case InspectTarget::Kind::Brazier: {
		FixtureInspector::Config fc;
		fc.brazier = true;
		fc.x = cx;
		fc.z = cz;
		if (!m_world.BrazierSettings(cx, cz, fc.lit, fc.brightness, fc.turbidity))
			return; // gone since the picker listed it
		OpenFixtureInspector(fc, /*walls*/ {},
							 m_world.FixturePreviewOf(m_world.BrazierTypeAt(cx, cz)));
		break;
	}
	case InspectTarget::Kind::Door: {
		DoorInspector::Config c;
		c.x = cx;
		c.z = cz;
		DungeonWorld::DoorEdit edit;
		if (!m_world.DoorSettings(cx, cz, edit))
			return; // gone since the picker listed it
		c.open = edit.open;
		c.key = edit.key;
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
		const CatalogEntry* type = m_project.doors.Find(m_world.DoorTypeAt(cx, cz));
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
		pv.subs = m_world.DoorPreviewSubs(cx, cz);
		m_doorInspector.onDelete = [this, cx, cz] {
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveDoorAt(cx, cz));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		m_doorInspector.Open(c, std::move(keys), std::move(openers),
							 std::move(typeOpenerName), std::move(typeSide),
							 std::move(pv));
		break;
	}
	case InspectTarget::Kind::Button: {
		ButtonInspector::Config c;
		c.x = cx;
		c.z = cz;
		if (!m_world.ButtonSettings(cx, cz, c.target))
			return; // gone since the picker listed it
		PreviewSpec pv;
		pv.subs = m_world.ButtonPreviewSubs(cx, cz);
		// A button can target a door OR a niche name — offer both.
		std::vector<std::string> targets = m_world.DoorNames();
		for (std::string& n : m_world.NicheNames()) targets.push_back(std::move(n));
		m_buttonInspector.onDelete = [this, cx, cz] {
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveButtonAt(cx, cz));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		m_buttonInspector.Open(c, std::move(targets), std::move(pv));
		break;
	}
	case InspectTarget::Kind::Decoration: {
		PropInspector::Config c;
		c.kind = PropInspector::Config::Kind::Decoration;
		c.handle = t.handle;
		c.type = t.type;
		c.facing = m_world.DecorationFacing(t.handle);
		PreviewSpec pv;
		pv.subs = m_world.DecorationPreviewSubs(t.handle);
		// Delete removes exactly the inspected prop (undo-bracketed like a
		// brush edit; the world is frozen while the modal is up, so the index
		// handle stays valid).
		m_propInspector.onDelete = [this, handle = t.handle] {
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveDecorationByIndex(handle));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		// "Map arrow" beside the Facing dropdown: the TYPE's facing_arrow flag
		// (a column/pot has no meaningful facing to point out). A toggle edits
		// the live kind and the catalog entry immediately — a type-level edit,
		// deliberately outside this instance's Save/Revert.
		const std::string typeId = m_world.DecorationTypeByIndex(t.handle);
		m_propInspector.facingExtra = InstanceInspector::FacingExtra{
			loc::Tr("map.insp.arrow"), m_world.DecorationShowsFacing(typeId),
			[this, typeId](bool show) {
				m_world.SetDecorationFacingArrow(typeId, show);
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
		c.facing = m_world.ItemFacing(t.handle);
		// Items are small/loose — auto-fit + spin them on a turntable (vs the
		// grounded head-on view props use).
		PreviewSpec pv;
		pv.subs = m_world.ItemPreviewSubs(t.handle, pv.fitMin, pv.fitMax);
		pv.autoFit = true;
		pv.spin = true;
		m_previewSpin = 0.0f;
		m_propInspector.onDelete = [this, handle = t.handle] {
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveItemById(handle));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		m_propInspector.facingExtra.reset(); // items draw no map arrow anyway
		m_propInspector.Open(c, std::move(pv));
		break;
	}
	case InspectTarget::Kind::Projectile: {
		ProjectileInfo p;
		if (!m_world.ProjectileById(t.runtimeId, p))
			return; // landed since the picker listed it
		ProjectileInspector::Config c;
		c.id = p.id;
		// A shot targeting the PARTY was fired by a monster, and vice versa.
		c.side = loc::Tr(p.target == TargetSide::Party ? "map.proj.frommonster"
													   : "map.proj.fromparty");
		// The type names itself through the book — a project type the engine
		// has never heard of still reads correctly here.
		c.dmgType = loc::Tr(m_world.DamageTypes().NameKey(p.atk.type));
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
		const std::span<const fx::Proc> procs = p.payload.Procs();
		if (procs.size() == 1) {
			const fx::Proc& proc = procs.front();
			c.payload = proc.id;
			if (proc.magnitude > 0.0f || proc.duration > 0.0f)
				c.payload += std::format(" {:.1f}/s for {:.1f}s", proc.magnitude,
										 proc.duration);
			if (proc.chance < 1.0f)
				c.payload += std::format(" ({:.0f}%)", proc.chance * 100.0f);
		} else {
			for (const fx::Proc& proc : procs)
				c.payload += (c.payload.empty() ? "" : ", ") + proc.id;
		}
		m_projectileInspector.onRemove = [this, id = p.id] {
			if (m_world.RemoveProjectile(id) && m_world.onMessage)
				m_world.onMessage(loc::View("map.proj.removed"));
		};
		m_projectileInspector.Open(c);
		break;
	}
	case InspectTarget::Kind::Niche: {
		const WallNiche* n = m_world.NicheOn(t.nicheX, t.nicheZ, t.wall);
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
			m_world.BeginUndoStep();
			m_world.CommitUndoStep(m_world.RemoveNiche(x, z, w));
			if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
		};
		// Faces it may move to: the cell's SOLID walls that aren't already carved,
		// plus the one it currently occupies (so the dropdown can show itself).
		std::vector<Direction> walls;
		for (const Direction d : {Direction::North, Direction::East,
								  Direction::South, Direction::West}) {
			if (d == t.wall) { walls.push_back(d); continue; }
			if (m_world.Map().IsWalkable(t.nicheX + DirDX(d), t.nicheZ + DirDZ(d)))
				continue; // no rock to carve into
			if (m_world.NicheOn(t.nicheX, t.nicheZ, d)) continue; // face already carved
			walls.push_back(d);
		}
		m_nicheInspector.Open(c, std::move(types), std::move(walls));
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
	// Delete: a sconce goes by its FACE, since a cell can ring itself with one
	// per wall and the cell-wide call would pick arbitrarily. A brazier stands
	// on the floor and has no face, so it takes the cell form.
	m_fixtureInspector.onDelete = [this, fc] {
		m_world.BeginUndoStep();
		m_world.CommitUndoStep(fc.brazier ? m_world.RemoveFixtureAt(fc.x, fc.z)
										  : m_world.RemoveFixtureAtFace(fc.x, fc.z, fc.wall));
		if (m_world.onMessage) m_world.onMessage(loc::View("map.erase.removed"));
	};
	m_fixtureInspector.Open(fc, walls, std::move(pv));
}


} // namespace dungeon::game
