// ============================================================================
// Game/DungeonWorld_LevelIO.cpp - split out of DungeonWorld_Editing.cpp to keep files
// small. Level entry (BeginLevelLoad), stashing the static layer, and the
// .map / .ent serializers behind SaveLevel and WriteStashedLevel.
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
void DungeonWorld::BeginLevelLoad(const std::string& stem, bool stashCurrent) {
	m_device.WaitIdle(); // the GPU may still be reading the old level's meshes

	// Undo steps snapshot the ACTIVE level's live state, so they are only
	// restorable while that level is live — a transition invalidates them.
	ClearUndoHistory();

	// Save the level we're leaving so a later return restores its fog/progress
	// AND its unsaved edits (skip for a throwaway baseline being replaced by a
	// save's level). The .ent records stash only when they diverged from disk
	// (a prune/re-face edited them) — else the file re-parse is identical.
	// A PARKED level was stashed when the party walked out of it, so it is
	// stashed whatever the caller asked: the callers that pass false believe
	// they are replacing a throwaway baseline, and a parked level is not one.
	// (Not stashed AGAIN: nothing has moved in it since — the party was away.)
	if (stashCurrent && !m_parked) {
		StashActive();
		StashStaticMap();
		if (m_entsDirty)
			m_levelEnts.insert_or_assign(
				m_currentLevel, std::make_unique<DungeonEntities>(m_entities));
	}

	m_parked = false;

	// Move-assign the new level into the existing objects (Party holds a
	// reference to m_map, so the object must persist — only its data changes).
	// A stashed level (this session's unsaved editor edits) takes precedence
	// over the files on disk, layer by layer.
	if (auto it = m_levelMaps.find(stem); it != m_levelMaps.end()) {
		m_map = std::move(*it->second);
		m_levelMaps.erase(it);
	} else {
		m_map = DungeonMap(m_project.LevelMapPath(stem), FixtureTypesOf(m_project));
	}
	if (auto it = m_levelEnts.find(stem); it != m_levelEnts.end()) {
		m_entities = std::move(*it->second);
		m_levelEnts.erase(it);
		m_entsDirty = true; // still diverged from the file; re-stash on leave
	} else {
		m_entities = DungeonEntities(m_project.LevelEntPath(stem), m_map);
		m_entsDirty = false;
	}
	m_currentLevel = stem;

	// Reset per-level state. The shared caches (m_monsterKinds, m_decorationKinds,
	// m_propTextures) persist — they are keyed by name and reused across levels.
	// Instance lists must be cleared (LoadMonsters/LoadDecorations/BuildFires
	// push_back); the surface chunks/blocks/textures self-reset when the caller
	// re-runs AppendLoadTasks.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	m_monsters.clear(); // new monsters get fresh runtimeIds; stale plans find no match
	m_walkableCache.reset(); // force a fresh walkability grid for the new level's map
	m_items.clear();
	m_buttons.clear();
	m_doors.clear();
	m_decorations.clear();
	m_fires.clear();
	m_projectiles.Clear(); // bolts/sparks don't survive a level change
	m_pendingBoltCount = 0; // and any volley still waiting its turn
	m_pendingTransition.reset();
	m_pendingFall.reset(); // the swap IS the fall's end
	m_fallT = -1.0f;
	m_shadows.InvalidateCubes();
	ResolveSurfacePalettes();
}

std::vector<Entity> DungeonWorld::LiveDecorationRecords() const {
	// Live decoration placements as .map records (mirrors the SaveLevel
	// writer's decoration emit; stair props are stairs records).
	std::vector<Entity> records;
	records.reserve(m_decorations.size());
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		Entity e;
		e.kind = EntityKind::Decoration;
		e.type = d.kind->id;
		e.x = d.x;
		e.z = d.z;
		e.facing = d.facing;
		if (d.wallMounted) {
			e.params.emplace_back("wall", DirToken(d.wall));
			if (d.solid) e.params.emplace_back("solid", "1");
		} else if (d.solid != d.kind->solidDefault) {
			e.params.emplace_back("solid", d.solid ? "1" : "0");
		}
		records.push_back(std::move(e));
	}
	return records;
}

void DungeonWorld::StashStaticMap() {
	auto copy = std::make_unique<DungeonMap>(m_map);
	copy->SetDecorationRecords(LiveDecorationRecords());
	// The stash is the STATIC layer, so it keeps the authored fires and niches.
	// What play did to them (a doused torch, a taken one, a niche found) rides
	// the level's dynamic state (SnapshotActive), which every way back in
	// re-applies - and which a new game or a loaded save REPLACES. Left on the
	// copy, a fire doused before a new game came back out in it.
	copy->ResetFixtureBurning();
	copy->ResetNicheOpen();
	m_levelMaps.insert_or_assign(m_currentLevel, std::move(copy));
}

std::optional<DungeonWorld::LevelTransition> DungeonWorld::ConsumeLevelTransition() {
	std::optional<LevelTransition> t = std::move(m_pendingTransition);
	m_pendingTransition.reset();
	return t;
}

namespace {
const char* KindName(EntityKind k) {
	switch (k) {
	case EntityKind::Monster:    return "monster";
	case EntityKind::Button:     return "button";
	case EntityKind::Decoration: return "decoration";
	case EntityKind::Door:       return "door";
	default:                     return "item";
	}
}
} // namespace

// Serializes a level's static layer (.map). `decoLines` carries the decoration
// records, pre-serialized by the caller: the ACTIVE level derives them from
// live instances (SaveLevel), a stashed level already holds them as records
// (WriteStashedLevel). Everything else lives on the DungeonMap.
static std::string SerializeMapStatic(const std::string& stem,
									  const DungeonMap& map,
									  const std::string& decoLines) {
	std::string m = std::format("; {} — written by the in-game editor.\n\n", stem);
	auto palette = [&](const char* surface, const std::vector<std::string>& ids) {
		if (ids.empty()) return;
		m += std::format("palette {}", surface);
		for (const std::string& id : ids) m += " " + id;
		m += '\n';
	};
	palette("wall", map.WallPalette());
	palette("floor", map.FloorPalette());
	palette("ceiling", map.CeilingPalette());
	// The level's tags (DungeonMap::Tags), written beside the palette
	// because they are the same kind of fact: what this level is made of. An
	// untagged level carries no record.
	if (!map.Tags().empty()) {
		m += "tags";
		for (const std::string& tag : map.Tags()) m += " " + tag;
		m += '\n';
	}
	// Per-level atmosphere (the Level settings dialog's mood knobs): only set
	// values are written — an untouched level carries no record and follows
	// the world defaults.
	if (map.DustDensity() >= 0.0f || map.HazeAmbient() >= 0.0f ||
		map.AmbientScale() >= 0.0f) {
		m += "atmosphere";
		if (map.DustDensity() >= 0.0f) m += std::format(" dust={:g}", map.DustDensity());
		if (map.HazeAmbient() >= 0.0f) m += std::format(" haze={:g}", map.HazeAmbient());
		if (map.AmbientScale() >= 0.0f)
			m += std::format(" ambient={:g}", map.AmbientScale());
		m += '\n';
	}
	// The UI material override (DungeonMap::UiStone) - absent unless the level
	// sets its own, so it follows its dungeon's.
	if (!map.UiStone().empty()) m += std::format("uistone {}\n", map.UiStone());
	m += ";\n";

	// Grid: 'P' start, '#' wall, 'D' authored-dusty floor, '.' floor. Fixtures
	// are emitted as records below, so their cells stay plain floor.
	for (int z = 0; z < map.Height(); ++z) {
		std::string row(static_cast<size_t>(map.Width()), '.');
		for (int x = 0; x < map.Width(); ++x) {
			if (x == map.StartX() && z == map.StartZ()) row[x] = 'P';
			else if (map.At(x, z) == Cell::Wall) row[x] = '#';
			else if (map.AuthoredDusty(x, z)) row[x] = 'D';
		}
		m += row;
		m += '\n';
	}
	m += ";\n";

	// The kind token is the instance's fixtures.cat id (the parser fills the
	// default for glyph shorthand, so it is never empty).
	for (const WallSconce& s : map.Sconces()) {
		// The facing only when it names a WALL. A sconce with no solid neighbour
		// (a 'T' glyph in open floor - crypt1 had one; LevelBuildTest plants one
		// back) loads by the glyph rule,
		// which defaults to north without complaint; written back as an explicit
		// `... north` it met the record rule instead, which asserts the wall is
		// there - so one `savemap` of crypt1 made the demo world fatal to load.
		// Without a facing the record takes the glyph's own path, and the
		// round trip is exact.
		const bool walled = !map.IsWalkable(s.x + DirDX(s.wall), s.z + DirDZ(s.wall));
		m += walled ? std::format("fixture {} {} {} {}", s.type, s.x, s.z, DirToken(s.wall))
					: std::format("fixture {} {} {}", s.type, s.x, s.z);
		// Only non-default light/smoke settings are written (keeps the .map minimal).
		if (!s.lit) m += " lit=0";
		if (s.brightness != kSconceBrightness) m += std::format(" bright={:g}", s.brightness);
		if (s.turbidity != kSconceTurbidity) m += std::format(" turb={:g}", s.turbidity);
		m += '\n';
	}
	for (const FloorBrazier& b : map.Braziers()) {
		m += std::format("fixture {} {} {}", b.type, b.x, b.z);
		if (!b.lit) m += " lit=0";
		if (b.brightness != kBrazierBrightness) m += std::format(" bright={:g}", b.brightness);
		if (b.turbidity != kBrazierTurbidity) m += std::format(" turb={:g}", b.turbidity);
		m += '\n';
	}

	for (const WallNiche& n : map.Niches()) {
		m += std::format("niche {} {} {} {}", n.type, n.x, n.z, DirToken(n.wall));
		if (!n.name.empty()) m += std::format(" name={}", n.name); // button target id
		if (n.hidden) m += " hidden=1"; // starts closed (runtime open state = save)
		m += '\n';
	}
	for (const WallBore& b : map.Bores())
		m += std::format("bore {} {} {} {}\n", b.type, b.x, b.z, b.axis);
	for (const SurfaceFeature& f : map.SurfaceFeatures())
		m += std::format("{} {} {} {}\n", f.ceiling ? "ceilingfeature" : "floorfeature",
						 f.type, f.x, f.z);

	// Always the new form, marked so (DungeonMap's constructor reads a file
	// without the marker as the old travel-facing meaning and turns it round).
	m += "stairfacing arrive\n";
	for (const StairLink& s : map.Stairs())
		m += std::format("stairs {} {} {} {} dest={} destx={} destz={}{}\n", s.type, s.x, s.z,
						 DirToken(s.facing), s.destLevel, s.destX, s.destZ,
						 s.flag.empty() ? std::string() : " flag=" + s.flag);

	// A pinned palette index is a `variant`; a theme reference a
	// `theme`, written by the theme's ID (its slot number is this
	// load's bookkeeping, not something a file should depend on).
	static constexpr const char* kSurfaceName[3] = {"wall", "floor", "ceiling"};
	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x)
			for (int s = 0; s < 3; ++s) {
				const int v = map.Variant(static_cast<Surface>(s), x, z);
				if (v >= 0)
					m += std::format("variant {} {} {} {}\n", kSurfaceName[s], x, z, v);
				else if (const int slot = DungeonMap::ThemeSlotOf(v); slot >= 0)
					m += std::format("theme {} {} {} {}\n", kSurfaceName[s], x, z,
									 map.ThemeId(slot));
			}

	m += decoLines;
	return m;
}

// One generic entity record line: kind, type, cell, facing, then the record's
// key=value params verbatim (a stashed record already carries everything).
static std::string SerializeRecord(const char* kind, const Entity& e) {
	std::string line = std::format("{} {} {} {} {}", kind, e.type, e.x, e.z,
								   DirToken(e.facing));
	for (const auto& [k, v] : e.params) line += std::format(" {}={}", k, v);
	line += '\n';
	return line;
}

bool DungeonWorld::SaveLevel() const {
	const std::string m = ActiveMapText();
	const std::string e = ActiveEntText();

	// Both serializers build with '\n'; the line ending is decided once, here at
	// the boundary (serialize::NormalizeEol), so no append site has to know it.
	const std::string mOut = serialize::NormalizeEol(m), eOut = serialize::NormalizeEol(e);
	const bool okMap = assets::WriteBinaryFile(m_project.LevelMapPath(m_currentLevel),
											   mOut.data(), mOut.size());
	const bool okEnt = assets::WriteBinaryFile(m_project.LevelEntPath(m_currentLevel),
											   eOut.data(), eOut.size());
	if (okMap && okEnt)
		log::Info("Saved level {}: {} decorations, {} monsters", m_currentLevel,
				  m_decorations.size(), m_monsters.size());
	else
		log::Warn("Failed to write level {} files", m_currentLevel);
	return okMap && okEnt;
}

void DungeonWorld::LevelTextFor(const std::string& stem, std::string& mapText,
								std::string& entText) const {
	mapText.clear();
	entText.clear();
	if (stem == m_currentLevel) {
		mapText = ActiveMapText();
		entText = ActiveEntText();
		return;
	}
	if (const auto ms = m_levelMaps.find(stem); ms != m_levelMaps.end())
		mapText = StashedMapText(stem, *ms->second);
	if (const auto es = m_levelEnts.find(stem); es != m_levelEnts.end())
		entText = StashedEntText(stem, *es->second);
}

std::string DungeonWorld::StashedMapText(const std::string& stem, const DungeonMap& map) {
	// A stash's decorations are already records (the live-instance sync happens
	// when the map is stashed / remote edits author records directly).
	std::string deco;
	for (const Entity& e : map.Decorations()) deco += SerializeRecord("decoration", e);
	return SerializeMapStatic(stem, map, deco);
}

std::string DungeonWorld::StashedEntText(const std::string& stem, const DungeonEntities& ents) {
	std::string e =
		std::format("; {} — written by the in-game editor (dynamic layer).\n\n", stem);
	for (const Entity& ent : ents.All()) e += SerializeRecord(KindName(ent.kind), ent);
	return e;
}

std::string DungeonWorld::ActiveMapText() const {
	// Decorations reconstructed from the live instances (so editor placements /
	// removals persist); stair props are skipped — they are stairs records.
	std::string deco;
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		if (d.wallMounted) {
			deco += std::format("decoration {} {} {} wall={}", d.kind->id, d.x,
								d.z, DirToken(d.wall));
			if (d.solid) deco += " solid=1";
		} else {
			deco += std::format("decoration {} {} {} {}", d.kind->id, d.x, d.z,
								DirToken(d.facing));
			if (d.solid != d.kind->solidDefault)
				deco += std::format(" solid={}", d.solid ? 1 : 0);
		}
		deco += '\n';
	}
	return SerializeMapStatic(m_currentLevel, m_map, deco);
}

// The active level's dynamic layer as .ent text: the monsters from the LIVE list
// (an editor-placed one has no record, and patrols/overrides are edited live),
// then the record-backed items, buttons and doors. SaveLevel writes it; a level
// resize re-parses it, which is how it gets every monster as it stands.
std::string DungeonWorld::ActiveEntText() const {
	std::string e =
		std::format("; {} — written by the in-game editor (dynamic layer).\n\n", m_currentLevel);
	for (const Monster& mon : m_monsters) {
		e += std::format("monster {} {} {} {}", mon.kind ? mon.kind->name : std::string("?"),
						 mon.x, mon.z, DirToken(mon.facing));
		// Per-instance AI overrides (authored, round-tripped by the editor inspector).
		if (mon.asleep) e += " asleep=1";
		if (mon.leashRange > 0.0f) e += std::format(" leash={:g}", mon.leashRange);
		if (mon.leashX != mon.spawnX || mon.leashZ != mon.spawnZ)
			e += std::format(" leashfrom={},{}", mon.leashX, mon.leashZ);
		// Per-instance behaviour overrides (only when they differ from the type).
		static const char* kArch[] = {"brute",   "skirmisher", "caster",
									  "swarm", "lurker",     "sentry"};
		if (mon.archOverride) e += std::format(" archetype={}", kArch[static_cast<int>(*mon.archOverride)]);
		if (mon.keepOverride) e += std::format(" keeprange={:g}", *mon.keepOverride);
		if (mon.fleeOverride) e += std::format(" fleebelow={:g}", *mon.fleeOverride);
		if (mon.spellOverride && !mon.spellOverride->empty())
			e += std::format(" spell={}", *mon.spellOverride);
		if (!mon.patrol.empty()) {
			e += " patrol=";
			for (size_t k = 0; k < mon.patrol.size(); ++k)
				e += std::format("{}{},{}", k ? ";" : "", mon.patrol[k].x, mon.patrol[k].z);
		}
		e += '\n';
	}
	// Items/buttons/doors are record-backed (placement/erase edits m_entities
	// directly), so their records ARE current.
	for (const Entity& ent : m_entities.All()) {
		if (ent.kind != EntityKind::Item && ent.kind != EntityKind::Button &&
			ent.kind != EntityKind::Door)
			continue;
		e += SerializeRecord(KindName(ent.kind), ent);
	}
	return e;
}

bool DungeonWorld::WriteStashedLevel(const std::string& stem) const {
	const auto ms = m_levelMaps.find(stem);
	if (ms == m_levelMaps.end()) return false;
	const std::string m = serialize::NormalizeEol(StashedMapText(stem, *ms->second));
	bool ok = assets::WriteBinaryFile(m_project.LevelMapPath(stem), m.data(),
									  m.size());

	// The .ent is rewritten only when its records were edited (a stash exists);
	// an untouched dynamic layer keeps its file byte-identical.
	if (const auto es = m_levelEnts.find(stem); es != m_levelEnts.end()) {
		const std::string e = serialize::NormalizeEol(StashedEntText(stem, *es->second));
		ok &= assets::WriteBinaryFile(m_project.LevelEntPath(stem), e.data(),
									  e.size());
	}
	if (ok) log::Info("Saved stashed level {}", stem);
	else log::Warn("Failed to write stashed level {} files", stem);
	return ok;
}

} // namespace dungeon::game
