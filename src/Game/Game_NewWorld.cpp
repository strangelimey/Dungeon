// ============================================================================
// Game/Game_NewWorld.cpp - making a new world (docs/editor-updates-plan.md,
// P4; W7 in docs/world-editor-plan.md before it). Split out of Game_Editor.cpp.
//
// A world IS a project folder, and making one is a file operation: nothing
// about the running game changes until it is opened. Four ways, one spec
// (Game/NewWorld.h):
//
//   BLANK       the TEMPLATE's content (assets/templates/default, built by
//               tools/BuildTemplate.py) and one starter room. It used to copy
//               the RUNNING world's catalogs, so what "blank" meant drifted
//               with whatever you had open.
//   COPY WORLD  this world whole, places included, and its UNSAVED edits
//               written into the copy - never saved here first.
//   COPY LEVEL  this world's content and one of its levels as the only floor
//               of a one-level dungeon.
//   WIZARD      the template's content and one GENERATED floor.
//
// Blank and Wizard can start in a LIBRARY STYLE (tool-refinement Phase 7): the
// world receives it, its starter dungeon names it, its first floor wears it.
//
// Each is built in a hidden `.building-<name>` folder (Project::List skips
// dot-folders) and RENAMED into place only when complete. The old path wrote
// project.ini first, so a failed level or world write left a half-built
// folder that every world list still offered.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <string>

namespace dungeon::game {

namespace {
namespace fs = std::filesystem;

// The one starter dungeon every blank or one-level world has, and the doorway
// onto it. Named here once: the exit stair, the overworld and the dungeon
// entry all have to agree on them.
constexpr const char* kStarterDungeon = "keep";
constexpr const char* kStarterDoorway = "keep_gate";

std::string TemplateFolder() { return paths::Asset("templates") + "\\default"; }

// The first few ids of a catalog: a fresh level's surface palette. The first
// and then `count` more - what the old space-joining loop produced, kept so a
// blank world still comes out as it did.
std::vector<std::string> FirstIds(const Catalog& catalog, size_t count) {
	std::vector<std::string> out;
	for (const CatalogEntry& e : catalog.Entries()) {
		if (out.size() > count) break;
		out.push_back(e.id);
	}
	return out;
}

bool WriteText(const std::string& path, const std::string& text) {
	const std::string out = serialize::NormalizeEol(text);
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	if (assets::WriteBinaryFile(path, out.data(), out.size())) return true;
	log::Warn("new world: could not write {}", path);
	return false;
}

// What makes a world a particular GAME: its levels, dungeons, quests, opening
// and manifest comments. A blank or one-level world starts without them.
void ClearPlaces(Project& p) {
	p.levels.clear();
	p.startDungeon.clear();
	p.startLevel.clear();
	p.startX = p.startZ = -1;
	p.evalLevel.clear();
	// The manifest's COMMENTS are the source world's, about its level list and
	// opening; carrying them into a world they no longer describe is worse
	// than having none.
	p.manifest = {};
	p.dungeons = {};
	p.quests = {};
	// FLAGS make a particular game as quests do (a relic lifted, a seal broken),
	// so they go too - the world's as well as the dungeons': a world flag kept
	// with nothing left to set it is dead content the checker would name.
	p.flags = {};
	// AND THE HOOKS THAT NAME THEM (W7): an item's `quest` names a quest stage,
	// its `reveals` a world location and its `flag` a flag, all of which just
	// went. Content comes across; what content POINTS AT does not.
	for (Catalog* c : {&p.items, &p.weapons, &p.armor}) {
		const std::vector<CatalogEntry> entries = c->Entries(); // Add replaces by id
		for (CatalogEntry copy : entries) {
			serialize::Remove(copy.fields, "quest");
			serialize::Remove(copy.fields, "reveals");
			serialize::Remove(copy.fields, "flag");
			c->Add(std::move(copy));
		}
	}
}

// The one dungeon holding `stem`, which is also the harness's ground: a world
// with no `eval_level` opens the harness on the WORLD MAP, where half the dev
// commands refuse (W7).
void AddStarterDungeon(Project& p, const std::string& stem, const std::string& style = {}) {
	p.levels = {stem};
	p.evalLevel = stem;
	CatalogEntry dungeon;
	dungeon.id = kStarterDungeon;
	dungeon.lead.push_back("; The starter: one level, so the world has ground to stand on.");
	dungeon.Set("display", "The Keep");
	dungeon.Set("levels", stem);
	// Its default style (Phase 7): the next [+] in it opens on the style the
	// world was made in.
	if (!style.empty()) dungeon.Set("style", style);
	p.dungeons.Add(std::move(dungeon));
}

// The project's first EXIT stair type (stairs.cat `exit = 1`), "" if it has none.
std::string ExitStairType(const Project& p) {
	for (const CatalogEntry& e : p.stairs.Entries())
		if (CatalogBool(&e, "exit", false)) return e.id;
	return {};
}

// The overworld: one passable terrain everywhere, a start square, and ONE
// doorway onto `stem`. BLANK ON PURPOSE - the point of a new world is to paint
// your own; what it must not be is unopenable.
bool WriteStarterWorld(const Project& p, const std::string& stem) {
	// The first PASSABLE terrain is the ground: a world of water would load and
	// then refuse every step, which reads as a broken game.
	const CatalogEntry* ground = nullptr;
	for (const CatalogEntry& t : p.terrain.Entries())
		if (t.GetBool("passable", true)) {
			ground = &t;
			break;
		}
	if (!ground) {
		log::Warn("new world: terrain.cat has no passable kind to build on");
		return false;
	}
	const std::string glyph = ground->Get("glyph", "?");
	constexpr int kW = 16, kH = 12;
	std::string w = "; The overworld of a new world - paint it.\n";
	w += "start 4 6\n";
	w += std::format("location dungeon {} 8 6 dungeon={} level={}\n;\n", kStarterDoorway,
					 kStarterDungeon, stem);
	for (int z = 0; z < kH; ++z) {
		for (int x = 0; x < kW; ++x) w += glyph;
		w += '\n';
	}
	return WriteText(p.WorldMapPath(), w);
}

// The exit stair a starter level needs, as a record: on (x,z), facing `f` (the
// way you face stepping off it into the level), out to the starter doorway.
std::string ExitRecord(const std::string& type, int x, int z, const char* f) {
	return std::format("stairs {} {} {} {} dest={} destx=0 destz=0\n", type, x, z, f,
					   kStarterDoorway);
}
} // namespace

std::string Game::CreateWorld(const std::string& name, const NewWorldSpec& spec,
							  std::string* problem) {
	const auto fail = [&](std::string why) {
		log::Warn("new world: {}", why);
		if (problem) *problem = std::move(why);
		return std::string();
	};
	// Names are FOLDER names, typed by hand: filtered the way every authored id
	// is, since a stray slash here would be a path, not a name.
	std::string id = name;
	std::erase_if(id, [](char ch) {
		const unsigned char u = static_cast<unsigned char>(ch);
		return !(std::isalnum(u) || ch == '_' || ch == '-');
	});
	if (id.empty()) return fail(loc::Tr("map.worlds.noname"));
	const std::string root = paths::Asset("projects");
	const std::string folder = Project::FolderFor(root, id);
	std::error_code ec;
	// REFUSED, never merged into - and a folder there WITHOUT a manifest counts
	// too: renaming over it would fail, or worse, half succeed.
	if (fs::exists(folder, ec)) return fail(loc::Format("map.worlds.exists", id));
	const bool copying = spec.source != NewWorldSpec::Source::Blank;
	if (copying && !m_world) return fail(loc::Tr("map.newworld.noworld"));
	if (spec.source == NewWorldSpec::Source::CopyLevel &&
		std::find(m_project.levels.begin(), m_project.levels.end(), spec.level) ==
			m_project.levels.end())
		return fail(loc::Format("map.newworld.nolevel", spec.level));

	const std::string temp = Project::FolderFor(root, ".building-" + id);
	fs::remove_all(temp, ec); // a leftover from an interrupted create
	fs::create_directories(temp, ec);
	bool built = false;
	switch (spec.source) {
	case NewWorldSpec::Source::Blank: built = BuildBlankWorld(temp, id, spec, problem); break;
	case NewWorldSpec::Source::CopyWorld: built = BuildCopiedWorld(temp, id, problem); break;
	case NewWorldSpec::Source::CopyLevel:
		built = BuildLevelWorld(temp, id, spec.level, problem);
		break;
	case NewWorldSpec::Source::Wizard: built = BuildWizardWorld(temp, id, spec, problem); break;
	}
	if (built) fs::rename(temp, folder, ec);
	if (!built || ec) {
		fs::remove_all(temp, ec);
		if (problem && problem->empty()) *problem = loc::Tr("map.worlds.failed");
		log::Warn("new world '{}': not created", id);
		return {};
	}
	log::Info("Created world '{}' at {}", id, folder);
	return id;
}

bool Game::AddSpecStyle(Project& made, const NewWorldSpec& spec, std::string* problem) const {
	if (spec.style.empty()) return true;
	StyleLibrary::AddResult r;
	if (!m_library.AddTo(spec.style, made, r)) {
		if (problem) *problem = loc::Format("map.style.nolib", spec.style);
		return false;
	}
	log::Info("new world: style '{}' from the library ({} entries came with it){}", spec.style,
			  r.copied.size(), r.missingMonsters.empty() ? "" : ", some of its monsters missing");
	return true;
}

bool Game::BuildBlankWorld(const std::string& folder, const std::string& id,
						   const NewWorldSpec& spec, std::string* problem) {
	if (!fs::exists(TemplateFolder() + "\\project.ini")) {
		if (problem) *problem = loc::Format("map.newworld.notemplate", TemplateFolder());
		return false;
	}
	Project made = Project::Load(TemplateFolder());
	made.folder = folder;
	made.name = id;
	ClearPlaces(made); // the template has none - held to the same rule anyway
	if (!AddSpecStyle(made, spec, problem)) return false;
	const std::string stem = "room1";
	AddStarterDungeon(made, stem, spec.style);
	if (!made.Save()) return false;

	// The first room: the same box an empty new level is, with its palette from
	// the CATALOGS (there is no level to copy one from) and an exit stair on
	// the square north of the start, facing into the room, out to the doorway.
	// In a STYLE it wears the style's room theme, and its tags.
	std::array<std::vector<std::string>, 3> palettes{
		FirstIds(made.walls, 4), FirstIds(made.floors, 4), FirstIds(made.ceilings, 4)};
	const std::string styled = StyledStarterRecords(made, spec.style, palettes);
	const auto join = [](const std::vector<std::string>& ids) {
		std::string out;
		for (const std::string& s : ids) out += (out.empty() ? "" : " ") + s;
		return out;
	};
	std::string map = "; " + stem + " - the first room of a new world.\n";
	map += "palette wall " + join(palettes[0]) + "\n";
	map += "palette floor " + join(palettes[1]) + "\n";
	map += "palette ceiling " + join(palettes[2]) + "\n";
	map += styled + "\n";
	AppendStarterRoom(map);
	if (const std::string exit = ExitStairType(made); !exit.empty())
		map += "stairfacing arrive\n" + ExitRecord(exit, 8, 7, "south");
	return WriteText(made.LevelMapPath(stem), map) &&
		   WriteText(made.LevelEntPath(stem), "; " + stem + " - dynamic layer (empty).\n") &&
		   WriteStarterWorld(made, stem);
}

bool Game::BuildWizardWorld(const std::string& folder, const std::string& id,
							const NewWorldSpec& spec, std::string* problem) {
	// The TEMPLATE's content (Michael: the wizard starts from the template, not
	// from whatever world is open), one dungeon of one floor, and that floor
	// GENERATED - the generator is pure, so none of this needs the new world
	// to be running.
	if (!fs::exists(TemplateFolder() + "\\project.ini")) {
		if (problem) *problem = loc::Format("map.newworld.notemplate", TemplateFolder());
		return false;
	}
	Project made = Project::Load(TemplateFolder());
	made.folder = folder;
	made.name = id;
	ClearPlaces(made);
	if (!AddSpecStyle(made, spec, problem)) return false;
	const std::string stem = "floor1";
	AddStarterDungeon(made, stem, spec.style);
	if (!made.Save()) return false;
	std::string map, ent;
	if (!GenerateWizardLevel(made, stem, spec, kStarterDoorway, map, ent)) {
		if (problem) *problem = loc::Tr("map.worlds.failed");
		return false;
	}
	return WriteText(made.LevelMapPath(stem), map) && WriteText(made.LevelEntPath(stem), ent) &&
		   WriteStarterWorld(made, stem);
}

std::vector<std::string> Game::WizardTags() const {
	std::vector<std::string> tags;
	if (!fs::exists(TemplateFolder() + "\\project.ini")) return tags;
	const Project tpl = Project::Load(TemplateFolder());
	for (const CatalogEntry& m : tpl.monsters.Entries())
		for (std::string& t : ParseTags(m.Get("tags", "")))
			if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(std::move(t));
	std::sort(tags.begin(), tags.end());
	return tags;
}

bool Game::BuildCopiedWorld(const std::string& folder, const std::string& id,
							std::string* problem) {
	(void)problem;
	// The folder first, whole - so anything a world holds that nothing below
	// writes comes across too - then what is IN MEMORY over the top of it.
	std::error_code ec;
	fs::copy(m_project.folder, folder,
			 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
	if (ec) {
		log::Warn("new world: copying {} failed: {}", m_project.folder, ec.message());
		return false;
	}
	// THE UNSAVED EDITS GO INTO THE COPY. The catalogs and manifest as they
	// stand in memory, every level the editor holds an edit of, and the
	// overworld - written to the COPY'S paths, never this world's, so making a
	// copy never saves the world you are in behind your back.
	Project made = m_project;
	made.folder = folder;
	made.name = id;
	if (!made.Save()) return false;
	for (const std::string& stem : m_project.levels) {
		std::string mapText, entText;
		m_world->LevelTextFor(stem, mapText, entText);
		if (!mapText.empty() && !WriteText(made.LevelMapPath(stem), mapText)) return false;
		if (!entText.empty() && !WriteText(made.LevelEntPath(stem), entText)) return false;
	}
	if (m_worldMap && !WriteText(made.WorldMapPath(), m_worldMap->Serialize())) return false;
	return true;
}

bool Game::BuildLevelWorld(const std::string& folder, const std::string& id,
						   const std::string& stem, std::string* problem) {
	(void)problem;
	Project made = m_project;
	made.folder = folder;
	made.name = id;
	ClearPlaces(made);
	AddStarterDungeon(made, stem);
	if (!made.Save()) return false;

	// The level as it stands (unsaved edits included), else as on disk.
	std::string mapText, entText;
	m_world->LevelTextFor(stem, mapText, entText);
	const auto readFile = [](const std::string& path) {
		const auto bytes = assets::ReadBinaryFile(path);
		return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
	};
	if (mapText.empty()) mapText = readFile(m_project.LevelMapPath(stem));
	if (entText.empty()) entText = readFile(m_project.LevelEntPath(stem));

	// ITS STAIRS GO: each leads to a level (or a doorway) the new world does not
	// have, and a stair to nowhere is a checker error the moment it exists.
	std::string kept;
	bool arrival = false;
	for (size_t at = 0; at < mapText.size();) {
		size_t end = mapText.find('\n', at);
		if (end == std::string::npos) end = mapText.size();
		std::string line = mapText.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.starts_with("stairs ")) continue;
		if (line.starts_with("stairfacing")) arrival = true;
		kept += line + "\n";
	}
	// ...and ONE EXIT takes their place: on the first open square beside the
	// start, facing the start, out to the new world's one doorway - so a party
	// that walks in can walk out. The map is parsed to find where that is.
	if (const std::string exit = ExitStairType(made); !exit.empty()) {
		const DungeonMap map = DungeonMap::FromText(kept, DungeonWorld::FixtureTypesOf(made), stem);
		struct Side { int dx, dz; const char* facing; };
		// Facing is the way you face stepping OFF the stair - back toward the start.
		constexpr Side kSides[] = {{0, -1, "south"}, {1, 0, "west"}, {0, 1, "north"},
								   {-1, 0, "east"}};
		for (const Side& s : kSides) {
			const int x = map.StartX() + s.dx, z = map.StartZ() + s.dz;
			if (!map.IsWalkable(x, z)) continue;
			if (!arrival) kept += "stairfacing arrive\n";
			kept += ExitRecord(exit, x, z, s.facing);
			break;
		}
	}
	// ITS FLAG WIRING GOES TOO: the new world starts with no flags (ClearPlaces),
	// so a door or lever still waiting on one would wait forever - an error the
	// moment the world exists. The door and the lever stay; what they named does
	// not (W7's rule, one tier down).
	std::string ents;
	for (size_t at = 0; at < entText.size();) {
		size_t end = entText.find('\n', at);
		if (end == std::string::npos) end = entText.size();
		std::string line = entText.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.starts_with("door ") || line.starts_with("button ")) {
			std::string out;
			for (size_t t = 0; t < line.size();) {
				size_t e = line.find(' ', t);
				if (e == std::string::npos) e = line.size();
				const std::string tok = line.substr(t, e - t);
				t = e + 1;
				const std::string key = tok.substr(0, tok.find('='));
				if (tok.find('=') != std::string::npos &&
					(key == "flag" || key == "sets" || key == "clears" || key == "toggles"))
					continue;
				out += (out.empty() ? "" : " ") + tok;
			}
			line = out;
		}
		ents += line + "\n";
	}
	return WriteText(made.LevelMapPath(stem), kept) &&
		   WriteText(made.LevelEntPath(stem), ents) && WriteStarterWorld(made, stem);
}

} // namespace dungeon::game
