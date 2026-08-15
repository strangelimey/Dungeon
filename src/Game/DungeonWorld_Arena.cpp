// ============================================================================
// Game/DungeonWorld_Arena.cpp — the eval harness's controlled space.
//
// Every attempt to verify the harness against the SHOWCASE level fought back: a
// monster already standing adjacent so it had nothing to walk toward, a target
// cell that turned out not to be walkable, one creature that would not engage in
// either AI mode. None of those are bugs — they are a hand-authored level being
// a hand-authored level — but they make it impossible to say what a measurement
// measured. An arena is a room whose entire contents you chose.
//
// IT WRITES NO FILES. The editor's "new level" button authors a .map/.ent pair
// and appends the project manifest, which in a dev build lands straight in the
// git tree (paths::Asset IS the source tree — see CLAUDE.md). An eval that
// dirtied the working copy on every run would be intolerable, so this carves the
// arena into the LOADED map instead and leaves the files alone. Nothing persists
// unless someone types `savemap`, which an eval script never does.
//
// The shapes are not arbitrary: they are the geometries a propagating blast has
// to be measured in (docs/damage-system.md "The area blast"). An open room lets
// a wavefront ring outward; a corridor makes it reflect off both ends; a dead
// end reflects it back onto the caster; a T-junction is where arms converge and
// MULTIPLY, which is where the worst number in the game lives.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Log.h"

namespace dungeon::game {

namespace {
// A blast's own geometry is what these exist to exercise, so the corridor width
// is 1 BY DEFINITION — a two-wide "corridor" is a room, and a wavefront in it
// does something else entirely.
constexpr int kCorridorWidth = 1;
} // namespace

bool DungeonWorld::BuildArena(ArenaShape shape, int w, int h, ArenaInfo& out) {
	const int W = m_map.Width(), H = m_map.Height();
	if (w < 1 || h < 1) return false;
	// Leave a solid border: a floor cell on the very edge would have no wall
	// beyond it for a blast to reflect off, and the whole point of the corridor
	// and dead-end shapes is what happens at the end.
	if (w > W - 2 || h > H - 2) {
		log::Warn("arena: {}x{} does not fit in a {}x{} map (needs a solid border)",
				  w, h, W, H);
		return false;
	}

	// --- 1. empty the world -------------------------------------------------
	// Everything dynamic goes. A leftover monster from the authored level would
	// wander into the measurement, and a leftover item would sit in a wall.
	m_monsters.clear();
	m_items.clear();
	m_decorations.clear();
	m_doors.clear();
	m_buttons.clear();
	m_projectiles.Clear();

	// --- 2. strip the map's own furniture -----------------------------------
	// Per-cell removers rather than reaching into the vectors: they are the
	// tested paths, and they keep the derived side-tables (fixture breakables,
	// niche records) consistent with what they remove.
	for (int z = 0; z < H; ++z)
		for (int x = 0; x < W; ++x) {
			while (m_map.RemoveFixtureAt(x, z)) {} // several can share a cell
			m_map.RemoveStair(x, z);
			m_map.RemoveAnyFeature(x, z);
			m_map.RemoveBoreAt(x, z);
			m_map.RemoveDecorationRecordAt(x, z);
			for (int d = 0; d < 4; ++d)
				m_map.RemoveNiche(x, z, static_cast<Direction>(d));
		}

	// --- 3. solid rock, then carve ------------------------------------------
	for (int z = 0; z < H; ++z)
		for (int x = 0; x < W; ++x) m_map.SetCell(x, z, Cell::Wall);

	// Centred on the map, so the arena's cells are PREDICTABLE from the map size
	// alone. A script has no way to read this function's return value — it reads
	// a log line at best — so the coordinates have to be derivable, not reported.
	const int cx = W / 2, cz = H / 2;
	const auto carve = [&](int x0, int z0, int x1, int z1) {
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x)
				if (x > 0 && z > 0 && x < W - 1 && z < H - 1)
					m_map.SetCell(x, z, Cell::Floor);
	};

	switch (shape) {
	case ArenaShape::Open:
		out = {cx - w / 2, cz - h / 2, cx - w / 2 + w - 1, cz - h / 2 + h - 1, cx, cz};
		carve(out.x0, out.z0, out.x1, out.z1);
		break;
	case ArenaShape::Corridor:
		// `w` is the LENGTH; a corridor's width is not a parameter (see above).
		out = {cx - w / 2, cz, cx - w / 2 + w - 1, cz + kCorridorWidth - 1, cx, cz};
		carve(out.x0, out.z0, out.x1, out.z1);
		break;
	case ArenaShape::DeadEnd:
		// The same strip, but the centre reported is the CLOSED END rather than
		// the middle — a dead end is not a different shape from a corridor, it is
		// a different place to stand in one, and what a script needs from it is
		// the cell with rock behind it.
		out = {cx, cz, cx + w - 1, cz + kCorridorWidth - 1, cx, cz};
		carve(out.x0, out.z0, out.x1, out.z1);
		break;
	case ArenaShape::Room: {
		// THE LADDER'S ARENA (Michael's protocol): a room with a corridor coming
		// off it. The monster waits at the room's centre; the party starts at the
		// FAR END of the corridor and walks in. The distance is the point — an
		// encounter that begins with the two sides already adjacent skips the
		// approach, and the approach is where the monster notices, closes, and
		// the corridor decides how many of them can reach you at once.
		//
		// `w` is the room's side, `h` the corridor's length. The corridor runs
		// SOUTH (+z) from the middle of the room's south wall.
		const int half = w / 2;
		const int roomZ1 = cz + half;
		const int endZ = std::min(H - 2, roomZ1 + h);
		out = {cx - half, cz - half, cx + half, endZ, cx, cz};
		carve(cx - half, cz - half, cx + half, roomZ1); // the room
		carve(cx, roomZ1 + 1, cx, endZ);               // the corridor
		out.sx = cx;
		out.sz = endZ; // the far end: the party's start
		break;
	}
	case ArenaShape::TJunction: {
		// A bar with a stem meeting it at the middle. The junction cell is the
		// centre, because that is the square a blast's arms reflect back onto and
		// multiply — the worst case the fireburst tuning was pinned against.
		const int arm = w / 2;
		out = {cx - arm, cz, cx + arm, cz + h - 1, cx, cz};
		carve(cx - arm, cz, cx + arm, cz);              // the bar
		carve(cx, cz, cx, cz + std::max(1, h - 1));     // the stem
		break;
	}
	}

	// --- 4. the party, and a clean slate around them ------------------------
	// Every shape but Room starts the party ON the centre; Room puts them at
	// the far end of its corridor, which is set in the case above.
	if (shape != ArenaShape::Room) { out.sx = out.cx; out.sz = out.cz; }
	m_map.SetStart(out.sx, out.sz);
	m_party.Reset(out.sx, out.sz);
	// The whole arena revealed: fog is a PLAYER concern, and an eval that could
	// not see what it built would be reading a blank map overlay.
	m_seen.assign(static_cast<size_t>(W) * H, 0);
	for (int z = out.z0; z <= out.z1; ++z)
		for (int x = out.x0; x <= out.x1; ++x) MarkSeen(x, z);
	MarkSeen(out.cx, out.cz);
	MarkSeen(out.sx, out.sz); // the corridor end, outside the room's bounds

	// --- 5. make it real ----------------------------------------------------
	// The full bake, not RebuildChunksAround: every cell in the map changed, and
	// the chunk-local path exists for a brush stroke.
	BuildDungeonMeshes();
	RebuildFiresAndDust(); // no fires left — this clears their light and smoke
	// The world the one-pipeline check was watching is gone (Game/DamageLedger.h).
	RebaseDamageLedger();
	return true;
}

bool DungeonWorld::DetonateSpell(std::string_view spellId, int cx, int cz) {
	const Spell* spell = m_magic.FindSpell(spellId);
	if (!spell) {
		log::Warn("blast: no spell '{}'", spellId);
		return false;
	}
	if (!spell->Blast().Any()) {
		// Refused rather than detonated as a nothing, because "the spell is not
		// an area effect" and "the blast reached nobody" produce the same empty
		// table and mean opposite things.
		log::Warn("blast: spell '{}' has no blast (needs blast_force)", spellId);
		return false;
	}
	// The spell's own payload, unchanged: its blast rules, its on-hit procs and
	// its school's damage type. No caster (-1) — nothing here is credited with
	// threat, and a blast has no side anyway.
	ProjectilePayload payload;
	payload.blast = spell->Blast();
	for (const fx::Proc& p : spell->Procs()) {
		if (payload.count >= payload.procs.size()) break;
		payload.procs[payload.count++] = p;
	}
	payload.flavour = spell->School(); // its burn arrives in the school's colours
	Detonate(cx, cz, payload, m_damageTypes.ForSchool(spell->School()), -1);
	return true;
}

bool DungeonWorld::ArenaShapeFromName(std::string_view name, ArenaShape& out) {
	if (name == "open") { out = ArenaShape::Open; return true; }
	if (name == "corridor") { out = ArenaShape::Corridor; return true; }
	if (name == "deadend") { out = ArenaShape::DeadEnd; return true; }
	if (name == "tjunction") { out = ArenaShape::TJunction; return true; }
	if (name == "room") { out = ArenaShape::Room; return true; }
	return false;
}

} // namespace dungeon::game
