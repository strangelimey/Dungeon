// ============================================================================
// Game/Game_Populate.cpp - the workflow, wired through (docs/tool-refinement-
// plan.md Phase 7): a style opening a new level, and POPULATE ONLY.
//
// The four stages are World, Build, Furnishings, Populate. Phases 1-6 made the
// pieces; this file is the seams between them that were still a dialog or a
// guess: which style a new level opens on, where the editor leaves you once it
// is made, and filling a level that is already built with what lives there -
// however it was built, so a hand-carved floor can be populated as readily as a
// generated one, and then adjusted by hand.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/DungeonEntities.h"
#include "Game/GenerateKnobs.h"
#include "Game/StyleLook.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <set>

namespace dungeon::game {

bool Game::LoadStyleKnobs(const std::string& id, generate::Params& params) const {
	const CatalogEntry* style = stylelook::Find(m_project, id);
	if (!style) return false;
	const u32 seed = params.seed; // a style is a recipe, not a roll (the preset rule)
	generate::Decode(style->Get("knobs", ""), params);
	params.seed = seed;
	params.style = id;
	return true;
}

std::vector<std::string> Game::TagsFor(const generate::Params& params, const Project& project,
									   std::vector<std::string> fallback) const {
	if (!params.tag.empty()) return {params.tag};
	if (const CatalogEntry* style = stylelook::Find(project, params.style))
		if (std::vector<std::string> tags = stylelook::Tags(*style); !tags.empty()) return tags;
	return fallback;
}

std::string Game::DefaultStyleFor(const std::string& dungeonId) const {
	if (const CatalogEntry* d = m_project.dungeons.Find(dungeonId))
		if (const std::string s = d->Get("style", ""); m_project.styles.Contains(s)) return s;
	const std::string& armed = m_mapEditor.CurrentStyle();
	return m_project.styles.Contains(armed) ? armed : std::string();
}

void Game::LandInBuild(const std::string& stem, const std::string& style) {
	m_mapView.SetViewLevel(stem);
	// The guided order is the Stage grouping; Build is its second group. Found
	// by NAME, so regrouping the table cannot quietly land you somewhere else.
	m_mapEditor.SetPaletteGrouping(MapEditor::Grouping::Stage);
	for (int g = 0; g < MapEditor::GroupCount(MapEditor::Grouping::Stage); ++g)
		if (std::string_view(MapEditor::GroupName(MapEditor::Grouping::Stage, g)) == "build")
			m_mapEditor.SetActiveGroup(g);
	if (!style.empty()) m_mapEditor.SetCurrentStyle(style);
}

int Game::PopulateViewedLevel(const generate::Params& in) {
	if (!m_world) return -1;
	const std::string stem = m_mapView.ViewedLevel();
	if (stem.empty()) return -1;
	// The level as it stands - unsaved edits and all - else as on disk.
	std::string mapText, entText;
	m_world->LevelTextFor(stem, mapText, entText);
	const auto readFile = [](const std::string& path) {
		const auto bytes = assets::ReadBinaryFile(path);
		return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
	};
	if (mapText.empty()) mapText = readFile(m_project.LevelMapPath(stem));
	if (entText.empty()) entText = readFile(m_project.LevelEntPath(stem));
	if (mapText.empty()) {
		log::Warn("populate: {} has no map to populate", stem);
		return -1;
	}
	const DungeonMap map = DungeonMap::FromText(mapText, DungeonWorld::FixtureTypesOf(m_project), stem);
	DungeonEntities ents = DungeonEntities::FromText(entText, map, stem);

	// The pools: the style's monsters when it names any, the tags' otherwise.
	generate::Params p = in;
	std::vector<std::string> fallback = map.Tags();
	if (fallback.empty())
		if (const CatalogEntry* d = m_project.DungeonOfLevel(stem))
			fallback = ParseTags(d->Get("tags", ""));
	FillPools(p, TagsFor(p, m_project, fallback));
	// A QUEST ITEM is never loot: it is placed where its story wants it, by hand,
	// and populating must neither scatter one nor take one away.
	std::erase_if(p.lootIds, [&](const std::string& id) {
		return DungeonWorld::IsQuestItem(m_project.FindItem(id));
	});

	// WHAT POPULATING REPLACES is exactly what it can make: monsters of the
	// pool's kinds and loot of the loot pool's. A key, a quest item, a hand-placed
	// monster of another kind - all stay, so populating twice rerolls the
	// populated part and nothing else.
	const std::set<std::string> monsterKinds(p.monsterIds.begin(), p.monsterIds.end());
	const std::set<std::string> lootKinds(p.lootIds.begin(), p.lootIds.end());
	std::vector<int> replaced;
	for (const Entity& e : ents.All())
		if ((e.kind == EntityKind::Monster && monsterKinds.count(e.type)) ||
			(e.kind == EntityKind::Item && lootKinds.count(e.type)))
			replaced.push_back(e.id);
	for (const int id : replaced) ents.RemoveById(id);

	// Where content may stand: walkable, and not a stair, a brazier, a floor
	// decoration or anything still there - nor within two steps of any stair, the
	// generator's rule that you do not arrive into a fight you could not see.
	const int w = map.Width(), h = map.Height();
	std::vector<u8> walkable(static_cast<size_t>(w) * h, 0), free(walkable.size(), 0);
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x)
			if (map.IsWalkable(x, z)) {
				walkable[static_cast<size_t>(z) * w + x] = 1;
				free[static_cast<size_t>(z) * w + x] =
					!map.StairAt(x, z) && !map.BrazierAt(x, z) && ents.At(x, z).empty();
			}
	for (const Entity& d : map.Decorations()) {
		const bool onWall = std::any_of(d.params.begin(), d.params.end(),
										[](const auto& kv) { return kv.first == "wall"; });
		if (!onWall && d.x >= 0 && d.z >= 0 && d.x < w && d.z < h)
			free[static_cast<size_t>(d.z) * w + d.x] = 0;
	}
	for (const StairLink& s : map.Stairs())
		for (int z = s.z - 2; z <= s.z + 2; ++z)
			for (int x = s.x - 2; x <= s.x + 2; ++x)
				if (x >= 0 && z >= 0 && x < w && z < h && std::abs(x - s.x) + std::abs(z - s.z) <= 2)
					free[static_cast<size_t>(z) * w + x] = 0;

	const generate::Level lv =
		generate::Populate(p, w, h, walkable, free, map.StartX(), map.StartZ());
	m_lastGenReport = lv.report;
	for (const Entity& e : lv.entities) ents.Add(e);

	// ONE undo step, the reroll's: the level goes back in whole. The ACTIVE
	// level's install puts the party on the start square (a regenerate may have
	// made its old one rock); populating changes no square, so it goes back.
	Party& party = m_world->GetParty();
	const bool active = stem == m_world->CurrentLevel();
	const int px = party.GridX(), pz = party.GridZ(), pf = party.Facing();
	m_world->BeginUndoStep();
	const bool ok = InstallLevelText(stem, mapText, DungeonWorld::EntTextOf(stem, ents));
	m_world->CommitUndoStep(ok);
	if (!ok) return -1;
	if (active && map.IsWalkable(px, pz)) {
		party.SetGridPosition(px, pz);
		party.SetFacing(pf);
	}
	log::Info("populate {}: {} monsters, {} loot, {} replaced (style '{}', {} kinds)", stem,
			  lv.report.monsters, lv.report.loot, replaced.size(), p.style, p.monsterIds.size());
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.gen.populated", stem, lv.report.monsters,
										   lv.report.loot, replaced.size()));
	return lv.report.monsters;
}

} // namespace dungeon::game
