// ============================================================================
// Game/DungeonWorld_Doors.cpp - split out of DungeonWorld_Editing.cpp to keep files
// small. Doors, their openers and inspector settings; niches toggled by name;
// buttons (levers) and items, including their remote-level placement.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cstdlib> // atof — the .ent `seconds=` override
#include <filesystem>
#include <format>
#include <optional>

using namespace DirectX;

namespace dungeon::game {
// ============================================================================
// Doors. Record-backed (the .ent layer carries them, like items/buttons):
// placement authors the record AND spawns the live instance, so the writer,
// the level stash, and remote editing all see one source of truth. Open-state
// changes are dynamic (save diffs), never written back to the record.
// ============================================================================
DungeonWorld::Door* DungeonWorld::DoorAt(int x, int z) {
	for (Door& d : m_doors)
		if (d.x == x && d.z == z) return &d;
	return nullptr;
}

const DungeonWorld::Door* DungeonWorld::DoorAt(int x, int z) const {
	for (const Door& d : m_doors)
		if (d.x == x && d.z == z) return &d;
	return nullptr;
}

// A catalog entry's motion shaping. One helper because door types and opener
// entries live in the SAME catalog and use the same two field names — a leaf
// entry shapes the leaf, an opener entry shapes the opener.
static EaseSpan EaseSpanOf(const CatalogEntry* def) {
	EaseSpan span;
	if (!def) return span;
	span.in = EaseShapeFromName(def->Get("ease_in", ""), span.in);
	span.out = EaseShapeFromName(def->Get("ease_out", ""), span.out);
	return span;
}

void DungeonWorld::SpawnDoor(const Entity& record) {
	Door door;
	door.id = record.id;
	door.type = record.type; // names it, and gates whether it can be broken
	door.x = record.x;
	door.z = record.z;
	door.facing = record.facing;
	if (const std::string* n = record.Param("name")) door.name = *n;
	if (const std::string* k = record.Param("key")) door.key = *k;
	if (const std::string* f = record.Param("flag")) door.flag = *f;
	if (const std::string* o = record.Param("open")) door.initialOpen = *o != "0";
	door.open = door.initialOpen;
	door.openT = door.open ? 1.0f : 0.0f;
	door.panel = &DecorationKindFor(record.type, m_project.doors);
	// The type's knobs, resolved ONCE from its catalog entry. A legacy record
	// naming a type the catalog has lost keeps the defaults, which are the
	// sideways slide and the shared frame every door had before either was
	// authorable.
	std::string frame = "door_frame";
	if (const CatalogEntry* def = m_project.doors.Find(record.type)) {
		const std::string m = def->Get("motion", "slide");
		door.motion = m == "rise"    ? DoorMotion::Rise
					  : m == "split" ? DoorMotion::Split
									 : DoorMotion::Slide;
		door.travel = def->GetFloat("travel", 0.75f);
		// CAN IT BE BROKEN DOWN? Off unless the type says so (Michael's
		// requirement): if doors were breakable by default, a party would chop
		// through every locked one and keys and switches would stop mattering. A
		// door authored `breakable = 1` is the deliberate exception — an
		// alternative route that costs time and noise instead of a key.
		if (CatalogBreakable(def)) {
			door.brk.maxHp = def->GetFloat("hp", 40.0f);
			door.brk.hp = door.brk.maxHp;
			door.brk.soak = def->GetFloat("armor", 0.0f);
			ParseResists(CatalogGet(def, "resists", ""), door.brk.resists,
						 "doors.cat [" + record.type + "]", m_damageTypes);
			fx::ReserveEffects(door.brk.effects); // a burning door burns DOWN
		}
		// Guarded: an open_seconds of 0 would divide by zero in the anim tick.
		const float secs = def->GetFloat("open_seconds", 0.7f);
		door.openSeconds = secs > 0.05f ? secs : 0.05f;
		if (const std::string t = def->Get("trim", ""); !t.empty())
			door.trim = &DecorationKindFor(t, m_project.doors);
		// The SURROUND is per type too: one mesh, but its own catalog entry, so
		// a vault door can stand in granite while a cellar door stands in the
		// wall's own stone. A named frame the catalog has lost falls back to the
		// shared one rather than leaving the doorway with no surround at all.
		if (const std::string f = def->Get("frame", ""); !f.empty()
			&& m_project.doors.Contains(f))
			frame = f;
		door.ease = EaseSpanOf(def);
	}
	door.frame = &DecorationKindFor(frame, m_project.doors);
	// The leaf's shaping, overridable per placement like the opener is.
	if (const std::string* e = record.Param("ease_in"))
		door.ease.in = EaseShapeFromName(*e, door.ease.in);
	if (const std::string* e = record.Param("ease_out"))
		door.ease.out = EaseShapeFromName(*e, door.ease.out);
	// ... and its SPEED. Two doors of one type are rarely the same door: the
	// cell door at the end of a corridor and the one the boss stands behind are
	// both `stone_door`, and how long each takes to grind open is a thing about
	// the PLACEMENT, not about stone. Guarded like the type's, since the record
	// is hand-editable and a zero divides by zero in the animation tick.
	if (const std::string* s = record.Param("seconds"))
		if (const float v = static_cast<float>(std::atof(s->c_str())); v > 0.05f)
			door.openSeconds = v;
	const std::string* o = record.Param("opener");
	const std::string* s = record.Param("opener_side");
	ResolveDoorOpener(door, record.type, o ? *o : std::string(),
					  s ? *s : std::string());
	if (const std::string* e = record.Param("opener_ease_in"))
		door.openerEase.in = EaseShapeFromName(*e, door.openerEase.in);
	if (const std::string* e = record.Param("opener_ease_out"))
		door.openerEase.out = EaseShapeFromName(*e, door.openerEase.out);
	m_doors.push_back(std::move(door));
}

// The type's opener, then the placement's own say over it. Shared by SpawnDoor
// and the inspector's edit, which is the point of pulling it out: the editor
// changes the overrides without respawning, so both paths have to land on the
// same resolution or an edited door would differ from the same door reloaded.
//
// THREE STATES, and the empty one is not the same as "none": an absent override
// INHERITS the type, while "none" is this door saying it has no hand-hold
// whatever its type carries. Collapsing them would make "the type's default"
// unsayable the moment an instance was edited once.
void DungeonWorld::ResolveDoorOpener(Door& door, const std::string& type,
									 const std::string& openerParam,
									 const std::string& sideParam) {
	std::string opener, side = "left";
	if (const CatalogEntry* def = m_project.doors.Find(type)) {
		opener = def->Get("opener", "");
		side = def->Get("opener_side", "left");
	}
	if (!openerParam.empty()) opener = openerParam;
	if (!sideParam.empty()) side = sideParam;
	door.openerX = side == "right" ? kOpenerX : -kOpenerX;
	door.opener = nullptr;
	door.openerMount = nullptr;
	door.openerStyle = OpenerStyle::Pad;
	door.openerEase = {};
	// Note the ORDER: the side is set before the early-outs below, so a door
	// with no opener still carries a sensible jamb — the field is read by the
	// editor and by anything that later wants to hang something there.
	// An override naming a type the catalog has lost leaves the door with no
	// hand-hold, which is the safe way round: it becomes button-only rather
	// than opening on a click with nothing drawn to explain why.
	if (opener.empty() || opener == "none" || !m_project.doors.Contains(opener))
		return;
	const CatalogEntry* def = m_project.doors.Find(opener);
	// How far out along the jamb THIS opener wants to sit — see kOpenerX for why
	// one number cannot serve both a pad and a chain.
	const float off = def ? def->GetFloat("offset", kOpenerX) : kOpenerX;
	door.openerX = side == "right" ? off : -off;
	door.openerEase = EaseSpanOf(def);
	door.opener = &DecorationKindFor(opener, m_project.doors);
	door.openerStyle =
		CatalogGet(def, "style", "pad") == "chain" ? OpenerStyle::Chain
												  : OpenerStyle::Pad;
	// The static half, if the opener has one — the socket a chain runs out of.
	if (const std::string m = CatalogGet(def, "mount", "");
		!m.empty() && m_project.doors.Contains(m))
		door.openerMount = &DecorationKindFor(m, m_project.doors);
}

bool DungeonWorld::AddDoor(const std::string& type, int x, int z) {
	Direction facing;
	if (!DungeonMap::DoorwayFacing(m_map, x, z, facing)) {
		if (onMessage) onMessage(loc::View("map.door.nodoorway"));
		return false;
	}
	return AddDoor(type, x, z, facing);
}

bool DungeonWorld::AddDoor(const std::string& type, int x, int z,
						   Direction facing) {
	if (!m_project.doors.Contains(type) || DoorAt(x, z)) return false;
	// Spawning a CLOSED door under the party or a monster would wall them in.
	if ((x == m_party.GridX() && z == m_party.GridZ()) ||
		MonsterRuntimeIdAt(x, z) != 0)
		return false;
	Entity record;
	record.kind = EntityKind::Door;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = facing;
	record.id = m_entities.Add(record); // Add() assigns; mirror it locally
	m_entsDirty = true;
	SpawnDoor(record);
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddDoorRemote(const std::string& stem,
								 const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.doors.Contains(type)) return false;
	Direction facing;
	if (!DungeonMap::DoorwayFacing(map, x, z, facing)) {
		if (onMessage) onMessage(loc::View("map.door.nodoorway"));
		return false;
	}
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Door) return false; // one door per cell
	Entity record;
	record.kind = EntityKind::Door;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = facing;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::ToggleDoor(Door& door) {
	// A BROKEN door is open for good — there is no panel left to work. This is the
	// one place the invariant has to hold, because it covers every route in: the
	// party's click, a wired button, and the editor's inspector all arrive here.
	if (door.brk.broken) {
		if (onMessage) onMessage(loc::View("log.door_wrecked"));
		return false;
	}
	// Anything standing in the doorway jams a closing panel.
	if (door.open && MonsterRuntimeIdAt(door.x, door.z) != 0) {
		if (onMessage) onMessage(loc::View("log.door_jammed"));
		return false;
	}
	door.open = !door.open;
	if (onMessage)
		onMessage(loc::View(door.open ? "log.door_open" : "log.door_close"));
	return true;
}

Vec3 DungeonWorld::OpenerPos(const Door& door, float face) const {
	// The render's own placement, in world terms: the opener sits at
	// (openerX, kOpenerY, face * kOpenerFaceZ) in the door's UNIT model space,
	// which the door's base matrix scales, turns by its facing and drops on the
	// cell. Kept in step with DungeonWorld_Render's door loop by construction —
	// both read the same four constants.
	const float s = kUnit * (door.frame ? door.frame->modelScale : 1.0f);
	const Vec3 c = m_map.CellCenter(door.x, door.z);
	const float yaw = DirYaw(door.facing);
	const float ca = std::cos(yaw), sa = std::sin(yaw);
	const float lx = door.openerX * s, lz = face * kOpenerFaceZ * s;
	// Row-vector convention (v' = v * RotY), matching the render's matrices.
	return {c.x + lx * ca + lz * sa, kOpenerY * s, c.z - lx * sa + lz * ca};
}

bool DungeonWorld::ToggleDoorAhead(float mx, float my, float w, float h) {
	const Direction f = static_cast<Direction>(m_party.Facing());
	Door* door = DoorAt(m_party.GridX() + DirDX(f), m_party.GridZ() + DirDZ(f));
	if (!door) return false;
	// NO HAND-HOLD, NO HAND. A door without an opener is worked from somewhere
	// else — a lever down the corridor, a pressure plate, a spell — and the
	// party has nothing to take hold of. This is what makes the setting mean
	// anything: without the refusal, "opened by other means" would still open to
	// anyone who walked up and clicked. Wired buttons go through ToggleDoorsNamed
	// and never come here, so a mechanism is unaffected — as with key locks.
	if (!door->opener) {
		if (onMessage) onMessage(loc::FormatLine("log.door_nohandle", LeaderName()));
		return true; // the click WAS for the door; it just found nothing to pull
	}
	// THE RAY HAS TO HIT THE HAND-HOLD. A sphere around it rather than the mesh:
	// the same test the niche items use, and a chain is a thin thing to ask
	// somebody to hit exactly. Both faces are tested because the party may be on
	// either side and only one copy is theirs; the far one is behind the door
	// and cannot be hit through it anyway.
	const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
	const float s = kUnit * (door->frame ? door->frame->modelScale : 1.0f);
	const bool chain = door->openerStyle == OpenerStyle::Chain;
	// Centre on the middle of what is DRAWN, not on the origin: a chain runs
	// upward from its grip, so a sphere on the grip would leave the links
	// unclickable — the part of it most people would aim at.
	const float rise = chain ? 0.17f * s : 0.0f;
	const float radius = (chain ? 0.26f : 0.13f) * s;
	bool hit = false;
	for (const float face : {-1.0f, 1.0f}) {
		const Vec3 p = OpenerPos(*door, face);
		const Vec3 oc{ray.origin.x - p.x, ray.origin.y - (p.y + rise),
					  ray.origin.z - p.z};
		const float bb = oc.x * ray.dir.x + oc.y * ray.dir.y + oc.z * ray.dir.z;
		const float cc = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - radius * radius;
		const float disc = bb * bb - cc;
		if (disc >= 0.0f && -bb + std::sqrt(disc) > 0.0f) hit = true;
	}
	if (!hit) return false; // missed the hand-hold: not this door's click
	door->pullT = 0.0f;     // the throw; DungeonWorld::Update runs it and eases
	door->pullRising = true; // it back afterwards, slower
	HandOnDoor(*door);
	return true; // the click was for the door even if it jammed
}

bool DungeonWorld::HandOnDoor(Door& door) {
	// A door waiting on a FLAG refuses the hand until the flag is on, before the
	// key is even asked about: a seal is not something a key turns. Wired
	// buttons still move it, as they do a locked one.
	if (!door.open && !FlagOn(door.flag)) {
		if (onMessage) onMessage(loc::View("log.door_sealed"));
		return false;
	}
	// A keyed door refuses the hand unless a member carries the key item (the
	// key stays — the door re-locks when shut). Wired buttons still move it
	// (mechanisms don't need the key).
	if (!door.open && !door.key.empty()) {
		if (!PartyHasItem(door.key)) {
			if (onMessage) onMessage(loc::FormatLine("log.door_locked", LeaderName()));
			return false;
		}
		if (onMessage) onMessage(loc::FormatLine("log.door_unlock", LeaderName()));
	}
	return ToggleDoor(door);
}

bool DungeonWorld::HandOnDoorAt(int x, int z, bool& open) {
	Door* door = DoorAt(x, z);
	if (!door) return false;
	HandOnDoor(*door);
	open = door->open;
	return true;
}

bool DungeonWorld::PartyHasItem(std::string_view typeId) const {
	if (typeId.empty() || !m_roster) return false;
	for (const Character& member : *m_roster)
		if (member.inventory.Has(typeId)) return true;
	return false;
}

std::string DungeonWorld::DoorTypeAt(int x, int z) const {
	// The type lives on the RECORD — a Door holds resolved meshes, not its id.
	const Door* door = DoorAt(x, z);
	if (!door) return {};
	for (const Entity& e : m_entities.All())
		if (e.id == door->id) return e.type;
	return {};
}

bool DungeonWorld::DoorSettings(int x, int z, DoorEdit& out) const {
	const Door* door = DoorAt(x, z);
	if (!door) return false;
	out.open = door->open;
	out.key = door->key;
	out.flag = door->flag;
	out.name = door->name;
	// The opener overrides come from the RECORD, not the door: the door holds
	// the RESOLVED opener, which cannot tell "inherit" from "the type's value
	// spelled out". The dialog needs that difference to offer a Default row.
	out.opener.clear();
	out.openerSide.clear();
	out.easeIn.clear();
	out.easeOut.clear();
	out.openerEaseIn.clear();
	out.openerEaseOut.clear();
	out.seconds = 0.0f;
	for (const Entity& e : m_entities.All()) {
		if (e.id != door->id) continue;
		auto get = [&](const char* k, std::string& to) {
			if (const std::string* v = e.Param(k)) to = *v;
		};
		get("opener", out.opener);
		get("opener_side", out.openerSide);
		get("ease_in", out.easeIn);
		get("ease_out", out.easeOut);
		get("opener_ease_in", out.openerEaseIn);
		get("opener_ease_out", out.openerEaseOut);
		if (const std::string* v = e.Param("seconds"))
			out.seconds = static_cast<float>(std::atof(v->c_str()));
		break;
	}
	return true;
}

void DungeonWorld::SetDoorSettings(int x, int z, const DoorEdit& in) {
	Door* door = DoorAt(x, z);
	if (!door) return;
	NoteEdit(); // an inspector apply: no undo step, still a change to check
	door->open = in.open;
	door->initialOpen = in.open; // the editor edits the AUTHORED state
	door->key = in.key;
	door->flag = in.flag;
	door->name = in.name;
	// Mirror onto the .ent record so the writer/stash carry it. Default-valued
	// params are removed to keep records minimal.
	if (Entity* record = m_entities.MutableById(door->id)) {
		auto set = [&](const char* k, const std::string& v) {
			std::erase_if(record->params,
						  [&](const auto& p) { return p.first == k; });
			if (!v.empty()) record->params.emplace_back(k, v);
		};
		set("open", in.open ? "1" : "");
		set("key", in.key);
		set("flag", in.flag);
		set("name", in.name);
		set("opener", in.opener);
		set("opener_side", in.openerSide);
		set("ease_in", in.easeIn);
		set("ease_out", in.easeOut);
		set("opener_ease_in", in.openerEaseIn);
		set("opener_ease_out", in.openerEaseOut);
		// Two decimals, because a slider hands back whatever pixel it was let go
		// on and a record line is read by people.
		set("seconds",
			in.seconds > 0.0f ? std::format("{:.2f}", in.seconds) : std::string());
		m_entsDirty = true;
		// The opener is RESOLVED at spawn, so editing it has to re-resolve: the
		// live door holds a mesh pointer and a style read from the old values.
		// In place rather than by respawning the door — a respawn would go
		// through the record path and take the open state, the animation and the
		// runtime id with it, to re-derive four fields.
		ResolveDoorOpener(*door, record->type, in.opener, in.openerSide);
		// The curves too: both fall back to the TYPE's value when the override
		// is cleared, which is what ResolveDoorOpener has already restored for
		// the opener's pair, and what this re-reads for the leaf's.
		const CatalogEntry* def = m_project.doors.Find(record->type);
		door->ease = EaseSpanOf(def);
		door->ease.in = EaseShapeFromName(in.easeIn, door->ease.in);
		door->ease.out = EaseShapeFromName(in.easeOut, door->ease.out);
		// And the throw time, resolved the same way and guarded the same way as
		// at spawn: a zero would divide by zero in the animation tick.
		const float secs =
			in.seconds > 0.0f ? in.seconds
							  : (def ? def->GetFloat("open_seconds", 0.7f) : 0.7f);
		door->openSeconds = secs > 0.05f ? secs : 0.05f;
		door->openerEase.in = EaseShapeFromName(in.openerEaseIn, door->openerEase.in);
		door->openerEase.out =
			EaseShapeFromName(in.openerEaseOut, door->openerEase.out);
	}
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::DoorPreviewSubs(int x, int z) const {
	std::vector<gfx::PreviewSubmesh> subs;
	const Door* door = DoorAt(x, z);
	if (!door) return subs;
	for (const DecorationKind* kind : {door->frame, door->panel}) {
		if (!kind || !kind->mesh) continue;
		gfx::MaterialParams mat;
		mat.doubleSided = true;
		ApplyPropMaterial(mat, *kind, 0.85f);
		subs.push_back({kind->mesh.get(), mat});
	}
	return subs;
}

void DungeonWorld::ToggleDoorsNamed(const std::string& name) {
	if (name.empty()) return;
	for (Door& door : m_doors)
		if (door.name == name) ToggleDoor(door);
}

// A button targeting `name` also opens/closes any niche with that name — the
// secret-niche reveal. Flips the map's runtime `open` and re-stamps each touched
// cell's wall panel (blank ⇄ recessed pocket).
bool DungeonWorld::ToggleNichesNamed(const std::string& name) {
	const std::vector<std::pair<int, int>> touched = m_map.ToggleNichesNamed(name);
	for (const auto& [x, z] : touched) RebuildChunksAround(x, z);
	return !touched.empty();
}

std::vector<std::string> DungeonWorld::NicheNames() const { return m_map.NicheNames(); }

std::vector<DungeonWorld::NicheFace> DungeonWorld::NicheFacesAt(int cx, int cz) const {
	// A niche is selected ONLY by clicking the WALL BLOCK it is carved into — it
	// reads as being IN the wall, so clicking the floor square it opens onto (even
	// though the niche is registered there) does nothing, which is less confusing.
	std::vector<NicheFace> faces;
	if (m_map.IsWalkable(cx, cz)) return faces; // clicked a floor cell — not a wall
	const int nbr[4][2] = {{cx, cz - 1}, {cx + 1, cz}, {cx, cz + 1}, {cx - 1, cz}};
	for (const auto& f : nbr)
		if (const WallNiche* n = m_map.NicheAt(f[0], f[1], cx - f[0], cz - f[1]))
			faces.push_back({f[0], f[1], n->wall});
	return faces;
}

const WallNiche* DungeonWorld::NicheOn(int x, int z, Direction wall) const {
	return m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall));
}

void DungeonWorld::SetNichePropsAt(int x, int z, Direction wall, const std::string& name,
								   bool hidden, const std::string& type) {
	NoteEdit(); // a niche's name is a button target
	if (m_map.SetNichePropsAt(x, z, wall, name, hidden, type)) RebuildChunksAround(x, z);
}

bool DungeonWorld::RemoveNiche(int x, int z, Direction wall) {
	if (!m_map.RemoveNiche(x, z, wall)) return false;
	RebuildChunksAround(x, z);
	return true;
}

std::vector<DungeonWorld::DoorMarker> DungeonWorld::DoorMarkers() const {
	std::vector<DoorMarker> markers;
	markers.reserve(m_doors.size());
	for (const Door& d : m_doors) markers.push_back({d.x, d.z, d.facing, d.open});
	return markers;
}

// ============================================================================
// Buttons. Record-backed like doors; the lever mounts on a solid wall of its
// cell and toggles the doors its target= names when pressed.
// ============================================================================
bool DungeonWorld::AddButton(const std::string& type, int x, int z) {
	if (!m_project.buttons.Contains(type) || !m_map.IsWalkable(x, z)) return false;
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z) return false; // one button per cell (editor rule)
	// Auto-mount on the first solid neighbour wall, like the 'T' glyph.
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	Direction wall = Direction::North;
	bool found = false;
	for (const Direction d : kScan)
		if (!m_map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
			wall = d;
			found = true;
			break;
		}
	if (!found) {
		if (onMessage) onMessage(loc::View("map.button.nowall"));
		return false;
	}
	Entity record;
	record.kind = EntityKind::Button;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = wall;
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	Button b;
	b.id = record.id;
	b.x = x;
	b.z = z;
	b.facing = wall;
	b.kind = &DecorationKindFor(type, m_project.buttons);
	if (m_project.buttons.Contains("lever_plate"))
		b.plate = &DecorationKindFor("lever_plate", m_project.buttons);
	m_buttons.push_back(std::move(b));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddButtonRemote(const std::string& stem,
								   const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.buttons.Contains(type) || !map.IsWalkable(x, z)) return false;
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Button) return false;
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	Direction wall = Direction::North;
	bool found = false;
	for (const Direction d : kScan)
		if (!map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
			wall = d;
			found = true;
			break;
		}
	if (!found) {
		if (onMessage) onMessage(loc::View("map.button.nowall"));
		return false;
	}
	Entity record;
	record.kind = EntityKind::Button;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = wall;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::AddItem(const std::string& type, int x, int z, int slot) {
	if (!m_project.HasItem(type) || !m_map.IsWalkable(x, z)) return false;
	// One item per quarter slot — a full cell (4 on the floor) refuses rather
	// than letting FreeItemSlotNear stack overlapping tablets. Niche items pile
	// separately (they don't use the floor quarters), so they don't count.
	int here = 0;
	for (const Item& it : m_items)
		if (!it.collected && it.niche < 0 && it.x == x && it.z == z) ++here;
	if (here >= 4) return false;
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	// An explicit quarter is authored into the record so it survives a reload;
	// without one, fall back to the nearest free quarter to the cell centre,
	// which is what the loader itself does for a record that carries none.
	if (slot >= 0 && slot < 4) record.params.emplace_back("slot", std::to_string(slot));
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	ItemKind& kind = ItemKindFor(type);
	if (slot < 0 || slot > 3) {
		const Vec3 c = m_map.CellCenter(x, z);
		slot = FreeItemSlotNear(x, z, c.x, c.z, -1);
	}
	m_items.push_back({&kind, record.id, x, z, false, slot});
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::NicheOpenAt(int x, int z, Direction wall) const {
	const WallNiche* n = m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall));
	return n && n->open;
}

Vec3 DungeonWorld::NicheItemPos(int x, int z, Direction wall) const {
	const int dx = DirDX(wall), dz = DirDZ(wall);
	const Vec3 c = m_map.CellCenter(x, z);
	// Units (fractions of a square) -> metres, like every other model dimension.
	const float into = kCellSize * 0.5f + 0.072f * kUnit; // inside the wall pocket
	// Rest on the pocket floor — its height is the niche mesh's py0 (must track
	// ModelBaker's BuildWallNiche / BuildWallNicheArch): 0.30 for the plain niche,
	// 0.20 for the arch. Placing below it would bury the item behind the frame.
	float floorY = 0.30f;
	if (const WallNiche* n = m_map.NicheAt(x, z, dx, dz); n && n->type == "niche_arch")
		floorY = 0.20f;
	return {c.x + dx * into, (floorY + 0.008f) * kUnit, c.z + dz * into};
}

bool DungeonWorld::AddNicheItem(const std::string& type, int x, int z, Direction wall) {
	if (!m_project.HasItem(type)) return false;
	if (!m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall))) return false; // no niche here
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	static const char* kDirName[4] = {"north", "east", "south", "west"}; // Direction order
	record.params.emplace_back("niche", kDirName[static_cast<int>(wall)]); // .ent round-trips it
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	ItemKind& kind = ItemKindFor(type);
	m_items.push_back({&kind, record.id, x, z, false, 0, static_cast<int>(wall)});
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddItemRemote(const std::string& stem,
								 const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.HasItem(type) || !map.IsWalkable(x, z)) return false;
	int here = 0;
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Item) ++here;
	if (here >= 4) return false; // one per quarter, like the live rule
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::PressButtonFacing() {
	const Direction f = static_cast<Direction>(m_party.Facing());
	for (Button& b : m_buttons)
		if (b.x == m_party.GridX() && b.z == m_party.GridZ() && b.facing == f) {
			// A lever waiting on a flag will not move at all: the press is the
			// party's, and it found nothing that gives.
			if (!FlagOn(b.needs)) {
				m_audio.Play(m_sounds.bump, 0.4f);
				if (onMessage) onMessage(loc::FormatLine("log.button_stuck", LeaderName()));
				return true;
			}
			PressButton(b);
			return true;
		}
	return false;
}

void DungeonWorld::PressButton(Button& b) {
	b.activated = !b.activated;
	// The handle snaps to its other pose (code-review C178).
	const WallMount mount = MountOnWall(b.x, b.z, b.facing);
	m_shadows.NoteCasterChanged({mount.pos.x, kEyeHeight, mount.pos.z}, 0.45f * kUnit);
	m_audio.Play(m_sounds.bump, 0.4f); // a soft clunk until a click exists
	if (onMessage) onMessage(loc::FormatLine("log.button_press", LeaderName()));
	ToggleDoorsNamed(b.target);
	ToggleNichesNamed(b.target); // secret-niche reveal
	ApplyFlagOp(b.op, b.sets);
}

void DungeonWorld::ApplyFlagOp(FlagOp op, std::string_view id) {
	if (op == FlagOp::None || id.empty() || !m_flagStore) return;
	const bool on = op == FlagOp::Set ? true
				  : op == FlagOp::Clear ? false
										: !m_flagStore->FlagOn(id);
	m_flagStore->SetFlagOn(id, on);
}

void DungeonWorld::ReadButtonFlags(const Entity& record, Button& b) {
	b.needs.clear();
	b.sets.clear();
	b.op = FlagOp::None;
	if (const std::string* f = record.Param("flag")) b.needs = *f;
	// One op per lever: the first of sets= / clears= / toggles= it carries.
	for (const auto& [k, v] : record.params)
		if (const FlagOp op = FlagOpFromKey(k); op != FlagOp::None) {
			b.op = op;
			b.sets = v;
			break;
		}
}

bool DungeonWorld::ButtonSettings(int x, int z, ButtonEdit& out) const {
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			out.target = b.target;
			out.needs = b.needs;
			out.sets = b.sets;
			out.op = b.op;
			return true;
		}
	return false;
}

void DungeonWorld::SetButtonSettings(int x, int z, const ButtonEdit& in) {
	NoteEdit(); // an inspector apply: no undo step, still a change to check
	for (Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			if (Entity* record = m_entities.MutableById(b.id)) {
				std::erase_if(record->params, [](const auto& p) {
					return p.first == "target" || p.first == "flag" ||
						   FlagOpFromKey(p.first) != FlagOp::None;
				});
				if (!in.target.empty()) record->params.emplace_back("target", in.target);
				if (!in.needs.empty()) record->params.emplace_back("flag", in.needs);
				// An op with no flag to act on, or a flag with no op, is no wiring.
				if (in.op != FlagOp::None && !in.sets.empty())
					record->params.emplace_back(FlagOpKey(in.op), in.sets);
				m_entsDirty = true;
				b.target = in.target;
				ReadButtonFlags(*record, b); // the live lever reads what was written
			} else {
				b.target = in.target;
			}
			return;
		}
}

std::vector<std::string> DungeonWorld::DoorNames() const {
	std::vector<std::string> names;
	for (const Door& d : m_doors)
		if (!d.name.empty() &&
			std::find(names.begin(), names.end(), d.name) == names.end())
			names.push_back(d.name);
	return names;
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::ButtonPreviewSubs(int x, int z) const {
	std::vector<gfx::PreviewSubmesh> subs;
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z && b.kind && b.kind->mesh) {
			gfx::MaterialParams mat;
			mat.doubleSided = true;
			ApplyPropMaterial(mat, *b.kind, 0.85f);
			subs.push_back({b.kind->mesh.get(), mat});
			break;
		}
	return subs;
}

} // namespace dungeon::game
