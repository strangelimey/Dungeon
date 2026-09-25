// ============================================================================
// Game/Game_Editor.cpp — split out of Game.cpp to keep files small (see Game.h).
// Asset-bake flow + editor level create/rename/sync + catalog writes.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace dungeon::game {
void Game::OpenCreateDialog(MapEditor::PaletteCat cat, AssetDialog::Source source,
							const std::string& asset) {
	const char* key = MapEditor::CategoryCatalogKey(cat);
	if (!*key) return; // belt-and-braces: every category names a catalog
	// The category's existing ids drive both the duplicate-name check and the
	// "copy from" list.
	std::vector<std::string> existing;
	if (const Catalog* c = m_project.CatalogForKey(key))
		for (const CatalogEntry& e : c->Entries()) existing.push_back(e.id);
	m_assetDialog.Open(loc::Tr(MapEditor::CategoryNameKey(cat)), key,
					   MapEditor::CategoryTextureSet(cat), std::move(existing),
					   m_settings.theme, source, asset);
}

bool Game::StartBakeStep() {
	// The baker is a sibling of this exe (per-config build output); the assets it
	// writes into are the ONE shared tree every config reads.
	const std::string baker = paths::ExecutableDir() + "\\AssetBaker.exe";
	const std::string assets = paths::AssetsDir();
	const auto q = [](const std::string& s) { return "\"" + s + "\""; };

	std::string cmd;
	if (!m_bakeReq.textureSet)
		cmd = q(baker) + " import-model " + q(m_bakeReq.sourcePath) + " " + q(assets) +
			  " " + m_bakeReq.name;
	else if (m_bakeStep == 0) {
		// A PBR set installs under its RESOLUTION-tagged name (<set>_1k/_2k/_4k)
		// — that is what LoadPbrSet asks for, and _2k is its universal fallback,
		// so an editor import lands there whatever the source resolution was.
		// The catalog's `texture` field names the base, as always.
		cmd = q(baker) + " import " + q(m_bakeReq.sourcePath) + " " + q(assets) + " " +
			  m_bakeReq.name + "_2k";
		// GL-convention normals (green up) need flipping; the importer sniffs the
		// filename, and this is the dialog's override for sets that don't say so.
		if (m_bakeReq.flipGreen) cmd += " --flip-green";
	} else {
		// Bake worn block meshes for just the new set (its kind = the catalog).
		const std::string kind = m_bakeReq.catalogKey == "floors"    ? "floor"
								 : m_bakeReq.catalogKey == "ceilings" ? "ceiling"
																	  : "wall";
		// A wornblock bake names the TEXTURE SET, which for an Installed-source
		// type is the pool asset, not the new catalog id.
		const std::string set = m_bakeReq.asset.empty() ? m_bakeReq.name : m_bakeReq.asset;
		cmd = q(baker) + " wornblock " + kind + " " + set + " " + q(assets);
		// Surface-look knobs (defaults for an asset-create; set by a restyle).
		if (m_bakeWear != 1.0f)
			cmd += std::format(" --wear {:.3f}", m_bakeWear);
		if (m_bakeRelief >= 0.0f) cmd += std::format(" --relief {:.4f}", m_bakeRelief);
	}
	log::Info("AssetBaker: {}", cmd);
	return m_bake.Start(cmd);
}

bool Game::SyncProjectToSource() {
	const std::string& repo = paths::RepoAssetsDir();
	if (repo.empty()) {
		log::Warn("sync to source: no source path baked in");
		return false;
	}
	// A dev build already RUNS from the source tree (paths::AssetsDir), so a save
	// has landed in git the moment it was written and there is nothing to copy —
	// copying here would be a directory onto itself. Only a packaged build, whose
	// assets are the copy beside the exe, still has real work to do.
	if (paths::AssetsDir() == repo) {
		log::Info("sync to source: already running from the source tree, nothing to copy");
		return true;
	}

	namespace fs = std::filesystem;
	const fs::path src = m_project.folder; // the build-copy project
	const fs::path dst = fs::path(repo) / "projects" / src.filename();
	std::error_code ec;
	fs::create_directories(dst, ec);
	fs::copy(src, dst,
			 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
			 ec);
	if (ec) {
		log::Warn("sync to source failed: {}", ec.message());
		return false;
	}

	// The project's catalogs may reference assets that exist only in this build's
	// own pool (an editor import writes into paths::AssetsDir). They are
	// gitignored either way, but the SOURCE tree is what a new worktree is
	// provisioned from, so leaving them build-only loses them with the build
	// directory. imports.cat says exactly which.
	int copied = 0;
	for (const CatalogEntry& e : m_project.imports.Entries()) {
		const std::string kind = e.Get("kind", "texture");
		// A texture set is its map trio (source PNG + baked DDS) plus the worn
		// block meshes derived from it; a model is its .gltf plus the PBR set
		// import-model brought in under <name>_2k.
		const std::pair<const char*, std::string> globs[] = {
			{"textures", e.id},
			{"models", kind == "texture" ? "worn_" + e.id : e.id},
			{"textures", kind == "model" ? e.id + "_2k" : std::string()},
		};
		for (const auto& [dir, prefix] : globs) {
			if (prefix.empty()) continue;
			const fs::path from = fs::path(paths::AssetsDir()) / dir;
			const fs::path to = fs::path(repo) / dir;
			fs::create_directories(to, ec);
			for (const auto& entry : fs::directory_iterator(from, ec)) {
				if (ec || !entry.is_regular_file()) continue;
				const std::string name = entry.path().filename().string();
				if (!name.starts_with(prefix)) continue;
				std::error_code copyEc;
				fs::copy_file(entry.path(), to / name,
							  fs::copy_options::overwrite_existing, copyEc);
				if (!copyEc) ++copied;
			}
		}
	}
	log::Info("Synced project {} -> source ({} imported asset file(s))",
			  src.filename().string(), copied);
	return true;
}

namespace {

// The first few ids of a catalog, space-joined — a fresh level's surface
// palette. The ACTIVE level is the donor when there is one (CreateNewLevel),
// but a brand-new world has no level to copy from, so the catalogs are.
std::string FirstIds(const Catalog& catalog, size_t count) {
	std::string out;
	for (const CatalogEntry& e : catalog.Entries()) {
		if (out.size() && count-- == 0) break;
		out += (out.empty() ? "" : " ") + e.id;
	}
	return out;
}

// The minimal level both a new world and an empty new level start from: a
// 16x16 block of rock with a 3x3 room in the middle and the start at its
// centre. Appended as grid rows, after the caller's palette records. FIXED on
// purpose: scenarios and habits build on the room being at 7..9 (see
// CreateNewLevel).
constexpr int kStarterSize = 16, kStarterCentre = 8;
void AppendStarterRoom(std::string& map) {
	for (int z = 0; z < kStarterSize; ++z) {
		for (int x = 0; x < kStarterSize; ++x) {
			const bool room = std::abs(x - kStarterCentre) <= 1 &&
							 std::abs(z - kStarterCentre) <= 1;
			map += !room ? '#' : (x == kStarterCentre && z == kStarterCentre) ? 'P' : '.';
		}
		map += '\n';
	}
}

// A new world's first room: the same minimal 16x16 box CreateNewLevel writes,
// with its palette taken from the CATALOGS rather than from a level, because
// there is not one yet.
bool WriteStarterLevel(const Project& p, const std::string& stem) {
	std::string map = "; " + stem + " - the first room of a new world.\n";
	map += "palette wall " + FirstIds(p.walls, 4) + "\n";
	map += "palette floor " + FirstIds(p.floors, 4) + "\n";
	map += "palette ceiling " + FirstIds(p.ceilings, 4) + "\n\n";
	AppendStarterRoom(map);
	const std::string ent = "; " + stem + " - dynamic layer (empty).\n";
	const std::string mapOut = serialize::NormalizeEol(map);
	const std::string entOut = serialize::NormalizeEol(ent);
	if (assets::WriteBinaryFile(p.LevelMapPath(stem), mapOut.data(), mapOut.size()) &&
		assets::WriteBinaryFile(p.LevelEntPath(stem), entOut.data(), entOut.size()))
		return true;
	log::Warn("new world: could not write the starter level {}", stem);
	return false;
}

// And its overworld: one passable terrain everywhere, a start cell, and one
// doorway onto the starter room. BLANK ON PURPOSE — the point of a new world is
// to paint your own; what it must not be is unopenable.
bool WriteStarterWorld(const Project& p) {
	// The first PASSABLE terrain is the ground. A world of water would load and
	// then refuse every step, which reads as a broken game rather than as an
	// authoring choice nobody made.
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
	w += "location dungeon keep_gate 8 6 dungeon=keep level=room1\n;\n";
	for (int z = 0; z < kH; ++z) {
		for (int x = 0; x < kW; ++x) w += glyph;
		w += '\n';
	}
	const std::string out = serialize::NormalizeEol(w);
	if (assets::WriteBinaryFile(p.WorldMapPath(), out.data(), out.size())) return true;
	log::Warn("new world: could not write {}", p.WorldMapPath());
	return false;
}

} // namespace

// --- worlds (W7, docs/world-editor-plan.md) ---------------------------------
// A world IS a project folder (Michael's word for one), and creating a new one
// is a file operation rather than a live edit: nothing about the running game
// changes until it is opened, which is what makes switching a relaunch.

bool Game::SwitchWorld(const std::string& name) {
	const std::string root = paths::Asset("projects");
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), name) == found.end()) {
		log::Warn("no world '{}' under {}", name, root);
		return false;
	}
	// ALREADY THERE means the world LOADED, not the one the setting names —
	// a `-project` run leaves the setting alone, so comparing against it
	// refused to leave a scenario for the world settings.ini already held.
	if (m_world && name == m_project.FolderName()) return true;
	// Remembered as the last world played (the next launch's default), unless
	// this run's world was named on the command line — a scenario must not
	// rewrite the developer's choice.
	if (!m_worldFromCommandLine) {
		m_settings.projectName = name;
		m_settings.Save();
	}
	// IN THE PROCESS (docs/world-on-demand.md) — it used to relaunch. Deferred
	// to the next frame's top: the asks come from inside widget callbacks.
	m_pendingWorld = PendingWorld{name, {}};
	return true;
}

// The new-game world list's pick: a new game in the loaded world at once, or
// the switch to another (a new game there, next frame).
void Game::StartNewGameIn(const std::string& folder) {
	if (m_world && folder == m_project.FolderName()) {
		m_ui.onStartNewGame();
		return;
	}
	if (!SwitchWorld(folder))
		log::Warn("new game: world '{}' is gone", folder);
}

// --- deleting a world (W9) ---------------------------------------------------
// The one file operation in the editor that nothing brings back: the undo
// history is in memory and describes THIS world, and a world made in the editor
// was never in git. So the rules are strict and live HERE rather than in the
// dialog — the console reaches this too, and a rule held by only one of two
// ways in is not a rule.

std::string Game::WorldDeleteRefusal(const std::string& name) const {
	const std::string root = paths::Asset("projects");
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), name) == found.end())
		return loc::Format("map.worlds.missing", name);
	// The RUNNING world's files are open in front of you — its levels, its
	// catalogs, the world being drawn. Leave it first.
	if (name == m_project.FolderName())
		return loc::Format("map.worlds.delete.running", name);
	// THE FALLBACK. ChooseProjectFolder lands here whenever the world a launch
	// asks for is missing — including the one being deleted, if settings.ini
	// names it — and a launch with no world to fall back on cannot stand up.
	if (name == kDefaultProject) return loc::Format("map.worlds.delete.fallback", name);
	return {};
}

std::string Game::DescribeWorld(const std::string& name) const {
	// Read from DISK, not from anything in memory: it is not the running
	// world, so the only true account of what deleting it destroys is the
	// folder itself.
	const Project p = Project::Load(Project::FolderFor(paths::Asset("projects"), name));
	return loc::Format("map.worlds.delete.what", p.levels.size(),
					   p.dungeons.Entries().size());
}

bool Game::DeleteWorld(const std::string& name) {
	if (const std::string why = WorldDeleteRefusal(name); !why.empty()) {
		log::Warn("delete world '{}' refused: {}", name, why);
		return false;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path root = fs::weakly_canonical(paths::Asset("projects"), ec);
	const fs::path folder =
		fs::weakly_canonical(Project::FolderFor(paths::Asset("projects"), name), ec);
	// BELT AND BRACES before a remove_all: the folder must sit DIRECTLY under
	// the projects root and look like a world. A name is filtered to an id
	// everywhere it is typed, but this is the line that deletes, so it checks
	// the path it is about to delete rather than trusting how it was built.
	if (ec || folder.parent_path() != root ||
		!fs::exists(folder / "project.ini")) {
		log::Warn("delete world '{}': {} is not a world folder under {} - refused",
				  name, folder.string(), root.string());
		return false;
	}
	const std::uintmax_t removed = fs::remove_all(folder, ec);
	if (ec) {
		// Partial is possible (a file held open elsewhere) — say so, since the
		// folder may now be a world with pieces missing.
		log::Warn("delete world '{}': {} ({} files removed before it stopped)", name,
				  ec.message(), removed);
		return false;
	}
	// A setting naming a world that is gone would fall back with a warning on
	// every launch; point it at the fallback instead.
	if (m_settings.projectName == name) {
		m_settings.projectName = kDefaultProject;
		m_settings.Save();
	}
	log::Info("Deleted world '{}' ({} files) from {}", name, removed, folder.string());
	return true;
}

std::string Game::CreateWorld(const std::string& name) {
	// Names are FOLDER names and are typed by hand, so they are filtered the
	// way every other authored id is (the DoorInspector rule) rather than
	// trusted — a stray slash here is a path, not a name.
	std::string id = name;
	std::erase_if(id, [](char ch) {
		const unsigned char u = static_cast<unsigned char>(ch);
		return !(std::isalnum(u) || ch == '_' || ch == '-');
	});
	if (id.empty()) {
		log::Warn("new world: a name is required");
		return {};
	}
	const std::string root = paths::Asset("projects");
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), id) != found.end()) {
		// REFUSED, never merged into: creating over a world would quietly
		// rewrite catalogs somebody else's levels reference.
		log::Warn("new world: '{}' already exists", id);
		return {};
	}
	const std::string folder = Project::FolderFor(root, id);

	// THE CONTENT COMES ACROSS, THE PLACES DO NOT. Start from this project so
	// the new world has surfaces to build with and monsters to place, then
	// clear what makes it a particular game: its levels, its dungeons, its
	// quests and where it begins.
	// CONTENT IS COPIED from the world loaded — or, on the title screen where
	// none is, from the default world (docs/world-on-demand.md).
	Project made = m_world ? m_project
						   : Project::Load(Project::FolderFor(root, m_defaultWorld));
	made.folder = folder;
	made.name = id;
	made.levels.clear();
	made.startDungeon.clear();
	made.startLevel.clear();
	made.startX = made.startZ = -1;
	made.evalLevel.clear();
	// The manifest's COMMENTS are this project's, about this project's level
	// list and opening — carrying them into a world they no longer describe
	// would be worse than having none.
	made.manifest = {};
	made.dungeons = {};
	made.quests = {};
	// AND THE HOOKS THAT NAME THEM. An item's `quest` points at a quest stage
	// and its `reveals` at a world location — progress and places, both of
	// which just went. Copying the items without stripping these left the new
	// world naming a quest and a location it had never heard of, which the
	// checker reported on its first run. Content comes across; what content
	// POINTS AT does not.
	for (Catalog* c : {&made.items, &made.weapons, &made.armor}) {
		// Over a SNAPSHOT: Add replaces by id, and writing into the very
		// element a range-for is holding is the kind of thing that is fine
		// today and a debugging session after the next change.
		const std::vector<CatalogEntry> entries = c->Entries();
		for (CatalogEntry copy : entries) {
			serialize::Remove(copy.fields, "quest");
			serialize::Remove(copy.fields, "reveals");
			c->Add(std::move(copy));
		}
	}

	// One room, in one dungeon, behind one doorway. The engine loads a level in
	// DungeonWorld's constructor, so a world with nowhere in it cannot stand
	// up; this is the smallest starter that both loads and passes the checker
	// (an unclaimed level is an orphan warning, and a dungeon nothing reaches
	// is another).
	const std::string stem = "room1";
	made.levels.push_back(stem);
	// AND IT IS THE HARNESS'S GROUND. A test scenario in its own world is the
	// point of worlds existing (Michael, 2026-09-23), and a world with no
	// `eval_level` opens the harness on the WORLD MAP — where half the dev
	// commands refuse, because the party is not in a level. Naming it here
	// means a new world is usable as a scenario the moment it exists.
	made.evalLevel = stem;
	CatalogEntry dungeon;
	dungeon.id = "keep";
	dungeon.lead.push_back("; The starter: one room, so the world has ground to stand on.");
	dungeon.Set("display", "The Keep");
	dungeon.Set("levels", stem);
	made.dungeons.Add(std::move(dungeon));

	if (!made.Save()) {
		log::Warn("new world: could not write {}", folder);
		return {};
	}
	if (!WriteStarterLevel(made, stem) || !WriteStarterWorld(made)) return {};
	log::Info("Created world '{}' at {}", id, folder);
	return id;
}

// Mints a fresh level: writes a .map/.ent pair next to the project's other
// levels - generated from the knobs, or the minimal empty box - appends the stem
// to the manifest and its dungeon, and stairs it to the floor above. The palette
// gate demands all three surface records; they are copied from the ACTIVE level
// so the new one shares its look. Everything downstream (browse, remote edits,
// stair dests, savemap) reads Project::levels or lazy-parses the files, so no
// other state needs touching. Returns the stem, or "" on failure.
std::string Game::CreateNewLevel(const std::string& dungeonId,
								 const generate::Params* params) {
	// THE STEM IS NAMED AFTER ITS DUNGEON when it has one — crypt1, crypt2,
	// crypt3 — so the grouping the picker shows is legible in the filename too,
	// which is how the demo's levels were already hand-named. A level with no
	// dungeon falls back to the old "levelN".
	const std::string base = dungeonId.empty() ? "level" : dungeonId;
	// Next free <base>N (numeric suffixes only; foreign stems just don't bump
	// the counter, and the find() guard keeps the pick collision-free).
	int maxN = 0;
	for (const std::string& s : m_project.levels)
		if (s.starts_with(base))
			if (int n = std::atoi(s.c_str() + base.size()); n > maxN) maxN = n;
	const std::string stem = base + std::to_string(maxN + 1);
	if (std::find(m_project.levels.begin(), m_project.levels.end(), stem) !=
		m_project.levels.end()) {
		log::Warn("new level: stem {} already exists", stem);
		return {};
	}

	std::string map, ent;
	// Where the stair from the floor above may land, best first.
	std::vector<std::pair<int, int>> linkCells;
	CatalogEntry* dungeon = m_project.dungeons.Find(dungeonId);
	// THE STAIR SQUARE IS CHOSEN FIRST and the new floor built around it: the
	// far end of the floor above, where a stair can stand. A stair needs the
	// same (x,z) on both levels, and searching two finished, unrelated layouts
	// for a square both happen to have free failed three times out of three on
	// the first run (docs/level-building.md P1). {-1,-1} for a dungeon's first
	// floor, which has nothing above it.
	//
	// A GENERATED level only. The EMPTY one is the blank canvas it always was -
	// the fixed box in the middle, and no stair: WorldTest's rename scenario
	// (and anyone who has used [+] before) builds on the room being at 7..9,
	// and moving it put a hand-written stair in rock, which is a load-time
	// abort. An empty floor is joined up with the stair brush, as before.
	std::pair<int, int> entry{-1, -1};
	if (dungeon && params)
		if (const std::vector<std::string> floors = m_project.DungeonLevels(dungeonId);
			!floors.empty())
			entry = m_world->FarthestStairCell(floors.back());
	if (params) {
		generate::Params p = *params;
		p.entryX = entry.first;
		p.entryZ = entry.second;
		// The theme the content pools are drawn by: the viewed level's own,
		// else the dungeon's flavour tags - a fresh dungeon's first generated
		// floor has no level theme to inherit, and "undead crypt" should still
		// fill with undead.
		// A theme CHOSEN in the dialog (P4b) wins over both.
		std::vector<std::string> theme = !p.theme.empty()
											 ? std::vector<std::string>{p.theme}
											 : m_mapView.ViewedMap().Theme();
		if (theme.empty() && dungeon) theme = ParseTags(dungeon->Get("tags", ""));
		linkCells = ComposeGeneratedLevel(stem, p, theme, map, ent);
	} else {
		auto join = [](const std::vector<std::string>& ids) {
			std::string out;
			for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
			return out;
		};
		const DungeonMap& live = m_world->Map(); // active level: the palette donor
		map = "; " + stem + " - created in the editor.\n";
		map += "palette wall " + join(live.WallPalette()) + "\n";
		map += "palette floor " + join(live.FloorPalette()) + "\n";
		map += "palette ceiling " + join(live.CeilingPalette()) + "\n\n";
		AppendStarterRoom(map);
		ent = "; " + stem + " - dynamic layer (empty).\n";
	}
	// Same boundary rule as the level writers: built with '\n', ended once here.
	const std::string mapOut = serialize::NormalizeEol(map);
	const std::string entOut = serialize::NormalizeEol(ent);
	if (!assets::WriteBinaryFile(m_project.LevelMapPath(stem), mapOut.data(),
								 mapOut.size()) ||
		!assets::WriteBinaryFile(m_project.LevelEntPath(stem), entOut.data(),
								 entOut.size())) {
		log::Warn("new level: failed to write {} files", stem);
		return {};
	}
	m_project.levels.push_back(stem);
	// AND INTO ITS DUNGEON (W5). `levels` above stays the flat universe every
	// route walks; this is the level joining a GROUP, which is what stops an
	// editor-made level arriving as an orphan the checker then reports.
	if (dungeon) {
		const std::string was = dungeon->Get("levels", "");
		dungeon->Set("levels", was.empty() ? stem : was + " " + stem);
	}
	m_project.Save();
	// AFTER joining the dungeon: the floor above is found by dungeon order.
	// (An empty level offers no cells, so it stays unlinked - see above.)
	if (!linkCells.empty()) LinkToFloorAbove(stem, linkCells);
	if (m_world->onMessage)
		m_world->onMessage(dungeonId.empty()
							  ? loc::FormatLine("map.level.created", stem)
							  : loc::FormatLine("map.level.createdin", stem,
												dungeonId));
	return stem;
}

// The Level dialog's rename: validate against the manifest, let the world
// move files / rekey stashes / repoint stair dests, then commit the manifest
// and keep the map view's browse snapshot truthful. (The dialog adopts the
// new stem itself on true.)
bool Game::RenameLevel(const std::string& oldStem, const std::string& newStem,
						std::string* why) {
	// Each refusal its own sentence (W4's lesson), handed back for a caller
	// that shows it and logged for one that does not.
	const auto refuse = [&](std::string reason) {
		log::Warn("rename level '{}' -> '{}' refused: {}", oldStem, newStem, reason);
		if (why) *why = std::move(reason);
		return false;
	};
	std::vector<std::string>& levels = m_project.levels;
	const auto it = std::find(levels.begin(), levels.end(), oldStem);
	if (it == levels.end()) return refuse(loc::Format("map.level.unknown", oldStem));
	// A stem is a FILE NAME and a record word. The dialog filters what is
	// typed, but the console reaches this too — the rule lives here as well.
	if (newStem.empty() || !std::all_of(newStem.begin(), newStem.end(), [](char ch) {
			const unsigned char u = static_cast<unsigned char>(ch);
			return std::isalnum(u) || ch == '_' || ch == '-';
		}))
		return refuse(loc::Format("map.level.badstem", newStem));
	if (std::find(levels.begin(), levels.end(), newStem) != levels.end()) {
		if (m_world->onMessage)
			m_world->onMessage(loc::FormatLine("map.level.dupname", newStem));
		return refuse(loc::Format("map.level.dupname", newStem));
	}
	if (!m_world->RenameLevel(oldStem, newStem))
		return refuse(loc::Format("map.level.movefailed", oldStem));
	*it = newStem;

	// EVERYTHING ELSE THAT NAMES A LEVEL BY ITS STEM (W11). The world half
	// only ever fixed stairs, so a renamed level fell out of its dungeon (an
	// orphan warning, and gone from the two-tier picker), and a new game, the
	// eval harness or a doorway naming it opened onto a file that was no
	// longer there. The list is closed — these are every stem reference
	// outside the level files — which is what makes it worth writing out.
	// Collected before any write, the SweepCatalogRefs rule: Add writes into
	// the vector being walked.
	std::vector<CatalogEntry> retyped;
	for (const CatalogEntry& e : m_project.dungeons.Entries()) {
		std::string words = e.Get("levels", "");
		std::string out;
		bool hit = false;
		size_t i = 0;
		while (i < words.size()) {
			while (i < words.size() && words[i] == ' ') ++i;
			const size_t start = i;
			while (i < words.size() && words[i] != ' ') ++i;
			if (i == start) break;
			std::string w = words.substr(start, i - start);
			if (w == oldStem) {
				w = newStem;
				hit = true;
			}
			out += (out.empty() ? "" : " ") + w;
		}
		if (!hit) continue;
		CatalogEntry copy = e;
		copy.Set("levels", out);
		retyped.push_back(std::move(copy));
	}
	for (CatalogEntry& e : retyped)
		m_project.dungeons.Add(std::move(e)); // replaces in place, by id
	if (m_project.startLevel == oldStem) m_project.startLevel = newStem;
	if (m_project.evalLevel == oldStem) m_project.evalLevel = newStem;
	m_project.Save();
	// A doorway's `level=`. The world is written straight away like the
	// manifest above, since the files it points at have already moved.
	if (m_worldMap) {
		std::vector<std::string> doors;
		for (const WorldMap::Location& l : m_worldMap->Locations())
			if (l.level == oldStem) doors.push_back(l.id);
		for (const std::string& id : doors) m_worldMap->MutableLocation(id)->level = newStem;
		if (!doors.empty()) SaveWorld();
	}
	m_mapView.OnLevelRenamed(oldStem, newStem);
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.level.renamed", oldStem, newStem));
	return true;
}

// The bake succeeded: append the new entry to the right project catalog and save
// (so the type is usable — model kinds load lazily on first placement). Writes go
// to the asset copy next to the exe, not the git source tree.
void Game::FinishBake() {
	CreateCatalogEntry(m_bakeReq);
	m_assetDialog.SetBusy(false);
	m_assetDialog.Close();
}

// Writes the new type's catalog entry and makes it reachable. The entry's SHAPE
// comes from the category's schema (CatalogSchema): every row with a default is
// seeded, so a new stair gets its up/pair/hole rows and a new item its
// weight/holdable — where the old one-shape-fits-all writer stamped
// authored=1/solid=1 on everything and left doors, stairs and items broken.
void Game::CreateCatalogEntry(const AssetDialog::CreateRequest& req) {
	Catalog* cat = m_project.CatalogForKey(req.catalogKey);
	if (!cat) {
		log::Warn("asset create: unknown catalog '{}'", req.catalogKey);
		return;
	}
	CatalogEntry e;
	// Duplicate starts from the source entry, so everything hand-authored on it
	// (fields no schema row covers included) comes along.
	if (req.source == AssetDialog::Source::Duplicate)
		if (const CatalogEntry* src = cat->Find(req.asset)) {
			e = *src;
			e.lead.clear(); // the source's comment introduced the source, not this
		}
	e.id = req.name;
	// Identity first, so the entry reads like a hand-authored one. Items name
	// themselves with a loc key by convention; everything else carries a display
	// string.
	if (req.catalogKey == "items" || req.catalogKey == "weapons" ||
		req.catalogKey == "armor")
		e.Set("name", "item." + req.name); // items name themselves with a loc key
	else e.Set("display", req.name);
	if (!req.group.empty()) e.Set("category", req.group);

	// What the type binds to: an import writes its own name (the baker wrote the
	// asset under it), the other sources point at what the user picked.
	const std::string asset =
		req.source == AssetDialog::Source::Import ? req.name : req.asset;
	if (req.source != AssetDialog::Source::Duplicate) {
		if (req.textureSet) e.Set("texture", asset);
		else {
			e.Set("model", asset);
			// An imported model brings its own PBR set under the same name; a pool
			// model keeps whatever the entry already binds (the schema default).
			if (req.source == AssetDialog::Source::Import) e.Set("texture", asset);
		}
	}
	if (!req.textureSet && req.source == AssetDialog::Source::Import)
		e.Set("authored", "1"); // bought/authored meshes are back-face culled

	// Then the category's own shape: every schema row with a default that the
	// entry doesn't already carry. This is what gives a new stair its up/pair/
	// hole rows and a new item its weight/holdable, where the old writer stamped
	// authored=1/solid=1 on every category alike.
	for (const FieldSpec& spec : SchemaFor(req.catalogKey))
		if (*spec.def && !e.Find(spec.key)) e.Set(spec.key, spec.def);

	// The dialog's material sliders, persisted only when the user moved them:
	// metallic/roughness become the draw's factors (with an ORM map the shader
	// scales the map by them), color tints the albedo, and height_scale overrides
	// the bound set's parallax depth. Untouched sliders leave the asset's own
	// material authoritative.
	const gfx::MaterialParams& m = req.material;
	if (req.metallicSet) e.Set("metallic", std::format("{:.3f}", m.metallic));
	if (req.roughnessSet) e.Set("roughness", std::format("{:.3f}", m.roughness));
	if (req.heightSet) e.Set("height_scale", std::format("{:.3f}", m.heightScale));
	if (req.colorSet)
		e.Set("color", std::format("{:.3f},{:.3f},{:.3f}", m.baseColor.x, m.baseColor.y,
								   m.baseColor.z));

	cat->Add(std::move(e));
	// An IMPORT brought a new asset into the pool; the pool is gitignored, so
	// record where it came from before saving (both ride the same Save).
	if (req.source == AssetDialog::Source::Import) RecordImport(req);
	m_project.Save();
	log::Info("Created type '{}' in {}", req.name, req.catalogKey);

	// A surface type is only reachable once the level's palette lists it (the
	// palette IS the variant order) — otherwise "+ New" would drop the type into
	// the catalog and leave the brush unable to touch it.
	const MapEditor::PaletteCat pcat = MapEditor::CatForCatalogKey(req.catalogKey);
	if (MapEditor::SurfaceCat(pcat)) m_mapEditor.AddToPalette(pcat, req.name);
	else if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("newasset.created", req.name));
}

// Opens the per-type catalog editor for a palette row. The dialog edits a COPY
// of the entry's fields against its category's schema (CatalogSchema), so it
// needs nothing but the entry itself; Monsters additionally get the button
// through to their animation/behaviour dialog, which owns the rows the schema
// leaves out.
// A new entry in a pure-data catalog: a free id, the schema's defaults, and
// nothing else. Returns the id, or "" if the category has no catalog.
//
// THE ID IS GENERATED rather than asked for, and then renamed in the type
// editor. That is one naming mechanism instead of two, and the rename it reuses
// already carries the reference sweep — so a type named properly a minute after
// it was made is indistinguishable from one named at birth.
std::string Game::CreateAuthoredType(MapEditor::PaletteCat cat) {
	const std::string key = MapEditor::CategoryCatalogKey(cat);
	Catalog* catalog = m_project.CatalogForKey(key);
	if (!catalog) {
		log::Warn("new type: unknown catalog '{}'", key);
		return {};
	}
	// "<key minus its plural s><n>" — dungeon1, terrain1, quest1 — stepping
	// until one is free. Collision is checked rather than assumed: Catalog::Add
	// REPLACES by id, so a clash would silently overwrite a type.
	std::string stem(key);
	if (stem.size() > 1 && stem.back() == 's') stem.pop_back();
	std::string id;
	for (int n = 1;; ++n) {
		id = stem + std::to_string(n);
		if (!catalog->Contains(id)) break;
	}

	CatalogEntry e;
	e.id = id;
	for (const FieldSpec& f : SchemaFor(key))
		if (f.def && f.def[0]) e.Set(f.key, f.def);
	e.Set("display", id); // something readable until it is renamed
	catalog->Add(std::move(e));
	log::Info("new {} type '{}'", key, id);
	return id;
}

void Game::OpenTypeEditor(MapEditor::PaletteCat cat, const std::string& id) {
	const std::string key = MapEditor::CategoryCatalogKey(cat);
	const Catalog* catalog = m_project.CatalogForKey(key);
	const CatalogEntry* entry = catalog ? catalog->Find(id) : nullptr;
	if (!entry) {
		// A level palette can name an id its catalog doesn't define (hand-edited
		// map, or a foreign project) — there is nothing to edit.
		log::Warn("type editor: '{}' is not in {}.cat", id, key);
		if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.type.unknown", id));
		return;
	}
	TypeEditorDialog::Config cfg;
	cfg.catalogKey = key;
	cfg.categoryLabel = loc::Tr(MapEditor::CategoryNameKey(cat));
	cfg.id = id;
	cfg.fields = entry->fields;
	m_typeDialog.extraLabel = cat == MapEditor::PaletteCat::Monsters
								  ? loc::Tr("map.type.anims")
								  : std::string();
	// Duplicate is offered wherever a type can be authored at all — the same test
	// the palette's "+ New..." row uses, since the button opens that same dialog.
	m_typeDialog.duplicateLabel = MapEditor::CategoryPlaceable(cat)
									  ? loc::Tr("map.type.duplicate")
									  : std::string();
	// A DUNGEON's delete takes its levels with it, so it confirms by typing the
	// id (W10); everything else keeps the two-click arm. The label is looked up
	// per open so a language switch reaches it.
	m_typeDialog.typedDelete = key == "dungeons";
	m_typeDialog.typedDeleteLabel = loc::Tr("map.dungeon.delete.confirm");
	m_typeDialog.Open(std::move(cfg), SchemaFor(key));
}

// The monster type's animation + behaviour dialog (the type editor's extra
// button). It owns the states/anim_*/archetype/threat_* rows and rewrites them
// authoritatively, which is why the schema leaves them out.
void Game::OpenMonsterConfig(const std::string& id) {
	// Guard the force-load: a catalog id whose <model>.gltf is missing would
	// abort in LoadModelOrDie. Warn and skip instead of crashing the editor.
	if (!m_world->MonsterModelAvailable(id)) {
		log::Warn("monster config: '{}' has no loadable model — skipped", id);
		return;
	}
	const CatalogEntry* e = m_project.monsters.Find(id);
	const std::string display = e ? e->Display() : id;
	DungeonWorld::AnimSupport supported;
	DungeonWorld::AnimClips clips;
	m_world->MonsterAnimConfig(id, supported, clips);
	ai::Archetype archetype;
	float keepRange, fleeBelow;
	std::string spell;
	ThreatTuning threat;
	m_world->MonsterBehaviorConfig(id, archetype, keepRange, fleeBelow, spell, threat);
	m_monsterDialog.Open(id, display, supported, clips, archetype, keepRange, fleeBelow,
						 spell, threat, m_world->MonsterClipNames(id), m_world->SpellIds());
	m_previewType.clear(); // force the preview animator to (re)build on first frame
	m_previewClip.clear();
	m_previewMonMesh = nullptr;
	m_previewMonSubs.clear();
}

// One line of provenance per imported asset. The key is the POOL name (what a
// catalog's texture=/model= field binds to), not the catalog id, because several
// types can share one imported asset — the second and third bind it through the
// dialog's "Use installed", which imports nothing and records nothing.
void Game::RecordImport(const AssetDialog::CreateRequest& req) {
	CatalogEntry e;
	// Texture sets install under their resolution-tagged name (see StartBakeStep);
	// models keep theirs, and their PBR maps ride along as <name>_2k.
	e.id = req.textureSet ? req.name + "_2k" : req.name;
	e.Set("kind", req.textureSet ? "texture" : "model");
	// The source path verbatim. Machine-specific by nature — the replay script
	// knows how to re-root a path under the asset archive onto another machine,
	// which is where that knowledge already lives (FetchTextures.ps1).
	e.Set("source", req.sourcePath);
	if (req.flipGreen) e.Set("flip_green", "1");
	// A SURFACE set also has worn block meshes baked from it, and their geometry
	// is kind-specific (a wall panel is not a floor slab) while their FILE NAME
	// is not — worn_<set>_<tier>.gltf, one per set. So the replay has to know
	// which kind to bake, or baking "all three" would just overwrite twice.
	if (req.catalogKey == "walls" || req.catalogKey == "floors" ||
		req.catalogKey == "ceilings")
		e.Set("surface", req.catalogKey == "walls"      ? "wall"
						 : req.catalogKey == "floors" ? "floor"
													  : "ceiling");
	m_project.imports.Add(std::move(e));
}

// References to a type that live OUTSIDE the level files: another catalog
// entry's field, or a project default. Small and closed — every cross-catalog
// field in the project is listed here — so a rename can't quietly strand one.
int Game::SweepCatalogRefs(const std::string& catalogKey, const std::string& id,
						   const std::string* newId) {
	int hits = 0;
	// One field of one catalog naming an id of another. The matches are
	// collected before any write: Catalog::Add mutates the entry vector being
	// walked.
	const auto sweepField = [&](Catalog& cat, const char* field) {
		std::vector<std::string> matches;
		for (const CatalogEntry& e : cat.Entries())
			if (e.Get(field, "") == id) matches.push_back(e.id);
		hits += static_cast<int>(matches.size());
		if (!newId) return;
		for (const std::string& entryId : matches) {
			CatalogEntry copy = *cat.Find(entryId);
			copy.Set(field, *newId);
			cat.Add(std::move(copy)); // add-or-replace by id
		}
	};
	// A stair type names the type auto-authored on the other side.
	if (catalogKey == "stairs") sweepField(m_project.stairs, "pair");
	// A door names the KEY ITEM that unlocks it.
	if (catalogKey == "items") sweepField(m_project.doors, "key");
	// The 'T'/'F' map glyphs resolve through the project's default fixtures.
	if (catalogKey == "fixtures") {
		for (std::string* slot : {&m_project.defaultSconce, &m_project.defaultBrazier})
			if (*slot == id) {
				++hits;
				if (newId) *slot = *newId;
			}
	}
	// AN ITEM'S QUEST HOOK names a quest, and a quest's stages are named too —
	// but a STAGE rename is not a type rename and does not come through here.
	if (catalogKey == "quests")
		for (Catalog* c : {&m_project.items, &m_project.weapons, &m_project.armor})
			for (const CatalogEntry& e : c->Entries()) {
				const std::string q = e.Get("quest", "");
				const size_t colon = q.find(':');
				if (colon == std::string::npos || q.substr(0, colon) != id) continue;
				++hits;
				if (newId) {
					CatalogEntry copy = e;
					copy.Set("quest", *newId + q.substr(colon));
					c->Add(std::move(copy));
				}
			}

	// THE WORLD TIER, which the level sweep cannot see: a location names a
	// DUNGEON and a LEVEL, and an item may reveal a location. Without this the
	// sweep's promise — "a delete REFUSES while anything still references the
	// type" — stopped being true at exactly the tier where a dangling
	// reference is worst, because a broken doorway is not visible from any
	// level.
	if (m_worldMap && catalogKey == "dungeons") {
		// Dungeon(), NOT the raw field: an absent `dungeon` means "the same as
		// my id", so a location named after its dungeon references it just as
		// surely as one that says so. Checking the field alone would have
		// missed exactly the locations authored the short way.
		std::vector<std::string> doors;
		for (const WorldMap::Location& l : m_worldMap->Locations())
			if (l.Dungeon() == id) doors.push_back(l.id);
		hits += static_cast<int>(doors.size());
		// RENAMED, since W11. This used to say the rename was "reported and
		// refused" while the world was const — and RenameType never looked at
		// the count, so the rename WENT THROUGH and every doorway to the
		// dungeon was left naming one that did not exist. The world has been
		// mutable since W3. A location that named its dungeon the short way
		// (by sharing its id) gets the field written out: the location keeps
		// its own id, which is a different thing's name.
		if (newId)
			for (const std::string& door : doors)
				m_worldMap->MutableLocation(door)->dungeon = *newId;
	}
	// THE GAME'S OPENING names a dungeon too, in the manifest (saved with the
	// catalogs by the caller).
	if (catalogKey == "dungeons" && m_project.startDungeon == id) {
		++hits;
		if (newId) m_project.startDungeon = *newId;
	}
	// TERRAIN IS NOT SWEPT, and that is a property of the format rather than an
	// omission: the world grid names a terrain by its GLYPH, so renaming the
	// id cannot orphan a cell. It is why terrain declares a glyph at all.
	return hits;
}

std::vector<std::string> Game::SavesReferencingType(const std::string& id) const {
	std::vector<std::string> names;
	for (const SaveSlot& slot : ListSaves()) {
		if (slot.world != m_project.FolderName()) continue; // another world's ids
		const std::optional<SaveData> data = ReadSave(slot.path);
		if (!data) continue;
		bool hit = false;
		for (const SaveData::LevelState& level : data->levels)
			for (const SaveData::EntityState& e : level.entities)
				// Only a SPAWN row carries a type; a diff references its .ent
				// baseline by id, which the level sweep already retyped.
				if (e.id < 0 && e.type == id) { hit = true; break; }
		if (hit) names.push_back(slot.name);
	}
	return names;
}

// Renames a type everywhere it is named. The level sweep is the big one (every
// level, including those not in memory); the catalog/project references are the
// long tail. Live objects are re-spawned from the retyped records afterwards, so
// what is on screen matches what was written.
bool Game::RenameType(const std::string& catalogKey, const std::string& id,
					  const std::string& newId, std::string& problem) {
	Catalog* cat = m_project.CatalogForKey(catalogKey);
	if (!cat || !cat->Find(id)) return false;
	if (newId.empty() || newId == id) return false;
	if (cat->Contains(newId)) {
		problem = loc::Format("newasset.err.dup", newId);
		return false;
	}
	// The entry itself, renamed WHERE IT SITS — a remove + re-add would drop it
	// at the end of the file and take its lead comments (the first entry's are
	// the file's header) with it.
	if (!cat->Rename(id, newId)) return false;
	// The world is compared whole rather than trusting a count: it is written
	// only when the sweep actually changed it, and then NOW, with the catalogs —
	// a renamed dungeon whose doorways still named the old id on disk would
	// open onto nothing at the next launch.
	const std::string worldBefore = m_worldMap ? m_worldMap->Serialize() : std::string();
	SweepCatalogRefs(catalogKey, id, &newId);
	if (!m_project.Save()) log::Warn("rename type: failed to save catalogs");
	if (m_worldMap && m_worldMap->Serialize() != worldBefore && !SaveWorld())
		log::Warn("rename type: failed to save the world");

	const DungeonWorld::TypeUsage used = m_world->SweepTypeRefs(catalogKey, id, &newId);
	// Live objects still point at kinds cached under the old id (and monsters
	// hold their type by name), so rebuild them from the records we just wrote.
	m_world->RespawnFromRecords(catalogKey == "wallfeatures");
	// The undo stack holds level snapshots taken BEFORE the rename; restoring
	// one would bring back records naming a type that no longer exists.
	m_world->ClearUndoHistory();
	log::Info("Renamed type '{}' -> '{}' ({} record(s) in {} level(s))", id, newId,
			  used.count, used.levels.size());
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.type.renamed", id, newId, used.count));
	WarnStaleSaves(id);
	return true;
}

// Saves are not swept (see SavesReferencingType) — say so when any of them
// still name the type, so the surprise happens here and not at the next load.
void Game::WarnStaleSaves(const std::string& id) {
	const std::vector<std::string> saves = SavesReferencingType(id);
	if (saves.empty()) return;
	std::string list;
	for (const std::string& name : saves)
		list += (list.empty() ? "" : ", ") + name;
	log::Warn("Save file(s) still reference type '{}': {}", id, list);
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.type.stalesaves", id, list));
}

// Deletes a type — but only an UNUSED one. A record naming a missing type is
// not a soft failure: the level loaders reject or abort on it, so the safe rule
// is to refuse and say which levels still use it.
bool Game::DeleteType(const std::string& catalogKey, const std::string& id,
					  std::string& problem) {
	Catalog* cat = m_project.CatalogForKey(catalogKey);
	if (!cat || !cat->Contains(id)) return false;
	// An effect is defined by its CLASS; the catalog entry only tunes it. So
	// deleting the entry would not remove the effect — it would silently revert
	// it to its class defaults, which is not what a Delete button promises.
	// Refuse, and say why (docs/effects.md).
	if (catalogKey == "effects") {
		problem = loc::Tr("map.type.classbacked");
		return false;
	}
	// A DUNGEON TAKES ITS LEVELS WITH IT (W10), so it is not a catalog delete
	// with a bigger sweep — it has rules of its own and files to remove.
	if (catalogKey == "dungeons") {
		problem = DungeonDeleteRefusal(id);
		if (!problem.empty()) return false;
		if (DeleteDungeon(id)) return true;
		problem = loc::Format("map.dungeon.delete.failed", id);
		return false;
	}
	const DungeonWorld::TypeUsage used = m_world->SweepTypeRefs(catalogKey, id);
	if (used.Any()) {
		std::string levels;
		for (const std::string& stem : used.levels)
			levels += (levels.empty() ? "" : ", ") + stem;
		problem = loc::Format("map.type.inuse", used.count, levels);
		return false;
	}
	if (const int refs = SweepCatalogRefs(catalogKey, id, nullptr); refs > 0) {
		problem = loc::Format("map.type.inuse.catalog", refs);
		return false;
	}
	cat->Remove(id);
	if (!m_project.Save()) log::Warn("delete type: failed to save catalogs");
	log::Info("Deleted type '{}' from {}", id, catalogKey);
	if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.type.deleted", id));
	WarnStaleSaves(id); // a save's spawn rows are outside the level sweep
	return true;
}

// --- deleting a dungeon (W10) ------------------------------------------------
// Michael's answer (2026-09-09): delete its LEVELS too, after a confirmation
// that names them. It is the one editor action no undo reaches — the history is
// in memory and the files are not — so the rules run BEFORE the confirmation
// opens, and each refusal is its own sentence naming what is in the way (W4's
// lesson: one sentence for two refusals hid one of them).

std::string Game::DungeonDeleteRefusal(const std::string& id) {
	if (!m_project.dungeons.Contains(id))
		return loc::Format("map.dungeon.missing", id);
	const std::vector<std::string> levels = m_project.DungeonLevels(id);
	const auto dying = [&](const std::string& stem) {
		return std::find(levels.begin(), levels.end(), stem) != levels.end();
	};
	// THE PARTY'S LEVEL is on screen and is the world's live state, not a file
	// — the world-delete rule one tier down.
	if (dying(m_world->CurrentLevel()))
		return loc::Format("map.dungeon.delete.party", m_world->CurrentLevel());
	// THE GAME'S OPENING and THE HARNESS'S GROUND are references in the
	// manifest that no level or location shows. A new game landing in a
	// deleted level would abort; so would every eval suite.
	if (m_project.startDungeon == id || dying(m_project.startLevel))
		return loc::Format("map.dungeon.delete.opening",
						   m_project.startLevel.empty() ? id : m_project.startLevel);
	if (dying(m_project.evalLevel))
		return loc::Format("map.dungeon.delete.eval", m_project.evalLevel);
	// A DOORWAY that leads here — by its dungeon, or by naming one of these
	// levels outright. Deleting behind it would leave a location on the world
	// map that opens onto nothing. The doorway is the WORLD's to remove.
	if (m_worldMap) {
		std::string doors;
		int count = 0;
		for (const WorldMap::Location& l : m_worldMap->Locations())
			if (l.Dungeon() == id || dying(l.level)) {
				doors += (doors.empty() ? "" : ", ") + l.id;
				++count;
			}
		if (count > 0) return loc::Format("map.dungeon.delete.doorway", count, doors);
	}
	// A LEVEL TWO DUNGEONS CLAIM would go from the other one too. The checker
	// already reports it as an error; the delete is not the place to settle it.
	for (const std::string& stem : levels)
		for (const CatalogEntry& other : m_project.dungeons.Entries())
			if (other.id != id) {
				const std::vector<std::string> theirs = m_project.DungeonLevels(other.id);
				if (std::find(theirs.begin(), theirs.end(), stem) != theirs.end())
					return loc::Format("map.dungeon.delete.shared", stem, other.id);
			}
	// A STAIR FROM OUTSIDE leading in. The plan's refusal: better than
	// deleting and reporting the wreckage afterwards.
	const std::vector<DungeonWorld::StairInto> stairs = m_world->StairsInto(levels);
	if (!stairs.empty()) {
		const DungeonWorld::StairInto& s = stairs.front();
		return loc::Format("map.dungeon.delete.stair", s.fromLevel, s.x, s.z,
						   s.destLevel, stairs.size());
	}
	return {};
}

std::vector<std::string> Game::DescribeDungeon(const std::string& id) const {
	const CatalogEntry* e = m_project.dungeons.Find(id);
	if (!e) return {};
	const std::string display = CatalogGet(e, "display", id);
	const std::vector<std::string> levels = m_project.DungeonLevels(id);
	// BY NAME AND BY COUNT — the plan's words. "Are you sure?" asks you to
	// remember what is in it; this tells you.
	std::vector<std::string> lines;
	lines.push_back(levels.empty()
						? loc::Format("map.dungeon.delete.what0", display, id)
						: loc::Format("map.dungeon.delete.what", display, id,
									  levels.size()));
	for (const std::string& stem : levels)
		lines.push_back(loc::Format("map.dungeon.delete.level", stem));
	return lines;
}

bool Game::DeleteDungeon(const std::string& id) {
	// Re-asked here, not trusted from the dialog: the console reaches this
	// too, and the world may have changed since the confirmation opened.
	if (const std::string why = DungeonDeleteRefusal(id); !why.empty()) {
		log::Warn("delete dungeon '{}' refused: {}", id, why);
		return false;
	}
	const std::vector<std::string> levels = m_project.DungeonLevels(id);

	// THE MANIFEST AND THE CATALOG FIRST, the files after. A failure between
	// the two then leaves stray files nothing names (harmless, logged) rather
	// than a manifest naming files that are gone (a level that aborts on load).
	std::erase_if(m_project.levels, [&](const std::string& stem) {
		return std::find(levels.begin(), levels.end(), stem) != levels.end();
	});
	m_project.dungeons.Remove(id);
	if (!m_project.Save()) {
		log::Warn("delete dungeon '{}': the project did not save - files kept", id);
		return false;
	}
	bool filesOk = true;
	for (const std::string& stem : levels) filesOk &= m_world->DeleteLevel(stem);

	// The history holds copies of these levels: an undo would put them back
	// in memory, and the next savemap would write them back to disk.
	m_world->ClearUndoHistory();
	// A viewport browsing one of them would be showing a level that is gone.
	if (std::find(levels.begin(), levels.end(), m_mapView.ViewedLevel()) !=
		levels.end())
		m_mapView.SetViewLevel(m_world->CurrentLevel());

	log::Info("Deleted dungeon '{}' and {} level(s){}", id, levels.size(),
			  filesOk ? "" : " (some files could not be removed - see above)");
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.dungeon.deleted", id, levels.size()));
	WarnSavesInLevels(levels);
	return filesOk;
}

// Saves are not swept (the rename rule): say which ones stand in, or carry
// state for, a level that is gone, so the surprise is here and not at a load.
void Game::WarnSavesInLevels(const std::vector<std::string>& stems) {
	const auto gone = [&](const std::string& stem) {
		return std::find(stems.begin(), stems.end(), stem) != stems.end();
	};
	std::string list;
	for (const SaveSlot& slot : ListSaves()) {
		if (slot.world != m_project.FolderName()) continue; // another world's levels
		const std::optional<SaveData> data = ReadSave(slot.path);
		if (!data) continue;
		bool hit = gone(data->currentLevel);
		for (const SaveData::LevelState& level : data->levels)
			hit = hit || gone(level.stem);
		if (hit) list += (list.empty() ? "" : ", ") + slot.name;
	}
	if (list.empty()) return;
	log::Warn("Save file(s) still reference deleted level(s): {}", list);
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.dungeon.stalesaves", list));
}

// Type editor Save: merge the dialog's working fields into the catalog entry.
// Starts from the EXISTING entry so anything the schema doesn't cover — a
// hand-authored field, or MonsterConfigDialog's states/anim_* rows — survives,
// and an empty value REMOVES the field (absent means "the loader's default",
// which is not the same as an empty string).
void Game::WriteTypeFields(const TypeEditorDialog::Config& cfg) {
	Catalog* cat = m_project.CatalogForKey(cfg.catalogKey);
	if (!cat) {
		log::Warn("type editor: unknown catalog '{}'", cfg.catalogKey);
		return;
	}
	CatalogEntry entry;
	if (const CatalogEntry* e = cat->Find(cfg.id)) entry = *e;
	else entry.id = cfg.id;
	for (const serialize::Field& f : cfg.fields) {
		if (f.value.empty()) {
			std::erase_if(entry.fields, [&](const serialize::Field& g) {
				return g.key == f.key;
			});
			continue;
		}
		entry.Set(f.key, f.value);
	}
	cat->Add(std::move(entry)); // add-or-replace by id
	if (!m_project.Save())
		log::Warn("type editor: failed to save project catalogs");
}

// Kicks the async worn-mesh rebake for a Surface Style Save: reuses the
// asset-bake subprocess flow, jumping straight to the wornblock step.
// m_restyleBake tells the Update poll to reload the dungeon blocks (not
// FinishBake) on success.
void Game::StartRestyleBake(const std::string& catalogKey, const std::string& texture,
						   float wear, float relief) {
	if (m_baking) {
		log::Warn("surface style: a bake is already running — try again in a moment");
		return;
	}
	m_bakeReq = {};              // a wornblock-only bake, no CreateRequest data
	m_bakeReq.textureSet = true; // routes StartBakeStep to the wornblock branch
	m_bakeReq.catalogKey = catalogKey; // picks wall/floor/ceiling in StartBakeStep
	m_bakeReq.name = texture;
	m_bakeStep = 1;               // skip the texture-import step
	m_bakeWear = wear;
	m_bakeRelief = relief;
	m_restyleBake = true;
	if (StartBakeStep())
		m_baking = true;
	else {
		log::Warn("wall style: could not launch AssetBaker");
		m_restyleBake = false;
	}
}

void Game::WriteMonsterAnim(const MonsterConfigDialog::Config& cfg) {
	// Start from the existing entry so every non-animation field (display, model,
	// hp, ...) is preserved; a brand-new type gets a bare entry.
	CatalogEntry entry;
	if (const CatalogEntry* e = m_project.monsters.Find(cfg.type)) entry = *e;
	else entry.id = cfg.type;
	// Drop the rows this dialog owns, then rewrite them authoritatively.
	std::erase_if(entry.fields, [](const serialize::Field& f) {
		return f.key == "states" || f.key.starts_with("anim_") || f.key == "archetype" ||
			   f.key == "keeprange" || f.key == "fleebelow" || f.key == "spell" ||
			   f.key == "threat_scale" || f.key == "threat_threshold" ||
			   f.key == "threat_switch" || f.key == "threat_decay";
	});

	// Behaviour fields (Behavior tab). archetype is always written; the params are
	// written only when they apply / are non-default, to keep the .cat tidy.
	static const char* kArch[] = {"brute",  "skirmisher", "caster",
								  "swarm", "lurker",     "sentry"};
	entry.Set("archetype", kArch[static_cast<int>(cfg.archetype)]);
	if (cfg.archetype == ai::Archetype::Skirmisher || cfg.archetype == ai::Archetype::Caster)
		entry.Set("keeprange", std::format("{:g}", cfg.keepRange));
	if (cfg.fleeBelow > 0.0f) entry.Set("fleebelow", std::format("{:g}", cfg.fleeBelow));
	if (cfg.archetype == ai::Archetype::Caster && !cfg.spell.empty())
		entry.Set("spell", cfg.spell);
	// Threat multipliers: write only the ones nudged off 1 (keep the .cat tidy).
	auto setThreat = [&](const char* key, float v) {
		if (v != 1.0f) entry.Set(key, std::format("{:g}", v));
	};
	setThreat("threat_scale", cfg.threat.scale);
	setThreat("threat_threshold", cfg.threat.threshold);
	setThreat("threat_switch", cfg.threat.switchMargin);
	setThreat("threat_decay", cfg.threat.decay);

	auto join = [](const std::vector<std::string>& v) {
		std::string out;
		for (const std::string& s : v) { if (!out.empty()) out += ' '; out += s; }
		return out;
	};
	std::vector<std::string> stateTokens;
	for (int i = 0; i < anim::kCreatureStateCount; ++i)
		if (cfg.supported[i])
			stateTokens.emplace_back(anim::StateName(static_cast<anim::CreatureState>(i)));
	entry.Set("states", join(stateTokens));
	for (int i = 0; i < anim::kCreatureStateCount; ++i) {
		if (cfg.clips[i].empty()) continue;
		const auto s = static_cast<anim::CreatureState>(i);
		entry.Set("anim_" + std::string(anim::StateName(s)), join(cfg.clips[i]));
	}

	m_project.monsters.Add(std::move(entry)); // add-or-replace by id
	if (!m_project.Save())
		log::Warn("monster config: failed to save project catalogs");
	else if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.cfg.saved", cfg.type));
}

// Runs one queued task per rendered frame (never before the current loading
// screen has been presented once); returns true when the queue is done.

} // namespace dungeon::game
