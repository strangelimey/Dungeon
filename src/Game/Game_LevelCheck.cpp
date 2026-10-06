// ============================================================================
// Game/Game_LevelCheck.cpp - `levelcheck`, the check that the pool holds every
// file the project's levels and types will load, and its mutations.
//
// Split out of Game_DevWorld.cpp when the check grew its loader rules and its
// self-checks (code-review C441) and took that file past two thousand lines.
// The rules themselves are not here: which FILE a type or a palette loads is
// AssetUtil's (ModelFileOf / WornBlockFile), the one resolver the loaders in
// DungeonWorld_Load.cpp ask too. This file only walks the project with it -
// and `modelfile`, which says which file one type's loader resolves and whether
// it has opened it.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Game/AssetUtil.h"
#include "Game/DevCommandArgs.h" // Need
#include "Game/Serialize.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

void Game::RegisterLevelCheckCommands() {
	m_console.Register(
		{.name = "levelcheck",
		 .group = CmdGroup::Levels,
		 .params = "\nmutate empty_model|part2_model|glb|gltf|id|trim|worn",
		 .summary = "check level files, every model a type or palette loads, and the pool's "
					"normal maps"},
		[this](const std::vector<std::string>& a) {
			// WHAT THIS GUARDS, and why it is scoped this narrowly: the baked pool
			// (assets/models, assets/textures) is GITIGNORED, so a fresh clone - or
			// a new worktree provisioned from a stale file list - has catalog
			// entries whose assets are absent. A missing TEXTURE renders magenta
			// and is survivable; a missing MODEL is a LoadModelOrDie and takes the
			// process down at level load, possibly on a level nobody has visited
			// in weeks. That asymmetry is why only models are fatal here.
			//
			// A model is checked as the FILE its loader opens, through the one
			// resolver the loaders use (AssetUtil's ModelFileOf / WornBlockFile).
			// It used to compare the `model` field with the installed STEMS, which
			// passed four kinds of entry that abort (code-review C441): a name
			// installed under neither extension, an entry with no `model` (the
			// loader opens the id), a fixture's empty_model / part2_model, and a
			// palette's worn block meshes. A fifth is the same id fallback met
			// through another type: a door's `trim` naming no doors.cat entry. A
			// name installed under the OTHER extension than its loader's own is no
			// fault since C301 - the loader opens whichever is there - and the
			// check, asking the same resolver, passes it.
			//
			// It does NOT re-validate records against the map (bounds, walkability,
			// a button facing a wall) - the loader already does that, and a second
			// copy of those rules here would be the very drift this suite exists
			// to catch.
			//
			// `mutate <case>` checks the CHECK: it plants one of those faults in
			// what this reads - a copy of one entry, or one file it is told is
			// absent - never in the files, and the verdict must come back FAIL
			// naming the planted file (tools\InGameTest.ps1 runs every case). Two
			// cases are CONTROLS instead (kControls): each plants an entry that
			// LOADS - a model named where its loader prefers the other extension
			// (glb: a decoration naming a .glb-only model; gltf: an item naming a
			// .gltf-only one) - and the verdict must come back as the real run's,
			// with the planted file the one the walk RESOLVED (`resolved=`). Batch
			// 13's `glb` case expected a FAIL; C301 made that entry a good one.
			static constexpr std::string_view kMutations[] = {
				"empty_model", "part2_model", "glb", "gltf", "id", "trim", "worn"};
			static constexpr std::string_view kControls[] = {"glb", "gltf"};
			// The project and the world load together (Game::LoadWorld): with none
			// resident there is nothing to check, and a PASS over nothing is not one.
			// The console's gate refuses first on the title screen; this keeps the
			// body honest if levelcheck ever joins the gate's list.
			if (!m_world) {
				m_console.Refuse("levelcheck: no world loaded - nothing to check");
				return;
			}
			std::string mutation;
			if (!a.empty()) {
				if (a[0] != "mutate" || a.size() != 2) {
					m_console.RefuseUsage();
					return;
				}
				if (std::ranges::find(kMutations, a[1]) == std::end(kMutations)) {
					m_console.Refuse(std::format("levelcheck: no mutation '{}'", a[1]));
					return;
				}
				mutation = a[1];
			}

			// A level's three surface palettes, each with the catalog its ids
			// name: the worn walk below and the worn mutation both read them.
			using Palette = std::pair<const std::vector<std::string>*, const Catalog*>;
			const auto palettesOf = [this](const DungeonMap& map) {
				return std::array<Palette, 3>{{
					{&map.WallPalette(), &m_project.walls},
					{&map.FloorPalette(), &m_project.floors},
					{&map.CeilingPalette(), &m_project.ceilings},
				}};
			};

			// The mutation's planted fault. A catalog case swaps one entry for an
			// altered COPY of it; the worn case hides one file from `installed`.
			const CatalogEntry* target = nullptr;
			CatalogEntry mutant;
			std::string planted, hidden;
			if (mutation == "empty_model" || mutation == "part2_model") {
				// A fixture whose bare bracket / second part names a model that is
				// not installed - a field the old check never read.
				for (const CatalogEntry& e : m_project.fixtures.Entries())
					if (!e.Get(mutation).empty()) {
						target = &e;
						break;
					}
				if (target) {
					mutant = *target;
					mutant.Set(mutation, "levelcheck_mutant");
					planted =
						ModelFileOf(ModelFamily::Fixture, &mutant, mutant.id, mutation);
				}
			} else if (mutation == "glb" || mutation == "gltf") {
				// CONTROLS (code-review C301): a type naming a model installed ONLY
				// under the extension its loader does NOT prefer - a decoration
				// (.gltf first) naming a .glb-only model, an item (.glb first) a
				// .gltf-only one. It LOADS, so the check must not count it, and must
				// resolve it to that very file. The planted file is named here from
				// the pool, never by asking the resolver, so a check that falls back
				// to the preferred extension resolves something else and says so.
				const bool glb = mutation == "glb";
				const std::string_view ext = glb ? ".glb" : ".gltf";
				std::string lone;
				for (const AssetInfo& m : InstalledModelInfo())
					if (m.file.ends_with(ext) &&
						!ModelFileInstalled(m.name + (glb ? ".gltf" : ".glb"))) {
						lone = m.name;
						break;
					}
				// The first decoration that is not `multimaterial` (which prefers
				// .glb itself), or the first item.
				const Catalog& host = glb ? m_project.decorations : m_project.items;
				for (const CatalogEntry& e : host.Entries())
					if (!glb || !CatalogBool(&e, "multimaterial", false)) {
						target = &e;
						break;
					}
				if (target && !lone.empty()) {
					mutant = *target;
					mutant.Set("model", lone);
					planted = lone + std::string(ext);
				}
			} else if (mutation == "id") {
				// A decoration that names its model, with the name taken away: its
				// loader opens `<id>.gltf`, an entry the old check skipped outright
				// for having no `model` to compare. One whose id is no installed
				// file, so the fallback has something to miss.
				for (const CatalogEntry& e : m_project.decorations.Entries())
					if (e.Find("model") && !CatalogBool(&e, "multimaterial", false) &&
						!ModelFileInstalled(ModelFileOf(ModelFamily::Prop, nullptr, e.id))) {
						target = &e;
						break;
					}
				if (target) {
					mutant = *target;
					std::erase_if(mutant.fields,
								  [](const serialize::Field& f) { return f.key == "model"; });
					planted = ModelFileOf(ModelFamily::Prop, &mutant, mutant.id);
				}
			} else if (mutation == "trim") {
				// A door type whose `trim` names no doors.cat entry: its loader
				// draws the trim through DecorationKindFor, which opens the name as
				// a file - the id fallback again, reached through another type.
				for (const CatalogEntry& e : m_project.doors.Entries())
					if (!e.Get("trim").empty()) {
						target = &e;
						break;
					}
				if (target && !m_project.doors.Contains("levelcheck_mutant")) {
					mutant = *target;
					mutant.Set("trim", "levelcheck_mutant");
					planted = ModelFileOf(ModelFamily::Prop, nullptr, "levelcheck_mutant");
				}
			} else if (mutation == "worn") {
				// The first set a level paints, at the first tier this session did
				// NOT load, chosen HERE and not by the walk below: a check that
				// skips that tier, or the worn pass, must come back PASS with it
				// planted (the harness then says the check missed it) rather than
				// refuse as though the world had nothing to plant it in.
				std::string tier;
				for (int q = 0; q <= static_cast<int>(Quality::Ultra) && tier.empty(); ++q) {
					const std::string t = GameSettings::MeshSuffixFor(static_cast<Quality>(q));
					if (t != m_settings.MeshSuffix()) tier = t;
				}
				for (const std::string& stem : m_project.levels) {
					if (tier.empty()) break;
					const DungeonMap* map = m_world->LevelMapAsItIs(stem);
					if (!map) continue;
					for (const auto& [palette, catalog] : palettesOf(*map))
						if (!palette->empty()) {
							const std::string& id = palette->front();
							planted = WornBlockFile(SurfaceSetOf(catalog->Find(id), id), tier);
							break;
						}
					if (!planted.empty()) break;
				}
				hidden = planted;
			}
			// A catalog case plants a file that is NOT installed; the worn case
			// hides one that IS, and a control names one that is. Either way round,
			// a planted file not in the state the case needs would test nothing.
			const bool hides = mutation == "worn";
			const bool control = std::ranges::find(kControls, mutation) != std::end(kControls);
			const bool wantInstalled = hides || control;
			if (!mutation.empty() &&
				(planted.empty() || ModelFileInstalled(planted) != wantInstalled)) {
				if (planted.empty())
					m_console.Refuse(std::format(
						"levelcheck mutate {}: nothing in this world to plant it in", mutation));
				else
					m_console.Refuse(std::format(
						"levelcheck mutate {}: {} is {} installed, so planting it tests nothing",
						mutation, planted, wantInstalled ? "not" : "already"));
				return;
			}
			const auto installed = [&hidden](const std::string& file) {
				return file != hidden && ModelFileInstalled(file);
			};

			int types = 0, missingModels = 0, missingWorn = 0, noRule = 0, missingFiles = 0;
			const auto missingModel = [&](const std::string& file, std::string_view field,
										  const std::string& id) {
				++missingModels;
				m_console.Print(
					std::format("  MISSING MODEL '{}' ({}) of type '{}'", file, field, id));
				log::Warn("levelcheck: missing model '{}' ({}) of type '{}'", file, field, id);
			};

			// THE TYPES, each by its loader's rules. Which catalog a loader reads is
			// AssetUtil's table (ModelCatalogs): DecorationKindFor is handed the
			// decorations, doors, stairs and buttons catalogs, and the rest are one
			// loader each - the same table the editor's create and Save checks ask.
			std::vector<std::pair<const Catalog*, ModelFamily>> families;
			for (const ModelCatalog& c : ModelCatalogs())
				if (const Catalog* cat = m_project.CatalogForKey(std::string(c.key)))
					families.emplace_back(cat, c.family);
			// The file the walk resolved the mutant's model to (a control's verdict).
			std::string resolved;
			for (const Catalog* cat : m_project.AllCatalogs()) {
				const auto family = std::ranges::find_if(
					families, [cat](const auto& f) { return f.first == cat; });
				for (const CatalogEntry& real : cat->Entries()) {
					++types;
					const CatalogEntry& e = &real == target ? mutant : real;
					if (family == families.end()) {
						// A model field where no loader reads one: either a loader
						// this table lacks, or a field that does nothing. Either way
						// it cannot be checked, so it is not passed. (The fixture's
						// are every field any loader reads a model from.)
						for (std::string_view field : ModelFields(ModelFamily::Fixture))
							if (e.Find(field)) {
								++noRule;
								m_console.Print(std::format(
									"  NO LOADER RULE for '{}' of type '{}'", field, e.id));
								log::Warn("levelcheck: no loader rule for '{}' of type '{}'",
										  field, e.id);
							}
						continue;
					}
					for (std::string_view field : ModelFields(family->second)) {
						const std::string file = ModelFileOf(family->second, &e, e.id, field);
						if (&real == target && field == "model") resolved = file;
						if (!file.empty() && !installed(file)) missingModel(file, field, e.id);
					}
					// A model reached through ANOTHER type's id. A door's `trim` is
					// drawn by DecorationKindFor(trim, doors), which opens the name as
					// a file when no doors.cat entry has it; one that IS an entry is
					// checked above as that entry. (Its `frame`, `opener` and `mount`
					// are looked up only when the catalog Contains them, so they never
					// fall back to a file; the frame's code default, door_frame, opens
					// a TRACKED model when the catalog lacks it.)
					if (cat == &m_project.doors)
						if (const std::string trim = e.Get("trim");
							!trim.empty() && !cat->Contains(trim))
							if (const std::string file =
									ModelFileOf(ModelFamily::Prop, nullptr, trim);
								!installed(file))
								missingModel(file, "trim", e.id);
				}
			}

			// THE WORN BLOCKS: every surface set any level paints, at EVERY mesh
			// tier a quality setting can pick. A quality swap loads the new tier's
			// blocks through LoadModelOrDie, so a tier this session never asks for
			// is as fatal, only later.
			std::vector<std::string> tiers;
			for (int q = 0; q <= static_cast<int>(Quality::Ultra); ++q) {
				const std::string tier =
					GameSettings::MeshSuffixFor(static_cast<Quality>(q));
				if (std::ranges::find(tiers, tier) == tiers.end()) tiers.push_back(tier);
			}
			std::vector<std::string> checkedWorn;
			for (const std::string& stem : m_project.levels) {
				// Each level as it IS (live, its stash, else its files read-only).
				// One with a file missing is skipped here and reported below.
				const DungeonMap* map = m_world->LevelMapAsItIs(stem);
				if (!map) continue;
				for (const auto& [palette, catalog] : palettesOf(*map))
					for (const std::string& id : *palette) {
						const std::string set = SurfaceSetOf(catalog->Find(id), id);
						for (const std::string& tier : tiers) {
							const std::string file = WornBlockFile(set, tier);
							if (std::ranges::find(checkedWorn, file) != checkedWorn.end())
								continue;
							checkedWorn.push_back(file);
							if (installed(file)) continue;
							++missingWorn;
							m_console.Print(std::format(
								"  MISSING WORN MESH '{}' of surface type '{}' on level {}", file,
								id, stem));
							log::Warn("levelcheck: missing worn mesh '{}' of surface type '{}' "
									  "on level {}",
									  file, id, stem);
						}
					}
			}

			for (const std::string& stem : m_project.levels) {
				for (const std::string& path :
					 {m_project.LevelMapPath(stem), m_project.LevelEntPath(stem)}) {
					std::error_code ec;
					if (std::filesystem::exists(path, ec)) continue;
					++missingFiles;
					m_console.Print(std::format("  MISSING LEVEL FILE {}", path));
					log::Warn("levelcheck: missing level file {}", path);
				}
			}

			// THE POOL'S NORMAL MAPS (code-review C471). An albedo with no `_n`
			// beside it at its resolution loads FLAT - no relief, no parallax, one
			// warning at load (LoadNormalMapFile) - which is survivable, so it is
			// counted and named rather than failed: the same asymmetry as above.
			// The whole pool, not only the sets a catalog names, because a
			// partly provisioned set is exactly the gap this command exists for,
			// whichever set it hits.
			const std::vector<std::string> flat = TextureStemsMissingNormals();
			for (const std::string& stem : flat) {
				m_console.Print(std::format("  NO NORMAL MAP {} - it loads flat", stem));
				log::Warn("levelcheck: no normal map for {} (it loads flat)", stem);
			}

			const bool ok = missingModels == 0 && missingWorn == 0 && noRule == 0 &&
							missingFiles == 0;
			std::string verdict = std::format(
				"levelcheck RESULT={} levels={} types={} missing_models={} missing_worn={} "
				"no_rule={} missing_files={} installed_models={} worn_files={} "
				"missing_normals={}",
				ok ? "PASS" : "FAIL", m_project.levels.size(), types, missingModels, missingWorn,
				noRule, missingFiles, InstalledModels().size(), checkedWorn.size(), flat.size());
			if (!mutation.empty())
				verdict += std::format(" mutate={} planted={}", mutation, planted);
			if (control) verdict += std::format(" resolved={}", resolved.empty() ? "-" : resolved);
			m_console.Print(verdict);
			log::Info("{}", verdict); // the harness reads this from dungeon.log
		});

	m_console.Register(
		{.name = "modelfile",
		 .group = CmdGroup::Types,
		 .params = "<category> <id>",
		 .summary = "the model file each of a type's fields loads, whether it is installed and "
					"opened, and an item's texture set"},
		[this](const std::vector<std::string>& args) {
			// THE RESOLVER'S ANSWER FOR ONE TYPE (code-review C301): the file each
			// field's loader opens - whichever extension is installed - whether the
			// pool holds it, and whether the world has OPENED it (its model cache:
			// a kind built on it). An item reports how its `texture` dresses its
			// model too: the parts that wear the set, the albedo the last frame's
			// draws HANDED the first of them and the one the details dialog's
			// preview is handed (DungeonWorld::DescribeItemModel, which builds the
			// item's kind).
			if (!devargs::Need(m_console, args, 2)) return;
			if (!m_world) {
				m_console.Refuse("modelfile: no world loaded");
				return;
			}
			const std::optional<ModelFamily> family = ModelFamilyOf(args[0]);
			const Catalog* cat = m_project.CatalogForKey(args[0]);
			if (!family || !cat) {
				m_console.Refuse(std::format("modelfile: '{}' is no catalog whose types load a model",
											 args[0]));
				return;
			}
			const CatalogEntry* e = cat->Find(args[1]);
			if (!e) {
				m_console.Refuse(std::format("modelfile: no {} '{}'", args[0], args[1]));
				return;
			}
			const bool item = *family == ModelFamily::Item;
			// An item's kind is built first, so `loaded` says what its build opened.
			const std::optional<DungeonWorld::ItemModelLook> look =
				item ? m_world->DescribeItemModel(e->id) : std::nullopt;
			for (std::string_view field : ModelFields(*family)) {
				const std::string file = ModelFileOf(*family, e, e->id, field);
				if (file.empty()) continue; // the field loads nothing
				m_console.Print(std::format("modelfile {} '{}' {}: {} installed={} loaded={}",
											args[0], e->id, field, file,
											ModelFileInstalled(file) ? 1 : 0,
											m_world->ModelFileLoaded(file) ? 1 : 0));
			}
			if (look)
				m_console.Print(std::format(
					"modelfile {} '{}' set={} parts={} wears={} drawn={} previewed={}", args[0],
					e->id, look->set.empty() ? "none" : look->set, look->parts, look->wears,
					look->drawn, look->previewed));
		});
}

} // namespace dungeon::game
