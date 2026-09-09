// ============================================================================
// Game/Game_World.cpp — the Game side of the world tier (docs/world-map.md).
//
// Two jobs, both small on purpose: resolve terrain.cat into the rules WorldMap
// parses against, and report what was loaded for the `world` dev command.
//
// The resolution lives HERE rather than in WorldMap because WorldMap is free of
// Project by design — it parses and answers questions and can be tested without
// a project, exactly as DungeonMap takes a resolved FixtureTypes rather than
// reaching for the catalogs itself. This file is the seam where catalog data
// becomes map rules, and it is the only place that knows both.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"

#include <format>

namespace dungeon::game {

namespace {

// A terrain kind's glyph, as authored. Exactly one character: a glyph IS one
// grid cell, so "" or "MM" is an authoring error rather than something to
// interpret generously.
char GlyphOf(const CatalogEntry& e) {
	const std::string g = e.Get("glyph", "");
	if (g.size() == 1) return g[0];
	log::Warn("terrain '{}' has {} glyph — one character required, using '?'",
			  e.id, g.empty() ? "no" : "a multi-character");
	return '?';
}

// How many space-separated stems a dungeon's `levels` field names.
size_t WordCount(const std::string& s) {
	size_t n = 0;
	bool inWord = false;
	for (const char c : s) {
		const bool space = c == ' ' || c == '	';
		if (!space && !inWord) ++n;
		inWord = !space;
	}
	return n;
}

} // namespace

void Game::LoadWorldMap() {
	WorldMap::TerrainRules rules;
	rules.reserve(m_project.terrain.Entries().size());
	for (const CatalogEntry& e : m_project.terrain.Entries()) {
		WorldMap::Terrain t;
		t.id = e.id;
		t.glyph = GlyphOf(e);
		t.passable = e.GetBool("passable", true);
		t.travel = e.GetFloat("travel", 1.0f);
		t.difficulty = e.GetFloat("difficulty", 0.0f);
		t.tags = CatalogTags(&e);
		rules.push_back(std::move(t));
	}

	if (rules.empty()) {
		// No terrain authored means no world can be read, and that is a normal
		// state for a project that has not grown one yet — not a failure.
		log::Info("No terrain.cat entries: project '{}' has no world map",
				  m_project.name);
		return;
	}

	m_worldMap = WorldMap::Load(m_project.WorldMapPath(), std::move(rules));
	if (!m_worldMap) {
		log::Info("Project '{}' has no world/world.map", m_project.name);
		return;
	}
	log::Info("Loaded world map: {}x{}, {} terrain kinds, {} areas, {} locations",
			  m_worldMap->Width(), m_worldMap->Height(),
			  m_worldMap->Terrains().size(), m_worldMap->Areas().size(),
			  m_worldMap->Locations().size());
}

std::vector<validate::Issue> Game::ValidateProject() {
	// The world tier's half of the snapshot. Gathered HERE because Game is the
	// only thing that holds both the world map and the catalogs; the checker
	// stays free of Project and DungeonWorld stays free of the world.
	validate::WorldView view;
	view.map = m_worldMap ? &*m_worldMap : nullptr;
	for (const CatalogEntry& e : m_project.dungeons.Entries()) {
		validate::DungeonView d;
		d.id = e.id;
		d.levels = ParseTags(e.Get("levels", "")); // space-split + lowercased
		d.entry = e.Get("entry", "");
		view.dungeons.push_back(std::move(d));
	}
	return m_world.Validate(view);
}

std::vector<std::string> Game::WorldReport() const {
	std::vector<std::string> out;
	if (!m_worldMap) {
		out.push_back("no world map loaded (project has no world/world.map)");
		return out;
	}
	const WorldMap& w = *m_worldMap;

	// Count the cells of each kind while walking the grid once. A terrain kind
	// authored but never placed is worth seeing — it is the shape a typo in a
	// glyph takes.
	std::vector<int> cells(w.Terrains().size(), 0);
	int passable = 0;
	for (int z = 0; z < w.Height(); ++z)
		for (int x = 0; x < w.Width(); ++x) {
			const WorldMap::Terrain& t = w.TerrainAt(x, z);
			for (size_t i = 0; i < w.Terrains().size(); ++i)
				if (w.Terrains()[i].id == t.id) {
					++cells[i];
					break;
				}
			if (t.passable) ++passable;
		}

	out.push_back(std::format("{}x{} world, start {},{}, {} of {} cells passable",
							  w.Width(), w.Height(), w.StartX(), w.StartZ(),
							  passable, w.Width() * w.Height()));
	for (size_t i = 0; i < w.Terrains().size(); ++i) {
		const WorldMap::Terrain& t = w.Terrains()[i];
		out.push_back(std::format(
			"  terrain {:<10} '{}' {:>5} cells  travel {:.2f}h  difficulty {:.2f}{}",
			t.id, t.glyph, cells[i], t.travel, t.difficulty,
			t.passable ? "" : "  (impassable)"));
	}
	for (const WorldMap::Area& a : w.Areas())
		out.push_back(std::format("  area    {:<10} {},{} {}x{}  difficulty {}",
								  a.id, a.x, a.z, a.w, a.h,
								  a.difficulty >= 0.0f
									  ? std::format("{:.2f}", a.difficulty)
									  : std::string("(terrain's own)")));
	for (const WorldMap::Location& l : w.Locations()) {
		// Report the location against the DUNGEON it names, since a location
		// pointing at nothing is the fault most worth seeing here.
		const CatalogEntry* d = m_project.dungeons.Find(l.id);
		out.push_back(std::format(
			"  {:<7} {:<10} {},{}  on {}  difficulty {:.2f}  -> {}", l.kind, l.id,
			l.x, l.z, w.TerrainAt(l.x, l.z).id, w.Difficulty(l.x, l.z),
			d ? std::format("{} level(s), entry {}",
							WordCount(d->Get("levels", "")),
							d->Get("entry", "(first)"))
			  : std::string("NO SUCH DUNGEON")));
	}
	return out;
}

} // namespace dungeon::game
