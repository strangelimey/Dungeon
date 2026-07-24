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
	else if (m_bakeStep == 0)
		cmd = q(baker) + " import " + q(m_bakeReq.sourcePath) + " " + q(assets) + " " +
			  m_bakeReq.name;
	else {
		// Bake worn block meshes for just the new set (its kind = the catalog).
		const std::string kind = m_bakeReq.catalogKey == "floors"    ? "floor"
								 : m_bakeReq.catalogKey == "ceilings" ? "ceiling"
																	  : "wall";
		cmd = q(baker) + " wornblock " + kind + " " + m_bakeReq.name + " " + q(assets);
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
	log::Info("Synced project {} -> source", src.filename().string());
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
	if (Catalog* cat = m_project.CatalogForKey(m_bakeReq.catalogKey)) {
		CatalogEntry e;
		e.id = m_bakeReq.name;
		e.Set("display", m_bakeReq.name);
		if (m_bakeReq.textureSet) {
			e.Set("texture", m_bakeReq.name);
			e.Set("height_scale", std::format("{:.3f}", m_bakeReq.material.heightScale));
		} else {
			e.Set("model", m_bakeReq.name);
			e.Set("texture", m_bakeReq.name);
			e.Set("authored", "1");
			e.Set("solid", "1");
			// The dialog's material sliders, persisted only when the user moved
			// them: metallic/roughness become the draw's factors (with an ORM map
			// the shader scales the map by them), color tints the albedo, and
			// height_scale overrides the bound set's parallax depth. Untouched
			// sliders leave the imported model's own material authoritative.
			const gfx::MaterialParams& m = m_bakeReq.material;
			if (m_bakeReq.metallicSet) e.Set("metallic", std::format("{:.3f}", m.metallic));
			if (m_bakeReq.roughnessSet) e.Set("roughness", std::format("{:.3f}", m.roughness));
			if (m_bakeReq.heightSet)
				e.Set("height_scale", std::format("{:.3f}", m.heightScale));
			if (m_bakeReq.colorSet)
				e.Set("color", std::format("{:.3f},{:.3f},{:.3f}", m.baseColor.x,
										   m.baseColor.y, m.baseColor.z));
		}
		cat->Add(std::move(e));
		m_project.Save();
		log::Info("Created asset '{}' in {}", m_bakeReq.name, m_bakeReq.catalogKey);
	}
	m_assetDialog.SetBusy(false);
	m_assetDialog.Close();
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
