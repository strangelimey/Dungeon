// ============================================================================
// Game/Game_Editor.cpp — split out of Game.cpp to keep files small (see Game.h).
// Asset-bake flow + editor level create/rename/sync + catalog writes.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Assets/ImportFiles.h"
#include "Assets/WornSets.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StringUtil.h"
#include "Game/AssetUtil.h"
#include "Game/Serialize.h"
#include "Game/Style.h"
#include "Game/StyleLook.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
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

// Launches the AssetBaker command for the current bake step (P4c). Models are a
// single import-model; texture sets import the maps (step 0) then rebake the
// worn block meshes that sample them (step 1).
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
		// GL-convention normals (green up) need flipping. The dialog's checkbox
		// starts as the filename's guess (Assets/PbrMaps.h) and is ALWAYS sent, on
		// or off (code-review C393): with only --flip-green to say, unticking a
		// wrong guess left the baker to make the same guess again.
		cmd += m_bakeReq.flipGreen ? " --flip-green" : " --no-flip-green";
	} else {
		// Bake worn block meshes for just the new set (its kind = the catalog).
		// An installed set only gets here with no meshes yet and nobody painting
		// it as another kind (AdoptSurfaceSet).
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
	// directory. imports.cat says exactly which, and assets::ImportOwnsFile
	// names each record's files exactly (code-review C336): a texture set's map
	// trio and the worn meshes baked from it - worn_<set>_<tier>, by the set's
	// BASE, where this looked for worn_<set>_2k* and copied none - and a model's
	// file, its sidecars and the maps it brought in as <name>_2k. Never by a
	// bare prefix, which took pottery's and potion's files for `pot`'s.
	int copied = 0;
	for (const CatalogEntry& e : m_project.imports.Entries()) {
		const bool model = e.Get("kind", "texture") == "model";
		for (const auto& [dir, folder] : {std::pair{assets::PoolDir::Textures, "textures"},
										  std::pair{assets::PoolDir::Models, "models"}}) {
			const fs::path from = fs::path(paths::AssetsDir()) / folder;
			const fs::path to = fs::path(repo) / folder;
			fs::create_directories(to, ec);
			for (const auto& entry : fs::directory_iterator(from, ec)) {
				if (ec || !entry.is_regular_file()) continue;
				const std::string name = str::Narrow(entry.path().filename().wstring());
				if (!assets::ImportOwnsFile(e.id, model, dir, name)) continue;
				std::error_code copyEc;
				fs::copy_file(entry.path(), to / entry.path().filename(),
							  fs::copy_options::overwrite_existing, copyEc);
				if (!copyEc) ++copied;
			}
		}
	}
	log::Info("Synced project {} -> source ({} imported asset file(s))",
			  src.filename().string(), copied);
	return true;
}

// The minimal 16x16 box a new world's first room and an empty new level both
// start from: rock, a 3x3 room in the middle and the start at its centre.
// Appended as grid rows, after the caller's palette records. FIXED on purpose:
// scenarios and habits build on the room being at 7..9 (see CreateNewLevel).
void Game::AppendStarterRoom(std::string& map) {
	const std::vector<u8> floor = StarterFloor();
	for (int z = 0; z < kStarterSize; ++z) {
		for (int x = 0; x < kStarterSize; ++x) {
			const bool room = floor[static_cast<size_t>(z) * kStarterSize + x] != 0;
			map += !room ? '#' : (x == kStarterCentre && z == kStarterCentre) ? 'P' : '.';
		}
		map += '\n';
	}
}

std::vector<u8> Game::StarterFloor() {
	std::vector<u8> floor(static_cast<size_t>(kStarterSize) * kStarterSize, 0);
	for (int z = kStarterCentre - 1; z <= kStarterCentre + 1; ++z)
		for (int x = kStarterCentre - 1; x <= kStarterCentre + 1; ++x)
			floor[static_cast<size_t>(z) * kStarterSize + x] = 1;
	return floor;
}

std::string Game::StyledStarterRecords(const Project& project, const std::string& styleId,
									   std::array<std::vector<std::string>, 3>& palettes) {
	const CatalogEntry* style = stylelook::Find(project, styleId);
	if (!style) return {};
	const stylelook::Look look =
		stylelook::Lay(project, *style, StarterFloor(), kStarterSize, kStarterSize);
	palettes = stylelook::Palettes(look, palettes);
	std::string out;
	if (const std::vector<std::string> tags = stylelook::Tags(*style); !tags.empty()) {
		out = "tags";
		for (const std::string& t : tags) out += " " + t;
		out += "\n";
	}
	return out + look.records;
}

// --- worlds (W7, docs/world-editor-plan.md) ---------------------------------
// A world IS a project folder (Michael's word for one), and creating a new one
// is a file operation rather than a live edit: nothing about the running game
// changes until it is opened - in the process, since docs/world-on-demand.md
// (it used to relaunch). Making one lives in Game_NewWorld.cpp.

std::string Game::WorldSwitchRefusal() const {
	// A BAKE LANDS IN WHATEVER WORLD IS LOADED (code-review C234): FinishBake
	// writes its catalog entry - and an import its imports.cat record - when the
	// AssetBaker exits, into m_project as it is then. Switched under it, the new
	// type went into the next world, replacing any type of the same id there.
	// Only the console reaches this while a bake runs (the dialog is busy and
	// modal), so it is refused rather than queued: the bake takes seconds.
	if (m_baking)
		return std::format("an asset bake is running ('{}') - switch worlds once it lands, "
						   "or it lands in the next one",
						   m_bakeReq.name);
	return {};
}

bool Game::SwitchWorld(const std::string& name, std::string* why) {
	const auto refuse = [&](std::string reason) {
		log::Warn("switch to world '{}' refused: {}", name, reason);
		if (why) *why = std::move(reason);
		return false;
	};
	const std::string root = paths::Asset("projects");
	const std::vector<std::string> found = Project::List(root);
	if (std::find(found.begin(), found.end(), name) == found.end())
		return refuse(std::format("no world '{}' under {}", name, root));
	// ALREADY THERE means the world LOADED, not the one the setting names —
	// a `-project` run leaves the setting alone, so comparing against it
	// refused to leave a scenario for the world settings.ini already held.
	if (m_world && name == m_project.FolderName()) return true;
	if (std::string busy = WorldSwitchRefusal(); !busy.empty()) return refuse(std::move(busy));
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
	SwitchWorld(folder); // a refusal (gone, or a bake running) logs its reason
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

// Mints a fresh level: writes a .map/.ent pair next to the project's other
// levels and appends the stem to the manifest and its dungeon. A GENERATED level
// (`params`) is built round a stair square on the dungeon's floor above and
// linked to it (LinkToFloorAbove); its palettes come from the dialog's donor
// level, else the active one (ComposeGeneratedLevel). The EMPTY box gets no
// stair, and its three palette records - the palette gate demands all three -
// are the ACTIVE level's. Either way a style's themes lead where it names
// surfaces. Everything downstream (browse, remote edits, stair dests, savemap)
// reads Project::levels or lazy-parses the files, so no other state needs
// touching. Returns the stem, or "" on failure.
std::string Game::CreateNewLevel(const std::string& dungeonId,
								 const generate::Params* params, const std::string& emptyStyle) {
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
		// The tags the content pools are drawn by: the viewed level's own,
		// else the dungeon's flavour tags - a fresh dungeon's first generated
		// floor has no level tags to inherit, and "undead crypt" should still
		// fill with undead.
		// A tag CHOSEN in the dialog (P4b) wins over both, and the STYLE's tags
		// (Phase 7) over the level's and the dungeon's.
		std::vector<std::string> fallback = m_mapView.ViewedMap().Tags();
		if (fallback.empty() && dungeon) fallback = ParseTags(dungeon->Get("tags", ""));
		linkCells = ComposeGeneratedLevel(stem, p, TagsFor(p, m_project, fallback), map, ent);
	} else {
		auto join = [](const std::vector<std::string>& ids) {
			std::string out;
			for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
			return out;
		};
		const DungeonMap& live = m_world->Map(); // active level: the palette donor
		// In a STYLE (Phase 7) the box wears its room theme: its palettes lead
		// where it names surfaces, and its tags come with it.
		std::array<std::vector<std::string>, 3> palettes{live.WallPalette(), live.FloorPalette(),
														 live.CeilingPalette()};
		const std::string styled = StyledStarterRecords(m_project, emptyStyle, palettes);
		map = "; " + stem + " - created in the editor.\n";
		map += "palette wall " + join(palettes[0]) + "\n";
		map += "palette floor " + join(palettes[1]) + "\n";
		map += "palette ceiling " + join(palettes[2]) + "\n";
		map += styled + "\n";
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
	m_world->NoteEdit(); // a new level joins the checked set
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
	// The bake made the model; the entry is still refused if its category could
	// not load what it made - and the form stays up saying so (SetError ends the
	// busy state too).
	if (const std::string refused = CreateCatalogEntry(m_bakeReq); !refused.empty()) {
		m_assetDialog.SetError(refused);
		return;
	}
	m_assetDialog.SetBusy(false);
	m_assetDialog.Close();
}

// Writes the new type's catalog entry and makes it reachable. The entry's SHAPE
// comes from the category's schema (CatalogSchema): every row with a default is
// seeded, so a new stair gets its up/pair/hole rows and a new item its
// weight/holdable — where the old one-shape-fits-all writer stamped
// authored=1/solid=1 on everything and left doors, stairs and items broken.
std::string Game::CreateCatalogEntry(const AssetDialog::CreateRequest& req) {
	Catalog* cat = m_project.CatalogForKey(req.catalogKey);
	if (!cat) {
		log::Warn("asset create: unknown catalog '{}'", req.catalogKey);
		return loc::Format("map.type.unknown", req.catalogKey);
	}
	// AN ID A RELATED CATALOG HAS is refused (code-review C302) - the form says
	// so first; this is the check a pick gone stale, or a console create, meets.
	// Catalog::Add would accept it, and one item id in two item catalogs is half
	// unreachable (FindItem finds one).
	if (const std::string why = RelatedIdRefusal(req.catalogKey, req.name); !why.empty()) {
		log::Warn("create {} '{}' refused: {}", req.catalogKey, req.name, why);
		return why;
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

	// THE ENTRY AS IT WOULD BE WRITTEN must load its model (code-review C301):
	// the form judged the pick when it was made, but a pick can go stale, a
	// duplicate's source may name none (its new id is then the model's name),
	// and an import's model is whatever its bake made. Nothing is written.
	if (const std::string why = UnloadableModelReason(req.catalogKey, e); !why.empty()) {
		log::Warn("create {} '{}' from '{}' refused: {}", req.catalogKey, req.name, req.asset,
				  why);
		return why;
	}

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
	// A new ITEM is in the catalog but not yet in the UI's banks - its icon,
	// weight, holdable and wear - until they are rebuilt (code-review C330).
	if (Project::IsItemCatalog(req.catalogKey)) RefreshItemBanks();
	return {};
}

std::string Game::RelatedIdRefusal(const std::string& catalogKey, const std::string& id) const {
	const std::string_view other = m_project.RelatedCatalogUsing(catalogKey, id);
	if (other.empty()) return {};
	return loc::Format("newasset.err.related", id,
					   loc::Tr(MapEditor::CategoryNameKey(MapEditor::CatForCatalogKey(other))));
}

std::string Game::UnloadableModelReason(const std::string& catalogKey,
										const CatalogEntry& e) const {
	const std::optional<ModelFamily> family = ModelFamilyOf(catalogKey);
	if (!family) return {}; // this catalog's types load no model
	if (const std::optional<UnloadableModel> miss = FirstUnloadableModel(*family, e))
		return loc::Format("map.type.nomodel", miss->name, miss->field);
	// A door's trim naming no doors.cat entry: SpawnDoor draws it through
	// DecorationKindFor, which opens the NAME as a model file (levelcheck's rule
	// for it). One that is an entry is that entry's own business.
	if (catalogKey == "doors")
		if (const std::string trim = e.Get("trim", "");
			!trim.empty() && !m_project.doors.Contains(trim) &&
			!ModelFileInstalled(ModelFileOf(ModelFamily::Prop, nullptr, trim)))
			return loc::Format("map.type.nomodel", trim, "trim");
	return {};
}

std::string Game::TypeSaveRefusal(const std::string& catalogKey,
								  const CatalogEntry& merged) const {
	if (std::string why = UnloadableModelReason(catalogKey, merged); !why.empty()) return why;
	// A terrain glyph the world could not be read with (code-review C345): the
	// Save writes terrain.cat at once, and Load asserts on the next launch.
	if (catalogKey == "terrain") return TerrainSaveRefusal(merged);
	return {};
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
		if (!catalog->Contains(id) && m_project.RelatedCatalogUsing(key, id).empty()) break;
	}

	CatalogEntry e;
	e.id = id;
	for (const FieldSpec& f : SchemaFor(key))
		if (f.def && f.def[0]) e.Set(f.key, f.def);
	e.Set("display", id); // something readable until it is renamed
	// A new THEME starts as the look of the square selected on the map
	// (right-click it first): its floor and ceiling as the square shows them,
	// and the wall of its first solid neighbour. Picking a room you like and
	// saying "that, as a theme" is the quickest way to author one; with
	// nothing selected it starts empty and the tabs fill it.
	if (key == "themes" && m_mapEditor.HasSelection()) {
		const DungeonMap& map = m_mapView.ViewedMap();
		const int x = m_mapEditor.SelX(), z = m_mapEditor.SelZ();
		const auto shown = [&](Surface s, int cx, int cz) -> std::string {
			const std::vector<std::string>& pal = map.Palette(s);
			if (pal.empty()) return {};
			return pal[ResolveSurfaceVariant(map, cx, cz, s, static_cast<u32>(pal.size()))];
		};
		if (map.IsWalkable(x, z)) {
			e.Set("floor", shown(Surface::Floor, x, z));
			e.Set("ceiling", shown(Surface::Ceiling, x, z));
			for (const auto [dx, dz] : {std::pair{0, -1}, {1, 0}, {0, 1}, {-1, 0}})
				if (!map.IsWalkable(x + dx, z + dz)) {
					e.Set("wall", shown(Surface::Wall, x + dx, z + dz));
					break;
				}
		} else {
			e.Set("wall", shown(Surface::Wall, x, z));
		}
	}
	// A new FLAG starts local to the dungeon being viewed - the palette lists
	// that dungeon's flags first, and most flags are one dungeon's business.
	// Clearing `dungeon` in the editor makes it a world flag.
	if (key == "flags")
		if (const CatalogEntry* d = m_project.DungeonOfLevel(m_mapView.ViewedLevel()))
			e.Set("dungeon", d->id);
	// A new TERRAIN gets a glyph of its own (code-review C345). It used to get
	// none, which reads as '?', so the second new kind shared the first's and
	// the next launch aborted on the clash. Taken = every kind's glyph as the
	// world reads it.
	if (key == "terrain") {
		std::string taken;
		for (const CatalogEntry& t : catalog->Entries()) {
			const std::string g = t.Get("glyph", "");
			taken += g.size() == 1 ? g[0] : '?';
		}
		const char glyph = WorldMap::FreeGlyph(taken);
		if (glyph == '\0') {
			log::Warn("new terrain: every glyph is taken - no new kind can be told apart");
			return {};
		}
		e.Set("glyph", std::string(1, glyph));
	}
	catalog->Add(std::move(e));
	log::Info("new {} type '{}'", key, id);
	// ...and is SAVED AND HANDED TO THE WORLD at once, so the brush can paint it.
	// Once a square is painted with it, world.map names its glyph, and a catalog
	// on disk without the kind would leave that file unreadable.
	if (key == "terrain") {
		if (!m_project.Save()) log::Warn("new terrain: failed to save the catalogs");
		SyncWorldTerrains();
	}
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
	// The per-category extra button: a monster's animations, a style's way back
	// to the shared library.
	m_typeDialog.extraLabel = cat == MapEditor::PaletteCat::Monsters ? loc::Tr("map.type.anims")
							  : cat == MapEditor::PaletteCat::Styles ? loc::Tr("map.style.savelib")
																	 : std::string();
	m_typeDialog.extraIcon = cat == MapEditor::PaletteCat::Styles ? "source" : "anim";
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
	// An effect's (or a spell's, an attack's) id is the code's: no rename
	// affordance and no Delete, rather than two controls that only refuse (C306).
	m_typeDialog.fixedIdentity = Project::IdentityInCode(key);
	m_typeDialog.Open(std::move(cfg), SchemaFor(key));
}

std::vector<BalanceDialog::EffectRow> Game::EffectRows() const {
	std::vector<BalanceDialog::EffectRow> rows;
	for (const CatalogEntry& e : m_project.effects.Entries()) {
		// `name` is a loc key (the sheet appends .desc for the long form); an
		// entry with none shows its id, which is what the palette used to show.
		const std::string key = e.Get("name", "");
		rows.push_back({e.id, key.empty() ? e.id : loc::Tr(key), e.Get("stacking", "refresh")});
	}
	return rows;
}

void Game::OpenBalanceDialog() {
	m_balanceDialog.SetEffects(EffectRows());
	m_balanceDialog.Open(m_world->GetBalance());
}

// The monster type's animation + behaviour dialog (the type editor's extra
// button). It owns the states/anim_*/archetype/threat_* rows and rewrites them
// authoritatively, which is why the schema leaves them out.
void Game::OpenMonsterConfig(const std::string& id) {
	// Guard the force-load: a catalog id whose model file is missing would
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
	ForgetMonsterPreview(); // the preview animator (re)builds on the first frame
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
	// The green flip the import was MADE with, on or off: the baker was told
	// one (StartBakeStep), so a replay must be too, rather than guessing from
	// the filename again (C393).
	if (req.textureSet) e.Set("flip_green", req.flipGreen ? "1" : "0");
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

// --- "Use installed" on a surface (code-review C407) --------------------------
// A set's worn meshes are worn_<set>_<tier>.gltf: named by the SET alone, shared
// by every world, with the KIND in the geometry. So adopting an installed set as
// a surface is a question about everyone who already paints with it, not about
// the catalog being added to. It used to always bake, as the NEW type's kind:
// a floor made from a wall set turned every wall of it into floor, and a second
// type of the same kind re-baked the set at its own values under the first.

namespace {
std::optional<assets::WornKind> SurfaceKindOf(std::string_view catalogKey) {
	if (catalogKey == "walls") return assets::WornKind::Wall;
	if (catalogKey == "floors") return assets::WornKind::Floor;
	if (catalogKey == "ceilings") return assets::WornKind::Ceiling;
	return std::nullopt;
}

const char* KindKey(assets::WornKind kind) {
	switch (kind) {
	case assets::WornKind::Floor: return "newasset.kind.floor";
	case assets::WornKind::Ceiling: return "newasset.kind.ceiling";
	default: return "newasset.kind.wall";
	}
}
} // namespace

Game::SurfaceAdopt Game::AdoptSurfaceSet(const std::string& catalogKey,
										 const std::string& set) const {
	SurfaceAdopt out;
	const std::optional<assets::WornKind> kind = SurfaceKindOf(catalogKey);
	if (!kind || set.empty()) return out;
	const auto refuse = [&](assets::WornKind theirs, const std::string& who) {
		out.refusal = loc::Format("newasset.err.wornkind", set, loc::Tr(KindKey(theirs)), who);
	};
	// A SHIPPED set's record is its kind for life (`AssetBaker wornblock` refuses
	// it as another).
	if (const assets::WornSet* s = assets::FindShippedWornSet(set); s && s->kind != *kind) {
		refuse(s->kind, loc::Tr("newasset.who.shipped"));
		return out;
	}
	// A TYPE painting with it as another kind - its `texture`, else its own id,
	// the rule the world loads it by.
	const auto boundAs = [&](const Catalog& walls, const Catalog& floors,
							 const Catalog& ceilings, const std::string& where) {
		const std::pair<const Catalog*, assets::WornKind> surfaces[] = {
			{&walls, assets::WornKind::Wall},
			{&floors, assets::WornKind::Floor},
			{&ceilings, assets::WornKind::Ceiling}};
		for (const auto& [cat, k] : surfaces) {
			if (k == *kind) continue;
			for (const CatalogEntry& e : cat->Entries())
				if (e.Get("texture", e.id) == set) {
					refuse(k, loc::Format("newasset.who.type", e.id, where));
					return true;
				}
		}
		return false;
	};
	// The editor IMPORT that brought the set in, which baked it as its type's
	// kind (RecordImport's `surface`, keyed by the resolution-tagged name).
	const auto importedAs = [&](const Catalog& imports, const std::string& where) {
		const CatalogEntry* e = imports.Find(set + "_2k");
		const std::optional<assets::WornKind> k =
			e ? assets::ParseWornKind(e->Get("surface", "")) : std::nullopt;
		if (!k || *k == *kind) return false;
		refuse(*k, loc::Format("newasset.who.import", where));
		return true;
	};
	// This world from memory, then EVERY world on disk (the pool is all of
	// theirs), the template a new world is made from and the style library.
	// Only read: nothing is loaded into the game or stashed.
	const std::string here = m_project.FolderName();
	if (boundAs(m_project.walls, m_project.floors, m_project.ceilings, here) ||
		importedAs(m_project.imports, here))
		return out;
	const auto onDisk = [&](const std::string& catalogDir, const std::string& where) {
		Catalog walls, floors, ceilings, imports;
		walls.Load(catalogDir + "walls.cat");
		floors.Load(catalogDir + "floors.cat");
		ceilings.Load(catalogDir + "ceilings.cat");
		imports.Load(catalogDir + "imports.cat");
		return boundAs(walls, floors, ceilings, where) || importedAs(imports, where);
	};
	const std::string root = paths::Asset("projects");
	for (const std::string& name : Project::List(root))
		if (name != here && onDisk(Project::FolderFor(root, name) + "\\catalog\\", name))
			return out;
	// (The template's folder: Game_NewWorld.cpp's TemplateFolder.)
	if (onDisk(paths::Asset("templates") + "\\default\\catalog\\",
			   loc::Tr("newasset.where.template")) ||
		boundAs(m_library.walls, m_library.floors, m_library.ceilings,
				loc::Tr("newasset.where.library")))
		return out;
	// Nobody paints it as another kind. Meshes that exist are used AS THEY ARE:
	// they are what every type of this kind already draws.
	out.bake = !HasWornMeshes(set);
	return out;
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
	// A surface THEME names one surface type per surface (themes.cat): a
	// delete refuses while a theme still uses the type, and a rename
	// rewrites it. Matched through ThemeMembersOf, the format's one reader.
	if (catalogKey == "walls" || catalogKey == "floors" || catalogKey == "ceilings") {
		const Surface surface = catalogKey == "walls"    ? Surface::Wall
								: catalogKey == "floors" ? Surface::Floor
														 : Surface::Ceiling;
		const char* field = catalogKey == "walls"    ? "wall"
							: catalogKey == "floors" ? "floor"
													 : "ceiling";
		std::vector<std::string> matches;
		for (const CatalogEntry& e : m_project.themes.Entries())
			if (DungeonWorld::ThemeMembersOf(e)[static_cast<size_t>(surface)] == id)
				matches.push_back(e.id);
		hits += static_cast<int>(matches.size());
		if (newId)
			for (const std::string& entryId : matches) {
				CatalogEntry copy = *m_project.themes.Find(entryId);
				copy.Set(field, *newId);
				m_project.themes.Add(std::move(copy)); // add-or-replace by id
			}
	}
	// A STYLE names its room and corridor themes, and a dungeon its default
	// style (tool-refinement Phase 5).
	if (catalogKey == "themes") {
		sweepField(m_project.styles, "room");
		sweepField(m_project.styles, "corridor");
	}
	if (catalogKey == "styles") sweepField(m_project.dungeons, "style");
	// ... and a style's monster list names monsters, among weights that a
	// rename must keep.
	if (catalogKey == "monsters") {
		std::vector<std::string> matches;
		for (const CatalogEntry& e : m_project.styles.Entries()) {
			std::vector<style::Pick> picks = style::ParseMonsters(e.Get("monsters", ""));
			if (style::RenameMonster(picks, id, newId ? *newId : id) > 0) matches.push_back(e.id);
		}
		hits += static_cast<int>(matches.size());
		if (newId)
			for (const std::string& entryId : matches) {
				CatalogEntry copy = *m_project.styles.Find(entryId);
				std::vector<style::Pick> picks = style::ParseMonsters(copy.Get("monsters", ""));
				style::RenameMonster(picks, id, *newId);
				copy.Set("monsters", style::FormatMonsters(picks));
				m_project.styles.Add(std::move(copy));
			}
	}
	// A FLAG is named by an item's hook (`flag = <id>`, or a hand-written
	// `<id>=<value>`) and by a world location's `flag=` (Phase 4). The level
	// records naming one are the level sweep's (DungeonWorld::SweepTypeRefs).
	if (catalogKey == "flags") {
		for (Catalog* c : {&m_project.items, &m_project.weapons, &m_project.armor}) {
			std::vector<std::string> matches;
			for (const CatalogEntry& e : c->Entries()) {
				const std::string f = e.Get("flag", "");
				if (f.substr(0, f.find('=')) == id) matches.push_back(e.id);
			}
			hits += static_cast<int>(matches.size());
			if (newId)
				for (const std::string& entryId : matches) {
					CatalogEntry copy = *c->Find(entryId);
					const std::string f = copy.Get("flag", "");
					const size_t eq = f.find('=');
					copy.Set("flag", *newId + (eq == std::string::npos ? "" : f.substr(eq)));
					c->Add(std::move(copy));
				}
		}
		if (m_worldMap)
			for (const WorldMap::Location& l : m_worldMap->Locations())
				if (const std::string* f = l.Param("flag"); f && *f == id) {
					++hits;
					if (newId)
						for (auto& [k, v] : m_worldMap->MutableLocation(l.id)->params)
							if (k == "flag") v = *newId;
				}
	}
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
	// TERRAIN: the WORLD'S SQUARES are its references (code-review C345). On
	// disk the grid names a kind by its GLYPH, so a rename cannot orphan a cell
	// in the file - but the loaded world holds its own copy of the kinds, which
	// a rename must reach or the brush and the next sync (by id) lose the kind.
	// Counting the squares is what lets a delete be refused while any is painted
	// (DeleteType says so in its own words) and `typerefs` show the use.
	if (catalogKey == "terrain" && m_worldMap) {
		hits += m_worldMap->TerrainCells(id);
		if (newId) m_worldMap->RenameTerrain(id, *newId);
	}
	return hits;
}

std::vector<std::string> Game::SavesReferencingType(const std::string& id) const {
	std::vector<std::string> names;
	// THIS world's saves, named rather than filtered after the fact: the list
	// used to be cut to the LAUNCH world first, so after a switch this found
	// none of the world in hand's (code-review C207).
	for (const SaveSlot& slot : ListSaves(m_project.FolderName())) {
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
	// An entry that only TUNES something the code defines keeps the code's id
	// (code-review C306). A renamed effect was ignored by EffectBook and its
	// class fell back to its defaults - name, icon, plume, stacking - while every
	// `on_hit = burn` still found the class, so nothing said anything was wrong.
	if (Project::IdentityInCode(catalogKey)) {
		problem = loc::Tr("map.type.classbacked");
		return false;
	}
	if (newId.empty() || newId == id) return false;
	if (cat->Contains(newId)) {
		problem = loc::Format("newasset.err.dup", newId);
		return false;
	}
	// ...or in a RELATED catalog (code-review C302): a door renamed onto a
	// decoration's id, a weapon onto an item's.
	if (problem = RelatedIdRefusal(catalogKey, newId); !problem.empty()) return false;
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

	// The live level HELD first: its placements written into records, so the
	// sweep retypes them too, and its state stashed, so the respawn below lays
	// it back. A respawn from the records alone dropped every monster and prop
	// the editor had placed, raised the dead, brought back what it had erased
	// and shut every door (code-review C311).
	m_world->HoldActiveState();
	const DungeonWorld::TypeUsage used = m_world->SweepTypeRefs(catalogKey, id, &newId);
	// Live objects still point at kinds cached under the old id (and monsters
	// hold their type by name), so rebuild them from the records we just wrote.
	// A FEATURE's mesh is filed by its type and stamped into the surfaces, so
	// both kinds re-stamp - the surface ones were left out (C305). An open
	// inspector names an object the respawn replaces, so it goes first (C232).
	CloseInspectors();
	m_world->RespawnFromRecords(DungeonWorld::StampedIntoSurfaces(catalogKey));
	m_world->RestoreHeldState();
	// The undo stack holds level snapshots taken BEFORE the rename; restoring
	// one would bring back records naming a type that no longer exists.
	m_world->ClearUndoHistory();
	// A brush armed with it holds it by id (C352), so it follows the rename.
	m_mapEditor.TypeIdChanged(catalogKey, id, &newId);
	log::Info("Renamed type '{}' -> '{}' ({} record(s) in {} level(s))", id, newId,
			  used.count, used.levels.size());
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.type.renamed", id, newId, used.count));
	WarnStaleSaves(id);
	// An item's banks are keyed by its id (code-review C330).
	if (Project::IsItemCatalog(catalogKey)) RefreshItemBanks();
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
	// Refuse, and say why (docs/effects.md). The same holds for every catalog
	// whose ids are the code's: spells, attacks, the balance sheet (C306).
	if (Project::IdentityInCode(catalogKey)) {
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
	// A TERRAIN still painted on the world (code-review C345): the squares
	// would be left naming a glyph no kind has, and the next launch aborts on
	// the first one. Said in its own words before the general count below,
	// which would call the squares "catalog entries".
	if (catalogKey == "terrain" && m_worldMap)
		if (const int cells = m_worldMap->TerrainCells(id); cells > 0) {
			problem = loc::Format("map.type.inuse.world", cells);
			return false;
		}
	// The live level's placements counted too: the sweep reads records, and a
	// monster or prop the editor placed had none, so its type could be deleted
	// from under it and the next savemap wrote a record naming nothing (C311).
	if (catalogKey == "monsters" || catalogKey == "decorations") m_world->SyncActiveRecords();
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
	m_mapEditor.TypeIdChanged(catalogKey, id, nullptr); // a brush armed with it goes too
	if (!m_project.Save()) log::Warn("delete type: failed to save catalogs");
	if (catalogKey == "terrain") {
		// The world drops the kind too (the kinds after it move down an index,
		// which SyncTerrains carries the grid through), and world.map is written:
		// a square painted over since the last save is still this kind in the
		// file, which the catalog just saved could not read. The undo history goes
		// too: a snapshot from before the last square was painted over still
		// paints that square with the kind that is gone, and an undo would
		// bring the abort back.
		SyncWorldTerrains();
		m_world->ClearUndoHistory();
	}
	log::Info("Deleted type '{}' from {}", id, catalogKey);
	if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.type.deleted", id));
	WarnStaleSaves(id); // a save's spawn rows are outside the level sweep
	if (Project::IsItemCatalog(catalogKey)) RefreshItemBanks(); // keyed by id (C330)
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
	for (const SaveSlot& slot : ListSaves(m_project.FolderName())) { // this world's (C207)
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
	// A type edit takes no undo step but can change what the checker reads (a
	// key item's category, a stair's traverse/exit), so it counts as an edit.
	if (m_world) m_world->NoteEdit();
	Catalog* cat = m_project.CatalogForKey(cfg.catalogKey);
	if (!cat) {
		log::Warn("type editor: unknown catalog '{}'", cfg.catalogKey);
		return;
	}
	cat->Add(MergedTypeEntry(cfg)); // add-or-replace by id
	if (!m_project.Save())
		log::Warn("type editor: failed to save project catalogs");
	// The world reads terrain.cat's kinds through its own copy: hand it the
	// saved ones, so a changed glyph reaches the grid's text (and world.map)
	// rather than only the catalog (code-review C345).
	if (cfg.catalogKey == "terrain") SyncWorldTerrains();
}

CatalogEntry Game::MergedTypeEntry(const TypeEditorDialog::Config& cfg) const {
	CatalogEntry entry;
	const Catalog* cat = m_project.CatalogForKey(cfg.catalogKey);
	if (const CatalogEntry* e = cat ? cat->Find(cfg.id) : nullptr) entry = *e;
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
	return entry;
}

// Kicks the async worn-mesh rebake for a surface type's Save: reuses the
// asset-bake subprocess flow, jumping straight to the wornblock step.
// m_restyleBake tells the Update poll to land it through LandRestyleBake (not
// FinishBake), and the Save itself waits in m_restyleCfg until then.
std::string Game::StartRestyleBake(const TypeEditorDialog::Config& cfg,
								   const CatalogEntry& merged) {
	// One baker at a time: m_bakeReq and m_bake are the running bake's, and a
	// second launch would take them over.
	if (m_baking) return loc::Format("map.type.bakebusy", m_bakeReq.name);
	m_bakeReq = {};              // a wornblock-only bake, no CreateRequest data
	m_bakeReq.textureSet = true; // routes StartBakeStep to the wornblock branch
	m_bakeReq.catalogKey = cfg.catalogKey; // picks wall/floor/ceiling in StartBakeStep
	// The set as the Save would leave it: its `texture`, else the type's own id.
	m_bakeReq.name = merged.Get("texture", cfg.id);
	m_bakeStep = 1;              // skip the texture-import step
	m_bakeWear = merged.GetFloat("wear", 1.0f);
	// Unset passes -1: the baker takes the texture SET's own (Assets/WornSets.h),
	// the depth `AssetBaker models` bakes it at, so a save that touched only
	// `texture` or `wear` cannot reshape the set.
	m_bakeRelief = merged.GetFloat("relief", -1.0f);
	if (!StartBakeStep()) {
		m_bakeWear = 1.0f;
		m_bakeRelief = -1.0f;
		return loc::Tr("newasset.err.launch");
	}
	m_baking = true;
	m_restyleBake = true;
	m_restyleCfg = cfg;
	return {};
}

void Game::LandRestyleBake(int exitCode) {
	m_baking = false;
	m_restyleBake = false;
	const TypeEditorDialog::Config cfg = std::exchange(m_restyleCfg, {});
	// The dialog that made the Save, if it is still up on that type (`typeset
	// dialog off` can close it under a bake, and open another).
	const bool dialog = std::exchange(m_restyleFromDialog, false) && m_typeDialog.IsOpen() &&
						m_typeDialog.CatalogKey() == cfg.catalogKey && m_typeDialog.Id() == cfg.id;
	const Catalog* cat = m_project.CatalogForKey(cfg.catalogKey);
	// A failed bake writes NOTHING: the catalog keeps the texture its worn meshes
	// were baked for. So does a type renamed or deleted under the bake (only the
	// console can), which a write would bring back under its old id.
	if (exitCode != 0 || !cat || !cat->Find(cfg.id)) {
		const std::string why = exitCode != 0
									? loc::Format("map.type.bakefailed", cfg.id, exitCode)
									: loc::Format("map.type.bakegone", cfg.id);
		log::Warn("type editor: {} '{}' not saved - {}", cfg.catalogKey, cfg.id, why);
		if (dialog) m_typeDialog.BakeFailed(why); // the form stays, its edits kept
		if (m_world && m_world->onMessage) m_world->onMessage(why);
		return;
	}
	WriteTypeFields(cfg);
	log::Info("type editor: {} '{}' saved - its worn meshes baked", cfg.catalogKey, cfg.id);
	// Swap the new worn geometry in live (if the world that asked for it is
	// still the one loaded).
	if (m_world) m_world->ReloadDungeonBlocks();
	if (dialog) m_typeDialog.Close();
	if (m_world && m_world->onMessage) m_world->onMessage(loc::View("map.wallstyle.applied"));
}

void Game::ApplyMonsterConfig(CatalogEntry& entry, const MonsterConfigDialog::Config& cfg) {
	// The rows as the dialog says they should now read, in the order a NEW row
	// is appended. Written IN PLACE (code-review C323): this used to erase every
	// row it owns and append them all again, which moved skel_warrior's
	// threat_threshold to the bottom of its entry and deleted the comment
	// explaining it on every Save.
	std::vector<std::pair<std::string, std::string>> rows;
	// Behaviour fields (Behavior tab). archetype is always written; the params are
	// written only when they apply / are non-default, to keep the .cat tidy.
	rows.emplace_back("archetype", ai::kArchetypeNames[static_cast<int>(cfg.archetype)]);
	if (cfg.archetype == ai::Archetype::Skirmisher || cfg.archetype == ai::Archetype::Caster)
		rows.emplace_back("keeprange", std::format("{:g}", cfg.keepRange));
	if (cfg.fleeBelow > 0.0f) rows.emplace_back("fleebelow", std::format("{:g}", cfg.fleeBelow));
	if (cfg.archetype == ai::Archetype::Caster && !cfg.spell.empty())
		rows.emplace_back("spell", cfg.spell);
	// Threat multipliers: write only the ones nudged off 1 (keep the .cat tidy).
	auto setThreat = [&](const char* key, float v) {
		if (v != 1.0f) rows.emplace_back(key, std::format("{:g}", v));
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
	rows.emplace_back("states", join(stateTokens));
	for (int i = 0; i < anim::kCreatureStateCount; ++i) {
		if (cfg.clips[i].empty()) continue;
		const auto s = static_cast<anim::CreatureState>(i);
		rows.emplace_back("anim_" + std::string(anim::StateName(s)), join(cfg.clips[i]));
	}

	// A row this dialog owns that it no longer writes goes, its comment with
	// it (serialize::Remove's rule); the rest are set where they stand.
	const auto owned = [](const std::string& key) {
		return key == "states" || key.starts_with("anim_") || key == "archetype" ||
			   key == "keeprange" || key == "fleebelow" || key == "spell" ||
			   key == "threat_scale" || key == "threat_threshold" ||
			   key == "threat_switch" || key == "threat_decay";
	};
	std::erase_if(entry.fields, [&](const serialize::Field& f) {
		return owned(f.key) && std::none_of(rows.begin(), rows.end(), [&](const auto& r) {
				   return r.first == f.key;
			   });
	});
	for (auto& [key, value] : rows) entry.Set(std::move(key), std::move(value));
}

void Game::WriteMonsterAnim(const MonsterConfigDialog::Config& cfg) {
	// Start from the existing entry so every non-animation field (display, model,
	// hp, ...) is preserved; a brand-new type gets a bare entry.
	CatalogEntry entry;
	if (const CatalogEntry* e = m_project.monsters.Find(cfg.type)) entry = *e;
	else entry.id = cfg.type;
	ApplyMonsterConfig(entry, cfg);

	m_project.monsters.Add(std::move(entry)); // add-or-replace by id
	if (!m_project.Save())
		log::Warn("monster config: failed to save project catalogs");
	else if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.cfg.saved", cfg.type));
}

// Runs one queued task per rendered frame (never before the current loading
// screen has been presented once); returns true when the queue is done.

} // namespace dungeon::game
