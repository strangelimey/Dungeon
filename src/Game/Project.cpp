// ============================================================================
// Game/Project.cpp — see Project.h.
// ============================================================================
#include "Game/Project.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Game/Serialize.h"

#include <format>
#include <sstream>

namespace dungeon::game {

namespace {

// The catalog files, paired with the member they load into and a header comment
// written when saving. One table drives both Load and Save.
struct CatalogSlot {
	const char* file;
	Catalog Project::*member;
	const char* header;
};
const CatalogSlot kCatalogs[] = {
	{"walls.cat", &Project::walls, "Wall surface types: texture set + worn-block height displacement."},
	{"floors.cat", &Project::floors, "Floor surface types: texture set + worn-block height displacement."},
	{"ceilings.cat", &Project::ceilings, "Ceiling surface types: texture set + worn-block height displacement."},
	{"decorations.cat", &Project::decorations, "Decoration props: model + texture set + solid/authored flags."},
	{"fixtures.cat", &Project::fixtures, "Fixtures: sconces and braziers (model + texture + mount)."},
	{"monsters.cat", &Project::monsters, "Monsters: model + texture set."},
	{"doors.cat", &Project::doors, "Doors (reserved — populated in a later phase)."},
	{"stairs.cat", &Project::stairs, "Stairs (reserved — populated in a later phase)."},
	{"items.cat", &Project::items, "Items (reserved — populated in a later phase)."},
};

// Splits a space-separated list (the manifest's "levels" field) into stems.
std::vector<std::string> SplitWords(const std::string& s) {
	std::vector<std::string> out;
	std::istringstream in(s);
	std::string word;
	while (in >> word) out.push_back(word);
	return out;
}

} // namespace

Project Project::Load(const std::string& folder) {
	Project p;
	p.folder = folder;

	// Manifest (project.ini). A missing file leaves the defaults — a fresh
	// project folder still loads.
	if (auto bytes = assets::ReadBinaryFile(folder + "\\project.ini")) {
		const std::string text(bytes->begin(), bytes->end());
		const std::vector<serialize::Block> blocks = serialize::ParseBlocks(text);
		for (const serialize::Block& b : blocks) {
			if (!b.id.empty()) continue; // manifest lives in the unnamed block
			p.name = b.Get("name", "Untitled");
			p.levels = SplitWords(b.Get("levels"));
			p.defaultSconce = b.Get("default_sconce", "sconce");
			p.defaultBrazier = b.Get("default_brazier", "brazier");
		}
	} else {
		log::Warn("project has no project.ini: {}", folder);
	}

	for (const CatalogSlot& slot : kCatalogs)
		(p.*(slot.member)).Load(p.CatalogPath(slot.file));

	log::Info("Loaded project '{}' ({}): {} levels, {} wall/{} floor/{} ceiling "
			  "types, {} decorations, {} monsters",
			  p.name, folder, p.levels.size(), p.walls.Entries().size(),
			  p.floors.Entries().size(), p.ceilings.Entries().size(),
			  p.decorations.Entries().size(), p.monsters.Entries().size());
	return p;
}

bool Project::Save() const {
	std::vector<serialize::Block> manifest(1); // one unnamed block
	serialize::Block& m = manifest.front();
	m.Set("name", name);
	std::string levelList;
	for (size_t i = 0; i < levels.size(); ++i)
		levelList += (i ? " " : "") + levels[i];
	m.Set("levels", levelList);
	m.Set("default_sconce", defaultSconce);
	m.Set("default_brazier", defaultBrazier);

	const std::string text = std::format("; {} — project manifest.\n\n", name) +
							 serialize::WriteBlocks(manifest);
	bool ok = assets::WriteBinaryFile(folder + "\\project.ini", text.data(),
									  text.size());
	if (!ok) log::Warn("Could not write project.ini in {}", folder);

	for (const CatalogSlot& slot : kCatalogs)
		ok &= (this->*(slot.member)).Save(CatalogPath(slot.file), slot.header);
	return ok;
}

Catalog* Project::CatalogForKey(const std::string& key) {
	if (key == "walls") return &walls;
	if (key == "floors") return &floors;
	if (key == "ceilings") return &ceilings;
	if (key == "decorations") return &decorations;
	if (key == "fixtures") return &fixtures;
	if (key == "monsters") return &monsters;
	if (key == "doors") return &doors;
	if (key == "stairs") return &stairs;
	if (key == "items") return &items;
	return nullptr;
}

std::string Project::LevelMapPath(const std::string& stem) const {
	return std::format("{}\\levels\\{}.map", folder, stem);
}

std::string Project::LevelEntPath(const std::string& stem) const {
	return std::format("{}\\levels\\{}.ent", folder, stem);
}

std::string Project::CatalogPath(const std::string& file) const {
	return std::format("{}\\catalog\\{}", folder, file);
}

} // namespace dungeon::game
