// ============================================================================
// Game/Project.cpp — see Project.h.
// ============================================================================
#include "Game/Project.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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
	{"doors.cat", &Project::doors, "Doors: sliding panel + shared frame (functional, block until opened)."},
	{"stairs.cat", &Project::stairs, "Stairs and pits: cross-level links with auto-authored pairs."},
	{"buttons.cat", &Project::buttons, "Buttons: wall levers (target= wires them to door names)."},
	{"items.cat", &Project::items, "Items: runes, keys, food, containers, ingredients (weapons/armor are their own catalogs)."},
	{"weapons.cat", &Project::weapons, "Weapons: items with attack settings (skill/damage/speed/stats/reach/command)."},
	{"armor.cat", &Project::armor, "Armor: worn items granting defense (armor soak + per-type resists)."},
	{"spells.cat", &Project::spells, "Spells: symbol-sequence recipes -> effect + element + power/mana/speed/range."},
	{"attacks.cat", &Project::attacks, "Attacks: per-melee-verb numbers (damage/accuracy/speed multipliers); identity (damage type) is C++ (Balance.h)."},
	{"balance.cat", &Project::balance, "Balance: the attack-formula knob sheet ([formula] block; docs/combat.md)."},
	{"damagetypes.cat", &Project::damagetypes, "Damage types: the vocabulary the combat maths is written in (physical flag + school); C++ names none of them."},
	{"effects.cat", &Project::effects, "Status effects: display name/icon/stacking per effect id; identity and behaviour are C++ (Game/Effect/)."},
	{"terrain.cat", &Project::terrain, "Terrain kinds: what a world-map cell is (glyph + travel/difficulty/tags)."},
	{"quests.cat", &Project::quests, "Quests: display name + ORDERED stage list; progress lives in the save, never here."},
	{"dungeons.cat", &Project::dungeons, "Dungeons: a named group of level stems with an entry level, reached through a world-map location."},
	{"wallfeatures.cat", &Project::wallfeatures, "Wall features: recessed niches carved into a wall panel."},
	{"surfacefeatures.cat", &Project::surfacefeatures, "Surface features: a tile stamped in place of a cell's floor or ceiling block (the wall-niche idea, laid flat). `surface` picks which."},
	{"imports.cat", &Project::imports,
	 "Imported assets: where each editor-imported texture set / model came from, "
	 "so tools/ReplayImports.ps1 can rebuild it (the baked files are gitignored)."},
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

std::string Project::FolderFor(const std::string& root, const std::string& name) {
	return root + "\\" + name;
}

std::string Project::FolderName() const {
	return std::filesystem::path(folder).filename().string();
}

std::vector<std::string> Project::List(const std::string& root) {
	std::vector<std::string> out;
	std::error_code ec; // no throwing: a missing projects folder is "none yet"
	for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
		if (!entry.is_directory()) continue;
		// A PROJECT IS A FOLDER WITH A MANIFEST. Anything else under here is
		// some other thing that happens to live there, and listing it would
		// offer the player a world that cannot be opened.
		if (!std::filesystem::exists(entry.path() / "project.ini")) continue;
		out.push_back(entry.path().filename().string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

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
			p.manifest = b; // kept whole — see the member's comment
			p.name = b.Get("name", "Untitled");
			p.levels = SplitWords(b.Get("levels"));
			p.defaultSconce = b.Get("default_sconce", "sconce");
			p.defaultBrazier = b.Get("default_brazier", "brazier");
			p.startDungeon = b.Get("start_dungeon", "");
			p.startLevel = b.Get("start_level", "");
			// -1 when unset OR unparseable: an entry cell is either authored in
			// full or not at all, and a half-read one would land the party on a
			// row it was never sent to.
			p.startX = std::atoi(b.Get("start_x", "-1").c_str());
			p.startZ = std::atoi(b.Get("start_z", "-1").c_str());
			p.evalLevel = b.Get("eval_level", "");
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

std::string Project::ManifestText() const {
	// THE BLOCK THAT WAS READ, updated — not a fresh one. Rebuilding it dropped
	// every comment in project.ini, and those comments are the only place the
	// level list, the harness level and the opening are explained. Keys the
	// manifest carries and this struct does not survive for the same reason a
	// catalog's unknown fields do.
	std::vector<serialize::Block> blocks(1, manifest);
	serialize::Block& m = blocks.front();
	m.id.clear(); // the manifest is the unnamed block, whatever was parsed
	m.Set("name", name);
	std::string levelList;
	for (size_t i = 0; i < levels.size(); ++i)
		levelList += (i ? " " : "") + levels[i];
	m.Set("levels", levelList);
	m.Set("default_sconce", defaultSconce);
	m.Set("default_brazier", defaultBrazier);
	if (!evalLevel.empty()) m.Set("eval_level", evalLevel);
	else serialize::Remove(m.fields, "eval_level");
	// Only written when the game starts in a dungeon: an absent block is the
	// ordinary "begin on the world map", and writing it out as empties would
	// make every project look like it had made a choice it had not. REMOVED
	// rather than blanked when the choice is taken back, because absent and
	// empty read differently — `start_x = ` is 0, not "unset".
	if (!startDungeon.empty()) {
		m.Set("start_dungeon", startDungeon);
		m.Set("start_level", startLevel);
		if (startX >= 0) {
			m.Set("start_x", std::to_string(startX));
			m.Set("start_z", std::to_string(startZ));
		} else {
			serialize::Remove(m.fields, "start_x");
			serialize::Remove(m.fields, "start_z");
		}
	} else {
		for (const char* key : {"start_dungeon", "start_level", "start_x", "start_z"})
			serialize::Remove(m.fields, key);
	}

	// THE GENERATED HEADER IS ONLY FOR A MANIFEST THAT HAD NONE. The unnamed
	// block has no "[id]" line, so the file's opening comment rides the FIRST
	// FIELD's lead — and it survives the round trip now. Emitting the generated
	// line as well would stack a second header on the first, one per save.
	const std::string header =
		manifest.fields.empty()
			? std::format("; {} — project manifest.{}{}", name, serialize::kEol,
						  serialize::kEol)
			: std::string();
	return header + serialize::WriteBlocks(blocks);
}

bool Project::Save() const {
	const std::string text = ManifestText();
	bool ok = assets::WriteBinaryFile(folder + "\\project.ini", text.data(),
									  text.size());
	if (!ok) log::Warn("Could not write project.ini in {}", folder);

	for (const CatalogSlot& slot : kCatalogs)
		ok &= (this->*(slot.member)).Save(CatalogPath(slot.file), slot.header);
	return ok;
}

Catalog* Project::CatalogForKey(const std::string& key) {
	if (key == "terrain") return &terrain;
	if (key == "dungeons") return &dungeons;
	if (key == "quests") return &quests;
	if (key == "walls") return &walls;
	if (key == "floors") return &floors;
	if (key == "ceilings") return &ceilings;
	if (key == "decorations") return &decorations;
	if (key == "fixtures") return &fixtures;
	if (key == "monsters") return &monsters;
	if (key == "doors") return &doors;
	if (key == "stairs") return &stairs;
	if (key == "buttons") return &buttons;
	if (key == "items") return &items;
	if (key == "weapons") return &weapons;
	if (key == "armor") return &armor;
	if (key == "spells") return &spells;
	if (key == "effects") return &effects;
	if (key == "damagetypes") return &damagetypes;
	if (key == "attacks") return &attacks;
	if (key == "balance") return &balance;
	if (key == "wallfeatures") return &wallfeatures;
	if (key == "surfacefeatures") return &surfacefeatures;
	return nullptr;
}

std::vector<const Catalog*> Project::AllCatalogs() const {
	// Same membership as CatalogForKey (minus `imports`, which is provenance) —
	// if a category is added there, add it here.
	return {&walls,  &floors,  &ceilings, &decorations,  &fixtures,
			&monsters, &doors, &stairs,   &buttons,      &items,
			&weapons, &armor,  &spells,   &effects,      &attacks,
			&balance, &damagetypes, &wallfeatures, &surfacefeatures,
			&terrain, &dungeons, &quests};
}

const CatalogEntry* Project::FindItem(std::string_view id) const {
	if (const CatalogEntry* e = items.Find(id)) return e;
	if (const CatalogEntry* e = weapons.Find(id)) return e;
	return armor.Find(id);
}

std::vector<const CatalogEntry*> Project::AllItems() const {
	std::vector<const CatalogEntry*> out;
	for (const Catalog* c : {&items, &weapons, &armor})
		for (const CatalogEntry& e : c->Entries()) out.push_back(&e);
	return out;
}

std::vector<Project::CatalogFile> Project::CatalogFiles() const {
	std::vector<CatalogFile> out;
	out.reserve(std::size(kCatalogs));
	for (const CatalogSlot& slot : kCatalogs)
		out.push_back({slot.file, &(this->*(slot.member)), slot.header});
	return out;
}

std::vector<std::string> Project::DungeonLevels(std::string_view dungeonId) const {
	std::vector<std::string> out;
	const CatalogEntry* d = dungeons.Find(dungeonId);
	if (!d) return out;
	// SplitWords, not ParseTags: a stem is a FILENAME, and it is about to be
	// drawn and opened, so it keeps the case it was authored in. (ParseTags
	// lowercases — right for tags, wrong for anything that names a file.)
	for (const std::string& stem : SplitWords(d->Get("levels", "")))
		if (std::find(levels.begin(), levels.end(), stem) != levels.end())
			out.push_back(stem);
	return out;
}

const CatalogEntry* Project::DungeonOfLevel(std::string_view stem) const {
	for (const CatalogEntry& d : dungeons.Entries())
		for (const std::string& s : SplitWords(d.Get("levels", "")))
			if (s == stem) return &d;
	return nullptr;
}

std::vector<std::string> Project::OrphanLevels() const {
	std::vector<std::string> out;
	for (const std::string& stem : levels)
		if (!DungeonOfLevel(stem)) out.push_back(stem);
	return out;
}

std::string Project::WorldMapPath() const {
	return std::format("{}\\world\\world.map", folder);
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
