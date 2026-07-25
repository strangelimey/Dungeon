// ============================================================================
// Game/Game_Editor.cpp — split out of Game.cpp to keep files small (see Game.h).
// Asset-bake flow + editor level create/rename/sync + catalog writes.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Graphics/DisplayEnum.h"
#include "Graphics/Texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace dungeon::game {
bool Game::StartBakeStep() {
	const std::string baker = paths::ExecutableDir() + "\\AssetBaker.exe";
	const std::string assets = paths::ExecutableDir() + "\\assets";
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
		// Wall-style knobs (default 1/on for an asset-create; set by a restyle).
		if (m_bakeWear != 1.0f)
			cmd += std::format(" --wear {:.3f}", m_bakeWear);
		if (!m_bakeColumns) cmd += " --columns 0";
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

	// The project's catalogs now reference assets that exist only in the
	// exe-side pool (an editor import writes there). They are gitignored either
	// way, but the SOURCE tree is what a build copies from and what a new
	// worktree is provisioned from, so leaving them build-only means the next
	// `rm -rf build` takes them with it. imports.cat says exactly which.
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
			const fs::path from = fs::path(paths::ExecutableDir()) / "assets" / dir;
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

// Mints a fresh level for the editor's [+] toolbar button: writes a minimal
// valid .map/.ent pair next to the project's other levels (all-rock 16x16
// canvas with a 3x3 start room — the palette gate demands all three surface
// records, copied from the ACTIVE level so the new one shares its look), then
// appends the stem to the manifest. Everything downstream (browse, remote
// edits, stair dests, savemap) reads Project::levels or lazy-parses the files,
// so no other state needs touching. Returns the stem, or "" on failure.
std::string Game::CreateNewLevel() {
	// Next free levelN stem (numeric suffixes only; foreign stems just don't
	// bump the counter, and the find() guard keeps the pick collision-free).
	int maxN = 0;
	for (const std::string& s : m_project.levels)
		if (s.starts_with("level"))
			if (int n = std::atoi(s.c_str() + 5); n > maxN) maxN = n;
	const std::string stem = "level" + std::to_string(maxN + 1);
	if (std::find(m_project.levels.begin(), m_project.levels.end(), stem) !=
		m_project.levels.end()) {
		log::Warn("new level: stem {} already exists", stem);
		return {};
	}

	auto join = [](const std::vector<std::string>& ids) {
		std::string out;
		for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
		return out;
	};
	const DungeonMap& live = m_world.Map(); // active level: the palette donor
	std::string map = "; " + stem + " - created in the editor.\n";
	map += "palette wall " + join(live.WallPalette()) + "\n";
	map += "palette floor " + join(live.FloorPalette()) + "\n";
	map += "palette ceiling " + join(live.CeilingPalette()) + "\n\n";
	constexpr int kSize = 16;
	for (int z = 0; z < kSize; ++z) {
		for (int x = 0; x < kSize; ++x) {
			const bool room = x >= 7 && x <= 9 && z >= 7 && z <= 9;
			map += !room ? '#' : (x == 8 && z == 8) ? 'P' : '.';
		}
		map += '\n';
	}
	const std::string ent = "; " + stem + " - dynamic layer (empty).\n";
	if (!assets::WriteBinaryFile(m_project.LevelMapPath(stem), map.data(),
								 map.size()) ||
		!assets::WriteBinaryFile(m_project.LevelEntPath(stem), ent.data(),
								 ent.size())) {
		log::Warn("new level: failed to write {} files", stem);
		return {};
	}
	m_project.levels.push_back(stem);
	m_project.Save();
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.level.created", stem));
	return stem;
}

// The Level dialog's rename: validate against the manifest, let the world
// move files / rekey stashes / repoint stair dests, then commit the manifest
// and keep the map view's browse snapshot truthful. (The dialog adopts the
// new stem itself on true.)
bool Game::RenameLevel(const std::string& oldStem, const std::string& newStem) {
	std::vector<std::string>& levels = m_project.levels;
	const auto it = std::find(levels.begin(), levels.end(), oldStem);
	if (it == levels.end()) return false;
	if (std::find(levels.begin(), levels.end(), newStem) != levels.end()) {
		if (m_world.onMessage)
			m_world.onMessage(loc::Format("map.level.dupname", newStem));
		return false;
	}
	if (!m_world.RenameLevel(oldStem, newStem)) return false;
	*it = newStem;
	m_project.Save();
	m_mapView.OnLevelRenamed(oldStem, newStem);
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.level.renamed", oldStem, newStem));
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
	else if (m_world.onMessage)
		m_world.onMessage(loc::Format("newasset.created", req.name));
}

// Opens the per-type catalog editor for a palette row. The dialog edits a COPY
// of the entry's fields against its category's schema (CatalogSchema), so it
// needs nothing but the entry itself; Monsters additionally get the button
// through to their animation/behaviour dialog, which owns the rows the schema
// leaves out.
void Game::OpenTypeEditor(MapEditor::PaletteCat cat, const std::string& id) {
	const std::string key = MapEditor::CategoryCatalogKey(cat);
	const Catalog* catalog = m_project.CatalogForKey(key);
	const CatalogEntry* entry = catalog ? catalog->Find(id) : nullptr;
	if (!entry) {
		// A level palette can name an id its catalog doesn't define (hand-edited
		// map, or a foreign project) — there is nothing to edit.
		log::Warn("type editor: '{}' is not in {}.cat", id, key);
		if (m_world.onMessage) m_world.onMessage(loc::Format("map.type.unknown", id));
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
	m_typeDialog.Open(std::move(cfg), SchemaFor(key));
}

// The monster type's animation + behaviour dialog (the type editor's extra
// button). It owns the states/anim_*/archetype/threat_* rows and rewrites them
// authoritatively, which is why the schema leaves them out.
void Game::OpenMonsterConfig(const std::string& id) {
	// Guard the force-load: a catalog id whose <model>.gltf is missing would
	// abort in LoadModelOrDie. Warn and skip instead of crashing the editor.
	if (!m_world.MonsterModelAvailable(id)) {
		log::Warn("monster config: '{}' has no loadable model — skipped", id);
		return;
	}
	const CatalogEntry* e = m_project.monsters.Find(id);
	const std::string display = e ? e->Display() : id;
	DungeonWorld::AnimSupport supported;
	DungeonWorld::AnimClips clips;
	m_world.MonsterAnimConfig(id, supported, clips);
	ai::Archetype archetype;
	float keepRange, fleeBelow;
	std::string spell;
	ThreatTuning threat;
	m_world.MonsterBehaviorConfig(id, archetype, keepRange, fleeBelow, spell, threat);
	m_monsterDialog.Open(id, display, supported, clips, archetype, keepRange, fleeBelow,
						 spell, threat, m_world.MonsterClipNames(id), m_world.SpellIds());
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
	return hits;
}

std::vector<std::string> Game::SavesReferencingType(const std::string& id) const {
	std::vector<std::string> names;
	for (const SaveSlot& slot : ListSaves()) {
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
	SweepCatalogRefs(catalogKey, id, &newId);
	if (!m_project.Save()) log::Warn("rename type: failed to save catalogs");

	const DungeonWorld::TypeUsage used = m_world.SweepTypeRefs(catalogKey, id, &newId);
	// Live objects still point at kinds cached under the old id (and monsters
	// hold their type by name), so rebuild them from the records we just wrote.
	m_world.RespawnFromRecords(catalogKey == "wallfeatures");
	// The undo stack holds level snapshots taken BEFORE the rename; restoring
	// one would bring back records naming a type that no longer exists.
	m_world.ClearUndoHistory();
	log::Info("Renamed type '{}' -> '{}' ({} record(s) in {} level(s))", id, newId,
			  used.count, used.levels.size());
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.type.renamed", id, newId, used.count));
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
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.type.stalesaves", id, list));
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
	const DungeonWorld::TypeUsage used = m_world.SweepTypeRefs(catalogKey, id);
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
	if (m_world.onMessage) m_world.onMessage(loc::Format("map.type.deleted", id));
	WarnStaleSaves(id); // a save's spawn rows are outside the level sweep
	return true;
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
						   float wear, bool columns) {
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
	m_bakeColumns = columns;
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
	else if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.cfg.saved", cfg.type));
}

// Runs one queued task per rendered frame (never before the current loading
// screen has been presented once); returns true when the queue is done.

} // namespace dungeon::game
