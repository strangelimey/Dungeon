// ============================================================================
// Game/Game_Generate.cpp — the seam between the level generator and the project.
//
// Game/Generate.h is pure and knows no catalogs, so everything catalog-shaped
// happens here: resolving the level's THEME TAGS into the id pools the generator
// picks from, and turning what it produces back into the ordinary .map/.ent text
// every other level is written as.
//
// The theme lens works exactly as it does in the palette (Catalog.h): an entry
// with no tags fits any theme, and a theme that matches nothing falls back to
// the whole pool rather than generating an empty dungeon — a knob that silently
// produces nothing teaches you to distrust the button, and "no monsters tagged
// undead yet" is a content gap, not a reason to refuse.
//
// TWO ENTRY POINTS, one builder. A NEW level goes through CreateNewLevel
// (Game_Editor.cpp) - the one writer of level files, generated or empty - which
// asks ComposeGeneratedLevel for the text and LinkToFloorAbove for the stair;
// RegenerateViewedLevel replaces the level you are looking at, in place, as one
// undo step. They share BuildLevelText so the same knobs cannot produce two
// different dungeons.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Serialize.h"
#include "Game/GenerateKnobs.h"
#include "Game/Threat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <system_error>

namespace dungeon::game {

namespace {

// Ids from a catalog whose tags match the theme, or ALL of them when nothing
// matches (see the header note).
std::vector<std::string> PoolFor(const Catalog& cat,
								 const std::vector<std::string>& theme) {
	std::vector<std::string> matched, all;
	for (const CatalogEntry& e : cat.Entries()) {
		if (CatalogBool(&e, "hidden", false)) continue;
		all.push_back(e.id);
		if (CatalogMatchesTags(&e, theme)) matched.push_back(e.id);
	}
	return matched.empty() ? all : matched;
}

// Direction order, matching the parser's tokens. A file-local table like the
// editor writer's - there is no shared DirName to call.
constexpr const char* kDir[4] = {"north", "east", "south", "west"};

// The .ent record line for a generated entity. Deliberately the same shape the
// editor's own writer emits — a generated level has to be indistinguishable
// from a hand-built one, or half the tooling stops applying to it.
std::string RecordLine(const Entity& e) {
	const char* kind = e.kind == EntityKind::Monster ? "monster"
					   : e.kind == EntityKind::Item  ? "item"
					   : e.kind == EntityKind::Door  ? "door"
													 : "button";
	std::string line = std::format("{} {} {} {} {}", kind, e.type, e.x, e.z,
								   kDir[static_cast<int>(e.facing)]);
	for (const auto& [k, v] : e.params) line += std::format(" {}={}", k, v);
	return line + "\n";
}

// The generated level as the two files' TEXT. Shared by both entry points so
// they cannot drift into producing different dungeons from the same knobs.
//
// `stairs` are carried across VERBATIM: a regenerated level keeps its links to
// the floors around it (the generator was told to leave their squares open), so
// rerolling a floor never strands the one above.
void BuildLevelText(const std::string& stem, const generate::Level& lv,
					const generate::Params& params, const DungeonMap& donor,
					const std::vector<std::string>& theme,
					std::span<const StairLink> stairs, std::string& map,
					std::string& ent) {
	auto join = [](const std::vector<std::string>& ids) {
		std::string out;
		for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
		return out;
	};
	map = std::format("; {} - generated (seed {}).\n", stem, params.seed);
	map += "palette wall " + join(donor.WallPalette()) + "\n";
	map += "palette floor " + join(donor.FloorPalette()) + "\n";
	map += "palette ceiling " + join(donor.CeilingPalette()) + "\n";
	if (!theme.empty()) map += "theme " + join(theme) + "\n";
	for (const StairLink& st : stairs)
		map += std::format("stairs {} {} {} {} dest={} destx={} destz={} destfacing={}\n",
						   st.type, st.x, st.z, kDir[static_cast<int>(st.facing)],
						   st.destLevel, st.destX, st.destZ,
						   kDir[static_cast<int>(st.destFacing)]);
	map += ";\n";
	for (int z = 0; z < lv.height; ++z) {
		for (int x = 0; x < lv.width; ++x)
			map += !lv.At(x, z)                         ? '#'
				   : (x == lv.startX && z == lv.startZ) ? 'P'
														: '.';
		map += '\n';
	}
	ent = std::format("; {} - generated dynamic layer.\n", stem);
	for (const Entity& e : lv.entities) ent += RecordLine(e);
}

} // namespace

// The reserved stem a random encounter carries. It is NOT a level: no file has
// this name, nothing writes it, and the '~' makes that structural rather than a
// convention — a stem beginning with it cannot be confused with a project level
// however it is passed around.
const char* const kEncounterStem = "~encounter";

bool Game::InEncounter() const { return m_world->CurrentLevel() == kEncounterStem; }

bool Game::StartEncounter(float difficulty, const std::vector<std::string>& tags,
						  u32 seed) {
	// THE AREA DECIDES WHAT YOU MEET. Difficulty scales the density and the
	// size; the terrain's tags pick the pool, so a moor throws undead and a
	// forest throws beasts without either being named here.
	generate::Params p;
	p.seed = seed;
	// Small: an encounter is a fight, not a dungeon. It grows a little with the
	// danger of the ground, which is the only thing that should make one longer.
	p.width = p.height = 12 + static_cast<int>(std::lround(difficulty * 8.0f));
	p.path = 2 + static_cast<int>(std::lround(difficulty * 3.0f));
	p.branches = 0; // strung out rather than a warren: you came to fight
	p.locks = 0;         // nothing to unlock and nowhere to come back to
	// AN AMBUSH WITH NOTHING IN IT IS NOT AN AMBUSH. Density has a floor even
	// on the safest ground, because the road's safety is that an encounter is
	// RARE — the roll already said so — and not that the one you get is empty.
	// A level generated with nothing to meet would read as a bug, and rightly.
	p.difficulty = std::max(difficulty, 0.25f);
	p.reward = difficulty * 0.5f;
	FillPools(p, tags);
	if (p.monsterIds.empty()) {
		log::Warn("encounter: no monsters match {} — nothing to meet",
				  tags.empty() ? std::string("(no tags)") : tags.front());
		return false;
	}

	generate::Level lv = generate::Run(p);
	// AND THERE IS ALWAYS SOMETHING. The generator's density is tuned for a
	// DUNGEON, where an empty room is breathing space between fights; asked for
	// a 13-square encounter at low density it can quite reasonably place none at
	// all, and it did. An encounter is not a place, it is the fight — so if the
	// roll produced no one, put one at the far end. The floor above makes this
	// rare; this makes it impossible.
	if (std::ranges::none_of(lv.entities, [](const Entity& e) {
			return e.kind == EntityKind::Monster;
		})) {
		Entity m;
		m.kind = EntityKind::Monster;
		m.type = p.monsterIds.front();
		m.x = lv.exitX;
		m.z = lv.exitZ;
		lv.entities.push_back(std::move(m));
	}

	std::string map, ent;
	BuildLevelText(kEncounterStem, lv, p, m_world->Map(), tags, {}, map, ent);
	// THE WAY OUT, authored onto the arrival cell. An encounter is left the same
	// way a dungeon is — by an exit stair — rather than by some second mechanism
	// that would then need its own rules about when it is allowed.
	map += std::format("stairs stairs_exit {} {} south dest=- destx=0 destz=0\n",
					   lv.startX, lv.startZ);

	if (!m_world->InstallLevelFromText(kEncounterStem, map, ent)) {
		log::Warn("encounter: could not install the generated level");
		return false;
	}
	m_worldState.onWorldMap = false;
	m_worldState.atLocation.clear(); // came from open ground, not a doorway
	m_state = AppState::Playing;
	m_ui.ResetHudStatus();
	if (m_world->onMessage) m_world->onMessage(loc::View("world.ambush"));
	log::Info("encounter: {}x{}, difficulty {:.2f}, seed {}, {} monster kinds",
			  p.width, p.height, difficulty, seed, p.monsterIds.size());
	return true;
}

void Game::FillPools(generate::Params& params,
					 const std::vector<std::string>& theme) {
	params.monsterIds = PoolFor(m_project.monsters, theme);
	// Each one's threat, from its stats (Game/Threat.h): what difficulty ranks.
	params.monsterThreat.clear();
	for (const std::string& id : params.monsterIds)
		params.monsterThreat.push_back(
			threat::Of(*m_project.monsters.Find(id)).threat);
	params.lootIds = PoolFor(m_project.items, theme);
	// Keys are the one pool that is NOT themed: a lock needs a key that exists,
	// and which key it is matters far less than that the pair is coherent. An
	// empty pool simply means no locks get authored (Generate.h clamps to it).
	params.keyIds.clear();
	for (const CatalogEntry* e : m_project.AllItems())
		if (e && e->Get("category", "") == "key") params.keyIds.push_back(e->id);
	// Loot must not hand out the keys as treasure — that would let a key turn up
	// behind its own door, precisely the fault the construction order prevents.
	std::erase_if(params.lootIds, [&](const std::string& id) {
		return std::find(params.keyIds.begin(), params.keyIds.end(), id) !=
			   params.keyIds.end();
	});
}

std::vector<std::pair<int, int>>
Game::ComposeGeneratedLevel(const std::string& stem, generate::Params params,
							const std::vector<std::string>& theme, std::string& map,
							std::string& ent) {
	FillPools(params, theme);
	const generate::Level lv = generate::Run(params);
	m_lastGenReport = lv.report;
	// The palette: the level CHOSEN in the dialog (P4b), else the active one.
	// Read at once - MapOf may parse a stash, and nothing else touches the
	// stashes between here and the text being built.
	BuildLevelText(stem, lv, params, PaletteDonor(params.palette, m_world->Map()), theme,
				   {}, map, ent);
	// (The caller sets params.entry to the floor above's stair square, so the
	// start comes first below and the link lands there.)
	// Where the stair from the floor above may land, best first: the generated
	// start (arriving there is what a player expects), then any floor cell.
	std::vector<std::pair<int, int>> cells{{lv.startX, lv.startZ}};
	for (int z = 1; z < lv.height - 1; ++z)
		for (int x = 1; x < lv.width - 1; ++x)
			if (lv.At(x, z) && (x != lv.startX || z != lv.startZ))
				cells.push_back({x, z});
	return cells;
}

void Game::ShowGenReport(const std::string& levelStem) {
	const generate::Report& r = m_lastGenReport;
	m_generateDialog.SetReport(
		{loc::Format("map.gen.report.shape", r.pathGot, r.pathWanted, r.branchesGot,
					 r.branchesWanted),
		 loc::Format("map.gen.report.complexity", r.loopsGot, r.loopsWanted,
					 r.deadEndsGot, r.deadEndsWanted, r.windingGot, r.corridors,
					 r.irregularGot),
		 loc::Format("map.gen.report.content", r.locksGot, r.locksWanted, r.monsters,
					 r.loot),
		 r.monsters == 0
			 ? std::string()
			 : loc::Format("map.gen.report.threat", std::format("{:.1f}", r.threatMin),
						   std::format("{:.1f}", r.threatMax),
						   loc::Tr(r.bossPlaced ? "map.gen.report.boss" : "map.gen.report.noboss"))},
		levelStem);
}

const DungeonMap& Game::PaletteDonor(const std::string& chosen,
									 const DungeonMap& fallback) {
	// Only a level the project KNOWS: a stale choice (a renamed or deleted
	// level still in a preset) falls back rather than aborting on a missing
	// file, which is what parsing an unknown stem would do.
	if (!chosen.empty() && std::find(m_project.levels.begin(), m_project.levels.end(),
									 chosen) != m_project.levels.end())
		return m_world->MapOf(chosen);
	return fallback;
}

// --- presets (P4b) -------------------------------------------------------------
// A PRESET IS A RECIPE, NOT A LEVEL: the settings line minus the seed, so
// loading one keeps the seed you are on and "the labyrinth recipe" makes a new
// labyrinth each roll. Stored in the project (catalog/genpresets.cat), since a
// world is a project and its recipes should travel with it.
std::vector<std::string> Game::GenPresetNames() const {
	std::vector<std::string> out;
	for (const CatalogEntry& e : m_project.genpresets.Entries()) out.push_back(e.id);
	return out;
}

bool Game::LoadGenPreset(const std::string& name, generate::Params& params) const {
	const CatalogEntry* e = m_project.genpresets.Find(name);
	if (!e) return false;
	generate::Decode(e->Get("knobs", ""), params);
	return true;
}

std::string Game::SaveGenPreset(std::string name, const generate::Params& params) {
	// Records are whitespace-tokenised and ids are [A-Za-z0-9_-] everywhere
	// else in the project; anything else becomes an underscore rather than a
	// refusal, since the name is only a label.
	for (char& c : name)
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
	if (name.empty()) return {};
	std::string knobs;
	for (const std::string& pair : SplitKnobs(generate::Encode(params)))
		if (!pair.starts_with("seed:")) knobs += (knobs.empty() ? "" : " ") + pair;
	CatalogEntry entry;
	if (const CatalogEntry* old = m_project.genpresets.Find(name)) entry = *old; // keep display
	entry.id = name;
	entry.Set("knobs", knobs);
	m_project.genpresets.Add(std::move(entry));
	return m_project.Save() ? name : std::string();
}

bool Game::DeleteGenPreset(const std::string& name) {
	if (!m_project.genpresets.Find(name)) return false;
	m_project.genpresets.Remove(name);
	return m_project.Save();
}

std::vector<std::string> Game::SplitKnobs(const std::string& line) {
	std::vector<std::string> out;
	std::string word;
	for (const char c : line) {
		if (c == ' ') {
			if (!word.empty()) out.push_back(std::move(word));
			word.clear();
		} else {
			word += c;
		}
	}
	if (!word.empty()) out.push_back(std::move(word));
	return out;
}

std::string Game::GenReportText() const {
	const generate::Report& r = m_lastGenReport;
	std::string lengths;
	for (const int n : r.branchRooms)
		lengths += (lengths.empty() ? "" : " ") + std::to_string(n);
	return std::format("path {}/{} rooms, branches {}/{} (rooms: {}), loops {}/{}, "
					   "dead ends {}/{}, winding {}/{} corridors, irregular {} rooms, "
					   "locks {}/{}, {} monsters, {} loot, threat {:.2f}-{:.2f}, boss {}",
					   r.pathGot, r.pathWanted, r.branchesGot, r.branchesWanted,
					   lengths.empty() ? "-" : lengths, r.loopsGot, r.loopsWanted,
					   r.deadEndsGot, r.deadEndsWanted, r.windingGot, r.corridors,
					   r.irregularGot, r.locksGot, r.locksWanted, r.monsters, r.loot,
					   r.threatMin, r.threatMax, r.bossPlaced ? "yes" : "no");
}

bool Game::LinkToFloorAbove(const std::string& stem,
							const std::vector<std::pair<int, int>>& cells) {
	// Without a stair the new level is unreachable, and the checker rightly says
	// so. The pair is authored through AddStairAt so it matches what the EDITOR
	// would write — each side arriving on its counterpart. Phase 3 found five
	// hand-made stairs that do not match that, and a generator is the last thing
	// that should manufacture more of them.
	//
	// The floor above is the one before `stem` in its DUNGEON, not in the
	// project's flat list: that list interleaves dungeons, and linking to its
	// previous entry once joined a new crypt floor to the harness's arena.
	const CatalogEntry* dungeon = m_project.DungeonOfLevel(stem);
	if (!dungeon) return false; // an orphan level has no floor above
	const std::vector<std::string> floors = m_project.DungeonLevels(dungeon->id);
	const auto it = std::find(floors.begin(), floors.end(), stem);
	if (it == floors.end() || it == floors.begin()) return false; // the top floor
	const std::string& prev = *(it - 1);

	std::string downType;
	for (const CatalogEntry& e : m_project.stairs.Entries())
		if (!CatalogBool(&e, "up", false) && CatalogBool(&e, "traverse", true)) {
			downType = e.id;
			break;
		}
	// AddStairAt puts both halves on the SAME cell, so the link needs a square
	// that suits BOTH levels.
	if (!downType.empty())
		for (const auto& [x, z] : cells)
			if (m_world->CellFreeForStair(prev, x, z) &&
				m_world->CellFreeForStair(stem, x, z) &&
				m_world->AddStairAt(prev, downType, x, z))
				return true;
	log::Warn("new level: {} has no stair from {} - no cell suits both", stem, prev);
	return false;
}

bool Game::BuildAndInstall(const std::string& stem, const generate::Params& params,
						   const std::vector<std::string>& theme,
						   const DungeonMap& donor, std::span<const StairLink> stairs) {
	generate::Params p = params;
	FillPools(p, theme);
	const generate::Level lv = generate::Run(p);
	m_lastGenReport = lv.report;

	std::string map, ent;
	BuildLevelText(stem, lv, p, donor, theme, stairs, map, ent);

	// Parsed through a TEMP file rather than the level's own, so the real files
	// stay untouched until `savemap` — which is how every other editor edit
	// behaves, and what keeps an undo COMPLETE instead of leaving the generated
	// version on disk after the reroll has been taken back. (DungeonMap only
	// constructs from a path; giving it a text constructor for this one caller
	// would be the larger change.)
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec) / "dungeon-gen";
	fs::create_directories(dir, ec);
	const std::string mapPath = (dir / (stem + ".map")).string();
	const std::string entPath = (dir / (stem + ".ent")).string();
	const std::string mapOut = serialize::NormalizeEol(map);
	const std::string entOut = serialize::NormalizeEol(ent);
	if (!assets::WriteBinaryFile(mapPath, mapOut.data(), mapOut.size()) ||
		!assets::WriteBinaryFile(entPath, entOut.data(), entOut.size())) {
		log::Warn("generate: could not stage {} for parsing", stem);
		return false;
	}
	const bool ok = m_world->InstallLevelFromFiles(stem, mapPath, entPath);
	fs::remove(mapPath, ec);
	fs::remove(entPath, ec);
	return ok;
}

bool Game::RegenerateViewedLevel(generate::Params params) {
	const std::string stem = m_mapView.ViewedLevel();
	if (stem.empty()) return false;
	const DungeonMap& viewed = m_mapView.ViewedMap();
	// THE LEVEL KEEPS ITS STAIRS. Their far ends live on the neighbouring floors,
	// which a reroll of this one has no business moving - so each stair's square
	// is handed to the generator to keep open. The one leading UP to the floor
	// above is the ENTRY (the start, and the root of the layout); every other is
	// kept open and joined on. Without this, the create-then-regenerate loop
	// stranded a fresh floor on its very first reroll.
	const std::vector<StairLink> stairs = viewed.Stairs();
	std::string above;
	if (const CatalogEntry* d = m_project.DungeonOfLevel(stem)) {
		const std::vector<std::string> floors = m_project.DungeonLevels(d->id);
		const auto it = std::find(floors.begin(), floors.end(), stem);
		if (it != floors.end() && it != floors.begin()) above = *(it - 1);
	}
	const auto entry = std::find_if(stairs.begin(), stairs.end(),
									[&](const StairLink& s) {
										return !above.empty() && s.destLevel == above;
									});
	for (auto it = stairs.begin(); it != stairs.end(); ++it) {
		const bool isEntry = entry != stairs.end() ? it == entry : it == stairs.begin();
		if (isEntry) {
			params.entryX = it->x;
			params.entryZ = it->z;
		} else {
			params.keepOpen.push_back({it->x, it->z});
		}
	}
	// ONE undo step, and no level transition — see the declaration in Game.h.
	// The level is its own palette donor: a reroll changes the shape, not the
	// look (it used to take the ACTIVE level's, so rerolling a browsed floor
	// quietly re-skinned it).
	m_world->BeginUndoStep();
	// A theme or palette CHOSEN in the dialog (P4b) replaces the level's own;
	// empty keeps it, which is what a reroll did before there was a choice.
	const std::vector<std::string> theme =
		!params.theme.empty() ? std::vector<std::string>{params.theme} : viewed.Theme();
	const bool ok = BuildAndInstall(stem, params, theme,
									PaletteDonor(params.palette, viewed), stairs);
	m_world->CommitUndoStep(ok);
	return ok;
}

} // namespace dungeon::game
