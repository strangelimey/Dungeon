// ============================================================================
// Game/DungeonWorld_Load.cpp — asset loading + content construction for
// DungeonWorld (declarations in DungeonWorld.h). Split out of DungeonWorld.cpp:
// the staged-load tasks, surface palettes/textures/worn blocks, the monster/
// item/decoration/fixture kind factories, fires, the turbidity map, item
// pickup/drop, and the quality hot-swap.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/Image.h"
#include "Assets/WornPanel.h"
#include "Core/Assert.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonMeshBuilder.h"
#include "Game/Effect/LightEffect.h"
#include "Game/Liquid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <queue>

using namespace DirectX;

namespace dungeon::game {

// A catalog entry's texture set name, defaulting to `fallback` (usually the id)
// when the entry or field is absent. Shared by the monster, decoration, and
// fixture loaders. Their model FILES come from AssetUtil's ModelFileOf - the one
// resolver, which `levelcheck` asks too, so the check cannot pass a type its
// loader would abort on.
static std::string TextureOf(const CatalogEntry* e, const std::string& fallback) {
	return CatalogGet(e, "texture", fallback);
}

// Whether a model ships an animation clip by name — the one membership test the
// catalog populate, the live-apply, and the editor all share.
static bool ModelHasClip(const assets::ModelData& model, const std::string& name) {
	for (const auto& c : model.clips)
		if (c.name == name) return true;
	return false;
}

// Splits a free-form list field (whitespace- and/or comma-separated) into its
// tokens, dropping empties — e.g. the items catalog `command` list "eat, drop".
static std::vector<std::string> SplitTokens(const std::string& s) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ','))
			++i;
		const size_t start = i;
		while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != ',')
			++i;
		if (i > start) out.emplace_back(s.substr(start, i - start));
	}
	return out;
}

// Parses an "x,z" cell token (a .ent override value like leashfrom=5,7). Leaves the
// outputs untouched and returns false on anything malformed.
static bool ParseCell(const std::string& s, int& x, int& z) {
	const size_t comma = s.find(',');
	if (comma == std::string::npos) return false;
	try {
		x = std::stoi(s.substr(0, comma));
		z = std::stoi(s.substr(comma + 1));
	} catch (...) {
		return false;
	}
	return true;
}

// monsters.cat `archetype` token -> the behaviour strategy enum. Unknown tokens
// warn and fall back to brute (the pre-archetype behaviour), so a typo is loud but
// never fatal and an undescribed monster keeps working.
static ai::Archetype ParseArchetype(const std::string& v) {
	if (v == "skirmisher") return ai::Archetype::Skirmisher;
	if (v == "caster") return ai::Archetype::Caster;
	if (v == "swarm") return ai::Archetype::Swarm;
	if (v == "lurker") return ai::Archetype::Lurker;
	if (v == "sentry") return ai::Archetype::Sentry;
	if (v != "brute" && !v.empty())
		log::Warn("monsters.cat: unknown archetype '{}' — using brute", v);
	return ai::Archetype::Brute;
}

// ============================================================================
// Staged loading — one queued task per frame (see LoadQueue).
// ============================================================================

// The three surface texture sets and the height scales their parallax uses —
// the single source of those constants, shared by the staged loader and the
// quality hot-swap (LoadAllSurfaceTextures).
std::array<DungeonWorld::SurfaceDef, 3> DungeonWorld::SurfaceDefs() {
	return {{{m_walls, m_wallSets, m_wallHeights, m_wallFactors},
			 {m_floors, m_floorSets, m_floorHeights, m_floorFactors},
			 {m_ceilings, m_ceilingSets, m_ceilingHeights, m_ceilingFactors}}};
}

void DungeonWorld::ApplySurfaceFactors() {
	for (const SurfaceDef& def : SurfaceDefs())
		def.surface.factors.assign(def.factors.begin(), def.factors.end());
}

// Re-reads the surface catalogs' non-baked material knobs and pushes them at the
// live scene: the parallax depth and the metallic/roughness factors. Nothing is
// reloaded or rebuilt — these are per-draw values, which is exactly why the type
// editor can apply them the moment a surface type is saved (only `texture`,
// `relief` and `wear` change baked geometry and need the wornblock re-bake).
void DungeonWorld::RefreshSurfaceMaterials() {
	ResolveSurfacePalettes();
	for (const SurfaceDef& def : SurfaceDefs())
		def.surface.heightScale.assign(def.heights.begin(), def.heights.end());
	ApplySurfaceFactors();
}

// Resolves each surface palette id through its project catalog into a texture
// set name (DungeonWorld loads <set>_<res> and worn_<set>_<tier>.gltf) and a
// PER-VARIANT parallax height scale. The scale is the type's `height_scale`
// folded with its `wear` (a flat wall type gets 0 parallax so it reads flat,
// matching its flat mesh). An unknown id falls back to the id verbatim as the
// set name, so a hand-edited level still loads something.
void DungeonWorld::ResolveSurfacePalettes() {
	struct Def {
		const std::vector<std::string>& palette;
		const Catalog& catalog;
		std::vector<std::string>& sets;
		std::vector<float>& heights;
		std::vector<SurfaceMaterial>& factors;
		float fallbackHeight;
	};
	const Def defs[] = {
		{m_map.WallPalette(), m_project.walls, m_wallSets, m_wallHeights,
		 m_wallFactors, 0.055f},
		{m_map.FloorPalette(), m_project.floors, m_floorSets, m_floorHeights,
		 m_floorFactors, 0.045f},
		{m_map.CeilingPalette(), m_project.ceilings, m_ceilingSets, m_ceilingHeights,
		 m_ceilingFactors, 0.035f},
	};
	for (const Def& d : defs) {
		d.sets.clear();
		d.heights.clear();
		d.factors.clear();
		for (const std::string& id : d.palette) {
			const CatalogEntry* e = d.catalog.Find(id);
			d.sets.push_back(SurfaceSetOf(e, id));
			const float h = e ? e->GetFloat("height_scale", d.fallbackHeight)
							  : d.fallbackHeight;
			const float wear = e ? std::clamp(e->GetFloat("wear", 1.0f), 0.0f, 1.0f)
								 : 1.0f;
			d.heights.push_back(h * wear);
			// Absent = -1 = the set's ORM map stays authoritative (the prop rule).
			d.factors.push_back({e ? e->GetFloat("metallic", -1.0f) : -1.0f,
								 e ? e->GetFloat("roughness", -1.0f) : -1.0f});
		}
	}
}

void DungeonWorld::AppendLoadTasks(LoadQueue& queue) {
	queue.Add(loc::Tr("load.blocks"), [this] { LoadDungeonBlocks(); }, "blocks");

	// One task per material (the scanned sets dominate the load); the first
	// material of each set resets the surface, exactly as LoadTextureSet does.
	for (const SurfaceDef& def : SurfaceDefs()) {
		Surface& surface = def.surface;
		// A level change re-runs this whole list, and levels usually share a
		// palette: if the surface already holds exactly these sets at this tier,
		// keep them. Only the per-variant parallax depth can differ (two palette
		// ids may name one texture with different height_scale / wear), so that
		// is refreshed; the factors are re-applied by BuildDungeonMeshes anyway.
		if (surface.Holds(def.names, m_settings.TextureSuffix())) {
			surface.heightScale.assign(def.heights.begin(), def.heights.end());
			log::Info("Surface textures kept: {} set(s) at {}", def.names.size(),
					  surface.loadedRes);
			continue;
		}
		for (size_t i = 0; i < def.names.size(); ++i) {
			const std::string& name = def.names[i];
			const float heightScale = def.heights[i]; // per-variant parallax depth
			const bool first = i == 0; // first material resets the set
			std::string spaced = name; // asset id, shown with the '_'s opened up
			std::ranges::replace(spaced, '_', ' ');
			queue.Add(
				loc::Format("load.surface", spaced),
				[this, &surface, name, heightScale, first] {
					if (first) surface.ResetTextures();
					LoadSurfaceMaterial(surface, name, heightScale);
				},
				"surface " + name);
		}
	}

	queue.Add(loc::Tr("load.dungeon"), [this] { BuildDungeonMeshes(); }, "dungeon meshes");
	queue.Add(
		loc::Tr("load.monsters"),
		[this] {
			LoadMonsters();
			ReserveAIPools(); // the map and its monsters are both known now
			LoadItems();
			LoadButtons();
		},
		"monsters + items + buttons");
	queue.Add(
		loc::Tr("load.decorations"),
		[this] {
			LoadDecorations();
			LoadStairs();
			LoadDoors(); // after decorations: shares the prop texture/model caches
		},
		"decorations + stairs + doors");
	queue.Add(
		loc::Tr("load.fires"),
		[this] {
			// Fixture kinds resolve lazily per placed record (FixtureKindFor, the
			// DecorationKind pattern) — BuildFires pulls in whatever the level uses.
			m_particleBatch = std::make_unique<gfx::ParticleBatch>(m_device);
			BuildFires();
			// Their damage side-table, once the kinds are resolved — it asks each
			// kind whether it is breakable at all, so it must run after BuildFires.
			SeedFixtureBreakables();
		},
		"fires");
	queue.Add(loc::Tr("load.dust"), [this] { BuildTurbidityMap(); }, "turbidity");
}

void DungeonWorld::LoadDungeonBlocks() {
	LoadFeatureMeshes();

	// A level change re-runs the load, and levels usually share a palette: the
	// blocks already in memory are the right ones whenever the tier and all three
	// set lists match what they were loaded for.
	BlockSetKey want{m_settings.MeshSuffix(), m_wallSets, m_floorSets, m_ceilingSets};
	if (m_loadedBlocks && *m_loadedBlocks == want) {
		log::Info("Worn blocks kept: {} wall / {} floor / {} ceiling set(s) at {}",
				  m_wallSets.size(), m_floorSets.size(), m_ceilingSets.size(), want.tier);
		return;
	}
	m_loadedBlocks.reset(); // until the loads below have all landed

	// The old dungeon uses the worn, crumbling block set - one mesh per
	// texture variant, displaced at bake time by that texture's height map
	// so geometry relief matches the painted bricks/slabs. The clean
	// *_block.gltf models remain baked for newer areas of the game.
	auto load = [&](std::vector<assets::MeshData>& blocks,
					std::span<const std::string> names) {
		blocks.clear();
		for (const std::string& name : names)
			blocks.push_back(
				LoadModelOrDie(WornBlockFile(name, m_settings.MeshSuffix())).meshes[0]);
	};
	load(m_floorBlocks, m_floorSets);
	load(m_ceilingBlocks, m_ceilingSets);

	// Walls carry more than one mesh per set: the baker emits a panel per PHASE
	// (which slice of a non-square texture the square shows) and per combination
	// of OPEN SIDES (which edges skip the pin because the neighbour matches).
	// Assets/WornPanel.h owns the naming both sides use.
	//
	// THE FILES DECIDE THE PHASE COUNT, not the albedo's aspect. The game could
	// compute an aspect of its own and be a half-pixel off the baker's, which
	// would index a panel that was never written; probing for what is actually
	// on disk cannot disagree with what was actually baked. Only the bare name
	// is required — everything else is optional, and a set that earned none of
	// it (procedural wear, a fractional aspect) degrades to that one panel.
	m_wallBlocks.clear();
	for (const std::string& name : m_wallSets) {
		const std::string tier = m_settings.MeshSuffix();
		const auto panelName = [&](int phase, int open) {
			return WornBlockFile(name, tier, assets::WornPanelSuffix(phase, open));
		};
		WallPanels panels;
		for (int phase = 0; phase < assets::kMaxWornPhases; ++phase) {
			std::array<assets::MeshData, 4> row;
			if (phase == 0) {
				row[0] = LoadModelOrDie(panelName(0, 0)).meshes[0];
			} else {
				// A phase with no fully pinned panel does not exist; stop rather
				// than leave a hole in the middle of the sequence.
				auto base = LoadModelIfPresent(panelName(phase, 0));
				if (!base || base->meshes.empty()) break;
				row[0] = std::move(base->meshes[0]);
			}
			for (int open = 1; open < 4; ++open)
				if (auto model = LoadModelIfPresent(panelName(phase, open));
					model && !model->meshes.empty())
					row[open] = std::move(model->meshes[0]);
			panels.byPhase.push_back(std::move(row));
		}
		m_wallBlocks.push_back(std::move(panels));
	}
	m_loadedBlocks = std::move(want);
}

// The feature meshes. The per-TYPE maps are rebuilt from the catalogs on every
// call, so a type the editor created or re-pointed since the last load is
// picked up exactly as before; only the FILE reads are cached, since the same
// model file is the same mesh whichever level asks. ReloadDungeonBlocks empties
// the cache when the files themselves may have changed.
void DungeonWorld::LoadFeatureMeshes() {
	const auto mesh = [this](const CatalogEntry& e) -> const assets::MeshData* {
		const std::string file = ModelFileOf(ModelFamily::Feature, &e, e.id);
		auto it = m_featureMeshCache.find(file);
		if (it == m_featureMeshCache.end())
			it = m_featureMeshCache
					 .emplace(file, std::move(LoadModelOrDie(file).meshes[0]))
					 .first;
		return &it->second;
	};

	// Wall-feature niche panels, one per wallfeatures.cat type (its `model`),
	// stamped per niche edge into the wall's variant bucket so they take the wall
	// texture (see DungeonMeshBuilder). A level references types.
	m_nicheMeshes.clear();
	m_boreMeshes.clear();
	for (const CatalogEntry& e : m_project.wallfeatures.Entries())
		// A `bore` feature is a see-through window (its own mesh map); everything
		// else is a niche.
		(e.GetBool("bore", false) ? m_boreMeshes : m_nicheMeshes).emplace(e.id, mesh(e));

	// Surface-feature tiles, one per surfacefeatures.cat type, filed by the
	// type's `surface`. Same shape as the niches above, referenced by a level's
	// `floorfeature` / `ceilingfeature` records. Splitting the map here is what
	// lets each resolver answer only for its own side.
	m_floorFeatureMeshes.clear();
	m_ceilingFeatureMeshes.clear();
	for (const CatalogEntry& e : m_project.surfacefeatures.Entries())
		(CatalogGet(&e, "surface", "floor") == "ceiling" ? m_ceilingFeatureMeshes
														 : m_floorFeatureMeshes)
			.emplace(e.id, mesh(e));
}

const assets::MeshData* DungeonWorld::BoreMeshFor(const std::string& type) const {
	const auto it = m_boreMeshes.find(type);
	return it != m_boreMeshes.end() ? it->second : nullptr;
}

const assets::MeshData* DungeonWorld::NicheMeshFor(const std::string& type) const {
	const auto it = m_nicheMeshes.find(type);
	return it != m_nicheMeshes.end() ? it->second : nullptr;
}

const assets::MeshData* DungeonWorld::FloorFeatureMeshFor(const std::string& type) const {
	const auto it = m_floorFeatureMeshes.find(type);
	return it != m_floorFeatureMeshes.end() ? it->second : nullptr;
}

const assets::MeshData* DungeonWorld::CeilingFeatureMeshFor(const std::string& type) const {
	const auto it = m_ceilingFeatureMeshes.find(type);
	return it != m_ceilingFeatureMeshes.end() ? it->second : nullptr;
}

bool DungeonWorld::FeatureIsCeiling(const std::string& type) const {
	return CatalogGet(m_project.surfacefeatures.Find(type), "surface", "floor") ==
		   "ceiling";
}

// Loads a PBR set (albedo sRGB + normal/height + ORM) by base name at the
// current quality tier. Every set is fetched content (tools/FetchTextures.ps1
// installs what the catalogs name; FetchModels.ps1 the bought models' sets),
// and 2k is the resolution every shipped set has - the prop sets come at 2k
// alone - so a set missing at the tier drops to its 2k. `required`
// (surfaces) never returns without an albedo: absent even at 2k (never
// fetched), it is the magenta checker placeholder and a warning
// (LoadTextureFile).
// Otherwise (props) a missing set returns maps with a null albedo and the
// caller keeps its flat color. The single source of the res->2k fallback,
// shared by surfaces and props.
DungeonWorld::PbrMaps DungeonWorld::LoadPbrSet(const std::string& name, bool required) {
	const char* res = m_settings.TextureSuffix();
	std::string stem = paths::Asset(std::format("textures\\{}_{}", name, res));
	PbrMaps maps;
	maps.albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
	if (!maps.albedo) {
		stem = paths::Asset(std::format("textures\\{}_2k", name));
		if (required) {
			log::Warn("{} not found at {} - falling back to 2k", name, res);
			// The placeholder checker, not an abort, if 2k is missing too.
			maps.albedo = LoadTextureFile(m_device, stem, /*srgb*/ true);
		} else {
			maps.albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
			if (!maps.albedo) {
				log::Warn("texture set '{}' not found - using flat material", name);
				return maps; // null albedo: caller falls back to a flat material
			}
		}
	}
	// Linear. A set with no `_n` draws FLAT and says so once (code-review C471):
	// it used to get the magenta checker AS its normal map, which lit the surface
	// as a checkerboard of normals tilted half away from the light.
	maps.normal = LoadNormalMapFile(m_device, stem, name, &maps.flatNormal);
	// ORM (occlusion/roughness/metallic) — present once the set is re-imported;
	// null until then (the renderer falls back to a neutral default).
	maps.mr = TryLoadTextureFile(m_device, stem + "_mr");
	return maps;
}

// Loads one material's PBR set and appends it to the surface's variant arrays
// (albedo/normal/mr + the variant's parallax depth).
void DungeonWorld::LoadSurfaceMaterial(Surface& surface, const std::string& name,
									   float heightScale) {
	PbrMaps maps = LoadPbrSet(name, /*required*/ true);
	// Read the aspect BEFORE the move — a non-square scan (ten of ours are 2:1)
	// has to reach the wall-feature stamp. `required` means the albedo is never
	// null here, but a zero dimension would still fall back to square.
	const gfx::Texture& tex = *maps.albedo;
	surface.uAspect.push_back(tex.Width() > 0 && tex.Height() > 0
								  ? static_cast<float>(tex.Width()) /
										static_cast<float>(tex.Height())
								  : 1.0f);
	surface.albedo.push_back(std::move(maps.albedo));
	surface.normal.push_back(std::move(maps.normal));
	surface.mr.push_back(std::move(maps.mr));
	surface.heightScale.push_back(heightScale);
	surface.loadedSets.push_back(name); // what the arrays hold (Surface::Holds)
	surface.loadedRes = m_settings.TextureSuffix();
}

void DungeonWorld::LoadTextureSet(const SurfaceDef& def) {
	def.surface.ResetTextures(); // hot-swap reuses the same Surface
	for (size_t i = 0; i < def.names.size(); ++i)
		LoadSurfaceMaterial(def.surface, def.names[i], def.heights[i]);
}

void DungeonWorld::LoadAllSurfaceTextures() {
	for (const SurfaceDef& def : SurfaceDefs()) LoadTextureSet(def);
}

void DungeonWorld::AppendSurfaceChunks(DungeonGeometry& geo) {
	// One batch for all three surfaces: a level is hundreds of chunks, and one
	// upload each cost ~0.7 ms of resource creation, submission and a GPU wait
	// (see Graphics/Mesh.h). The meshes come back parallel to the input.
	std::vector<const assets::MeshData*> data;
	data.reserve(geo.walls.size() + geo.floors.size() + geo.ceilings.size());
	for (const auto* list : {&geo.walls, &geo.floors, &geo.ceilings})
		for (const GeometryChunk& gc : *list) data.push_back(&gc.mesh);
	std::vector<std::unique_ptr<gfx::Mesh>> meshes = gfx::CreateMeshes(m_device, data);

	size_t next = 0;
	const auto append = [&](Surface& surface, const std::vector<GeometryChunk>& chunks) {
		for (const GeometryChunk& gc : chunks) {
			SurfaceChunk sc;
			sc.variant = gc.variant;
			sc.chunk = gc.chunk;
			sc.boundsMin = gc.boundsMin;
			sc.boundsMax = gc.boundsMax;
			sc.mesh = std::move(meshes[next++]);
			surface.chunks.push_back(std::move(sc));
		}
	};
	append(m_walls, geo.walls);
	append(m_floors, geo.floors);
	append(m_ceilings, geo.ceilings);
}

DungeonWorld::GeometryPrint DungeonWorld::GeometryFingerprint() const {
	const DungeonGeometry geo = BuildDungeonGeometry(
		m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks, m_walls.uAspect,
		m_floors.uAspect, m_ceilings.uAspect,
		[this](int x, int z) {
			return CellHoles{FloorHoleAt(x, z), CeilingHoleAt(x, z)};
		},
		[this](const std::string& type) { return NicheMeshFor(type); },
		[this](const std::string& type) { return BoreMeshFor(type); },
		[this](const std::string& type) { return FloorFeatureMeshFor(type); },
		[this](const std::string& type) { return CeilingFeatureMeshFor(type); });
	GeometryPrint out;
	const auto fnv = [](u64 h, const void* data, size_t bytes) {
		const auto* p = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
		return h;
	};
	const auto hashSurface = [&](const std::vector<GeometryChunk>& chunks) {
		u64 h = 14695981039346656037ull;
		for (const GeometryChunk& c : chunks) {
			h = fnv(h, &c.variant, sizeof c.variant);
			h = fnv(h, &c.chunk, sizeof c.chunk);
			h = fnv(h, c.mesh.vertices.data(), c.mesh.vertices.size() * sizeof(assets::Vertex));
			h = fnv(h, c.mesh.indices.data(), c.mesh.indices.size() * sizeof(u32));
			out.vertices += c.mesh.vertices.size();
		}
		return h;
	};
	out.walls = hashSurface(geo.walls);
	out.floors = hashSurface(geo.floors);
	out.ceilings = hashSurface(geo.ceilings);

	// The layout pair: (surface, chunk, variant, index count), sorted so the
	// order partial rebuilds leave the live lists in does not matter.
	using Row = std::array<u32, 4>;
	const auto layoutHash = [&](std::vector<Row> rows) {
		std::sort(rows.begin(), rows.end());
		return fnv(14695981039346656037ull, rows.data(), rows.size() * sizeof(Row));
	};
	std::vector<Row> fresh, live;
	u32 s = 0;
	for (const auto* list : {&geo.walls, &geo.floors, &geo.ceilings}) {
		for (const GeometryChunk& c : *list)
			fresh.push_back({s, static_cast<u32>(c.chunk), static_cast<u32>(c.variant),
							 static_cast<u32>(c.mesh.indices.size())});
		++s;
	}
	s = 0;
	for (const Surface* surface : {&m_walls, &m_floors, &m_ceilings}) {
		for (const SurfaceChunk& c : surface->chunks)
			live.push_back({s, static_cast<u32>(c.chunk), static_cast<u32>(c.variant),
							c.mesh ? c.mesh->IndexCount() : 0u});
		++s;
	}
	out.layout = layoutHash(std::move(fresh));
	out.liveLayout = layoutHash(std::move(live));
	return out;
}

void DungeonWorld::BuildDungeonMeshes() {
	// Every path that (re)builds the surfaces runs through here - the staged
	// load, the quality swap, an undo restore, `arena`, the eval `reset` - so
	// this is the one place the per-variant material factors need refreshing.
	ApplySurfaceFactors();
	DungeonGeometry geo = BuildDungeonGeometry(
		m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks, m_walls.uAspect,
		m_floors.uAspect, m_ceilings.uAspect,
		[this](int x, int z) {
			return CellHoles{FloorHoleAt(x, z), CeilingHoleAt(x, z)};
		},
		[this](const std::string& type) { return NicheMeshFor(type); },
		[this](const std::string& type) { return BoreMeshFor(type); },
		[this](const std::string& type) { return FloorFeatureMeshFor(type); },
		[this](const std::string& type) { return CeilingFeatureMeshFor(type); });

	// THE FUNCTION THAT FREES IS THE ONE THAT DRAINS (code-review C193). The
	// clears below release every chunk mesh, and frames still in flight drew
	// them; `arena` and the eval `reset` call this straight from a console line
	// with nothing drained before it, and only a slow CPU build ahead of the
	// clears ever let the GPU finish first. After the geometry build, so the
	// drain waits on as little as possible; on the staged load and the swap
	// paths, which drained already, it finds nothing to wait for.
	m_device.WaitIdle();
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	AppendSurfaceChunks(geo);
	// And every wall look a lever can swap in, so a press builds nothing
	// (code-review C211). After the drain: the looks it drops may have been on
	// show in a frame still in flight.
	PrebuildNicheLooks();
	m_geometryDirty = false; // any full bake pays the deferred-undo debt
}

// What a source leaves behind on a landed blow. The modern form NAMES its
// effects — `on_hit = burn 3 6 0.5, bleed 2 10` — which is what makes a
// serrated blade or a venomous monster pure content. The older
// one-effect-per-line fields are still read, appended as procs naming the same
// effects, so no catalog has to be rewritten to keep working.
void DungeonWorld::ParseOnHit(const CatalogEntry* def, std::vector<fx::Proc>& out,
							  std::string_view where) {
	if (!def) return;
	fx::ParseProcs(CatalogGet(def, "on_hit", ""), out, where);
	for (const char* id : {"poison", "bleed"}) // deprecated: `<effect> = ...`
		if (const std::string line = CatalogGet(def, id, ""); !line.empty())
			fx::ParseProcs(std::string(id) + " " + line, out, where);
	if (const std::string line = CatalogGet(def, "element_dot", ""); !line.empty())
		fx::ParseProcs("burn " + line, out, where); // deprecated
}

// See the declaration. The same parser the catalog goes through, so a spec the
// console accepts is one a weapons.cat line could carry.
bool DungeonWorld::SetItemOnHit(const std::string& type, std::string_view spec) {
	if (!m_project.HasItem(type)) return false;
	ItemKind& kind = ItemKindFor(type);
	kind.onHit.clear();
	fx::ParseProcs(spec, kind.onHit, "onhit [" + type + "]");
	return true;
}

// See the declaration. Every number here mirrors a line of MonsterAttack /
// MonsterRangedAttack / the monster-bolt impact, and the defaults are
// MonsterKindFor's, so a kind scores as it fights.
threat::Profile DungeonWorld::ThreatProfile(const CatalogEntry& def) const {
	const std::string where = "monsters.cat [" + def.id + "]";
	ResistTable powers{};
	ParseResists(CatalogGet(&def, "powers", ""), powers, where, m_damageTypes);
	DamageType type = m_bashType;
	if (const std::string t = CatalogGet(&def, "dmgtype", ""); !t.empty())
		m_damageTypes.Find(t, type);
	std::vector<fx::Proc> onHit;
	ParseOnHit(&def, onHit, where);
	// What a landed blow leaves behind; Threat.cpp turns each into a rate.
	auto dotsOf = [](std::span<const fx::Proc> procs) {
		std::vector<threat::Dot> out;
		for (const fx::Proc& p : procs) out.push_back({p.magnitude, p.duration, p.chance});
		return out;
	};

	const float damage = def.GetFloat("damage", 4.0f);
	const float accuracy = def.GetFloat("accuracy", 60.0f);
	const float offense = def.GetFloat("offense", 1.0f);
	threat::Profile out;
	// Melee: the stance takes its share of the accuracy (MonsterAttack).
	out.melee.damage = m_balance.Potent(damage, powers, type);
	out.melee.accuracy = accuracy * offense;
	out.melee.dots = dotsOf(onHit);

	const ai::Archetype arch = ParseArchetype(CatalogGet(&def, "archetype", "brute"));
	if (arch != ai::Archetype::Skirmisher && arch != ai::Archetype::Caster) return out;
	// A shot flies at the kind's FULL accuracy - no stance - and the impact
	// applies the shooter's powers to whatever type it arrives as.
	threat::Attack shot;
	shot.accuracy = accuracy;
	const std::string spellId = CatalogGet(&def, "spell", "");
	const Spell* spell = spellId.empty() ? nullptr : m_magic.FindSpell(spellId);
	std::optional<ProjectileSpec> bolt;
	if (spell) bolt = spell->MonsterBolt(Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, accuracy);
	if (bolt && bolt->payload.blast.Any()) {
		// A BURST (Hagalaz) bolt is priced by its BLAST, which is what lands once
		// it reaches a member's lane (ResolveMonsterProjectileHit, code-review C1):
		// the detonation square's damage on EVERY member there, not rolled, each
		// left with the payload's effects. Its strike does not land at all. The
		// open-room figure - a corridor's reflections only add to it. No powers:
		// a blast goes off with none of its shooter's (ApplyBlastHit; code-review
		// C2 carries the shooter into it, and this follows).
		const blast::Rules& r = bolt->payload.blast.rules;
		shot.damage = m_balance.Potent(r.damage, ResistTable{}, bolt->atk.type);
		shot.dots = dotsOf(bolt->payload.Procs());
		shot.rolled = false;
		shot.targets = threat::kRefMembers;
	} else if (bolt) { // a caster's spell: its power, its school's type, its payload
		shot.damage = m_balance.Potent(bolt->atk.damage, powers, bolt->atk.type);
		shot.dots = dotsOf(bolt->payload.Procs());
	} else { // a plain bolt carries the kind's melee numbers and on-hit effects
		shot.damage = out.melee.damage;
		shot.dots = out.melee.dots;
	}
	out.shot = shot;
	return out;
}

// Loads each monster model once (shared per kind) and creates one animator
// per spawn. The shared ModelData must stay alive for the animators' sake —
// it lives in m_monsterKinds for the app's lifetime.
DungeonWorld::MonsterKind& DungeonWorld::MonsterKindFor(const std::string& type) {
	auto it = m_monsterKinds.find(type);
	if (it == m_monsterKinds.end()) {
		// Resolve model + texture set through the monsters catalog; an unlisted
		// type falls back to the old name convention (<type>, as .gltf or .glb).
		const CatalogEntry* def = m_project.monsters.Find(type);
		const std::string file = ModelFileOf(ModelFamily::Monster, def, type);
		const std::string tex = TextureOf(def, type);
		auto assets = std::make_unique<MonsterKind>();
		assets->model = ModelFile(file); // shared by every kind on the file
		assets->name = type; // catalog id — drives the monster.<id> loc key
		assets->mesh = ModelMesh(file);
		// The rig's root joint and its rest position: the model is drawn centred
		// on it (MonsterModelWorld) and a burn rides it (BurnOrigin); the
		// Animator keeps its clips from carrying it away (LockRootTravel).
		assets->rigRoot = assets->model->skeleton.RootJoint();
		assets->rigRest = assets->model->skeleton.RootRest();
		if (std::abs(assets->rigRest.x) > 0.05f || std::abs(assets->rigRest.z) > 0.05f)
			log::Info("monster model {}: rig root rests at ({:.3f}, {:.3f}) units off the "
					  "origin - drawn centred on it",
					  file, assets->rigRest.x, assets->rigRest.z);
		// A bound PBR set serves the single-mesh path; an authored
		// multi-material rig carries its textures EMBEDDED and its entry
		// usually names no set — don't warn-hunt one by the id (the skeleton
		// kit's four kinds fired a bogus missing-set warning each) unless the
		// catalog names one explicitly.
		const bool multi = assets->model->meshes.size() > 1;
		if (!multi || (def && def->Find("texture")))
			assets->tex = LoadPropTextures(tex); // <tex>_<res> PBR set, if present
		// Authored multi-material rig (bones/armor/weapons primitives, embedded
		// textures): build the per-material submeshes the draw paths loop.
		if (multi) assets->multi = ModelMulti(file);
		// Map head-shot icon RT; a fresh kind re-arms the one-shot bake pass.
		assets->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
		m_monsterIconsBaked = false;
		// Combat stats (defaults keep an undescribed monster fightable).
		if (def) {
			assets->maxHp = def->GetFloat("hp", 12.0f);
			assets->damage = def->GetFloat("damage", 4.0f);
			assets->accuracy = def->GetFloat("accuracy", 60.0f);
			assets->evasion = def->GetFloat("defense", 10.0f);
			assets->armor = def->GetFloat("armor", 0.0f);
			// The defender side (docs/combat.md part 4): per-type resist
			// cells + what this monster's melee deals AS (default bash).
			ParseResists(CatalogGet(def, "resists", ""), assets->resists,
						 "monsters.cat [" + type + "]", m_damageTypes);
			// Default bash: a monster that names no dmgtype hits like a club.
			assets->damageType = m_bashType;
			if (const std::string t = CatalogGet(def, "dmgtype", "");
				!t.empty() && !m_damageTypes.Find(t, assets->damageType))
				log::Warn("monsters.cat [{}]: unknown dmgtype '{}'", type, t);
			// The ATTACKER half of the type axis — what it is dangerous WITH, the
			// mirror of the `resists` above it.
			ParseResists(CatalogGet(def, "powers", ""), assets->powers,
						 "monsters.cat [" + type + "]", m_damageTypes);
			// What its blows leave behind, named by effect id.
			ParseOnHit(def, assets->onHit, "monsters.cat [" + type + "]");
			assets->shotPayload =
				PackPayload(assets->onHit, "monsters.cat [" + type + "]");
			fx::ParseProcs(CatalogGet(def, "on_crit", ""), assets->onCrit,
						   "monsters.cat [" + type + "]");
			// What the dice's EXTREMES do. The fumble tables stay empty when
			// unauthored rather than being filled with the default here — the
			// default is resolved at the moment of the fumble, so a Balance
			// dialog change to fumble_recover takes effect on the next swing
			// instead of on the next level load.
			assets->critPierce = CatalogGet(def, "crit", "") == "pierce";
			fx::ParseProcs(CatalogGet(def, "on_fumble", ""), assets->onFumble,
						   "monsters.cat [" + type + "]");
			mishap::Parse(CatalogGet(def, "fumble", ""), assets->fumble,
						  "monsters.cat [" + type + "]");
			mishap::Parse(CatalogGet(def, "fumble_severe", ""),
						  assets->fumbleSevere, "monsters.cat [" + type + "]");
			// Melee reach in cells (Phase 7): 2 = a pike melees from its
			// queue post down a clear shared row/column.
			assets->reach = std::max(
				1, static_cast<int>(def->GetFloat("reach", 1.0f) + 0.5f));
			assets->attackInterval = def->GetFloat("attackcd", 1.6f);
			assets->aggroRange = def->GetFloat("aggro", 6.0f);
			assets->moveInterval = def->GetFloat("movecd", 0.6f);
			assets->iq = def->GetFloat("iq", 100.0f);
			assets->archetype = ParseArchetype(CatalogGet(def, "archetype", "brute"));
			// THE STANCE IS PER KIND, not per archetype. An archetype says how
			// a monster MOVES and PERCEIVES — a caster keeps range and throws
			// spells — and says nothing about whether it is reckless doing so.
			// Some casters hurl everything they have; others hang back healing
			// and buffing. That is a personality, it varies WITHIN an
			// archetype, and it belongs in the catalog where it can be seen and
			// edited rather than inferred in C++.
			//
			// All-out is the default because it is the simple behaviour: a
			// monster that has not been told to hedge does not hedge.
			assets->offense = def->GetFloat("offense", 1.0f);
			assets->keepRange = def->GetFloat("keeprange", 4.0f);
			assets->fleeBelow = def->GetFloat("fleebelow", 0.0f);
			assets->spell = CatalogGet(def, "spell", "");
			// Per-type threat multipliers (Balance.h ThreatTuning; 1 = the
			// balance.cat global unchanged).
			assets->threatTuning.scale = def->GetFloat("threat_scale", 1.0f);
			assets->threatTuning.threshold = def->GetFloat("threat_threshold", 1.0f);
			assets->threatTuning.switchMargin = def->GetFloat("threat_switch", 1.0f);
			assets->threatTuning.decay = def->GetFloat("threat_decay", 1.0f);
			if (assets->archetype == ai::Archetype::Caster && assets->spell.empty())
				log::Warn("monsters.cat [{}]: archetype=caster but no spell= set", type);
			assets->facesTarget = def->GetBool("faces", true);
			assets->flammable = def->GetBool("flammable", false);
			assets->fallbackRoughness = def->GetFloat("roughness", 0.9f);
			// Imported-model fixups (degrees in the catalog -> radians here).
			assets->modelYaw = def->GetFloat("modelyaw", 0.0f) * (kPi / 180.0f);
			assets->modelScale = def->GetFloat("modelscale", 1.0f);
			assets->size = ParseSizeClass(CatalogGet(def, "size", "large"));
		}
		// Data-driven animation table (see Animation/CreatureState.h): for each
		// state, an `anim_<state> = clipA clipB ...` row lists the candidate clips
		// (variations); with no row, a state defaults to a clip named after itself
		// when the model ships one. Names are validated against the model so a typo
		// or a not-yet-authored clip is dropped (never a runtime miss), and a state
		// whose name differs from its clip (spawn→rise, taunt→roar) just needs a row.
		for (int i = 0; i < anim::kCreatureStateCount; ++i) {
			const auto st = static_cast<anim::CreatureState>(i);
			const std::string field = "anim_" + std::string(anim::StateName(st));
			const std::string spec = def ? CatalogGet(def, field, "") : std::string();
			std::vector<std::string> clips;
			if (!spec.empty()) {
				for (const std::string& c : SplitTokens(spec))
					if (ModelHasClip(*assets->model,c)) clips.push_back(c);
			} else if (const std::string dflt(anim::StateName(st));
					   ModelHasClip(*assets->model,dflt)) {
				clips.push_back(dflt);
			}
			assets->animClips[i] = std::move(clips);
		}
		// Supported-state set (which CreatureStates this kind can be in). Explicit
		// `states = idle walk attack ...` is the source of truth; with no row, fall
		// back to "supported iff the state has clips" so un-migrated monsters work.
		const std::string statesSpec = def ? CatalogGet(def, "states", "") : std::string();
		if (!statesSpec.empty()) {
			for (const std::string& tok : SplitTokens(statesSpec)) {
				if (const auto s = anim::ParseState(tok))
					assets->stateSupported[static_cast<int>(*s)] = true;
				else
					log::Warn("monsters.cat [{}]: unknown state '{}' in states=", type, tok);
			}
		} else {
			for (int i = 0; i < anim::kCreatureStateCount; ++i)
				assets->stateSupported[i] = !assets->animClips[i].empty();
		}
		assets->stateSupported[static_cast<int>(anim::CreatureState::Idle)] = true; // always rests
		// Authoring aid: a supported state with no clip will animate nothing.
		for (int i = 0; i < anim::kCreatureStateCount; ++i) {
			const auto s = static_cast<anim::CreatureState>(i);
			if (assets->stateSupported[i] && assets->animClips[i].empty()
				&& s != anim::CreatureState::Idle)
				log::Info("monsters.cat [{}]: state '{}' supported but has no clip",
						  type, anim::StateName(s));
		}
		// The map icon's pose, now its idle is known (the bake reads it).
		PoseMonsterIcon(*assets);
		it = m_monsterKinds.emplace(type, std::move(assets)).first;
	}
	return *it->second;
}

// --- editor: monster animation config ---------------------------------------
// These back the right-click config dialog. They force-load the kind (it may not
// be placed in the level) and read/write the same two MonsterKind members the
// catalog populate above fills, so an edit takes effect with no reload.

std::vector<std::string> DungeonWorld::MonsterClipNames(const std::string& type) {
	const MonsterKind& kind = MonsterKindFor(type);
	std::vector<std::string> names;
	names.reserve(kind.model->clips.size());
	for (const auto& c : kind.model->clips) names.push_back(c.name);
	return names;
}

void DungeonWorld::MonsterAnimConfig(const std::string& type, AnimSupport& supported,
									 AnimClips& clips) {
	const MonsterKind& kind = MonsterKindFor(type);
	supported = kind.stateSupported;
	clips = kind.animClips;
}

void DungeonWorld::ApplyMonsterAnimConfig(const std::string& type,
										  const AnimSupport& supported, const AnimClips& clips) {
	MonsterKind& kind = MonsterKindFor(type);
	kind.stateSupported = supported;
	kind.stateSupported[static_cast<int>(anim::CreatureState::Idle)] = true; // rest floor
	for (int i = 0; i < anim::kCreatureStateCount; ++i) {
		std::vector<std::string> filtered;
		for (const std::string& name : clips[i])
			if (ModelHasClip(*kind.model,name)) filtered.push_back(name);
		kind.animClips[i] = std::move(filtered);
	}
	// A new idle is a new map icon: posed on it, and baked again.
	PoseMonsterIcon(kind);
	m_monsterIconsBaked = false;
}

void DungeonWorld::MonsterBehaviorConfig(const std::string& type, ai::Archetype& archetype,
										 float& keepRange, float& fleeBelow, std::string& spell,
										 ThreatTuning& threat) {
	const MonsterKind& kind = MonsterKindFor(type);
	archetype = kind.archetype;
	keepRange = kind.keepRange;
	fleeBelow = kind.fleeBelow;
	spell = kind.spell;
	threat = kind.threatTuning;
}

void DungeonWorld::ApplyMonsterBehavior(const std::string& type, ai::Archetype archetype,
										float keepRange, float fleeBelow,
										const std::string& spell, const ThreatTuning& threat) {
	MonsterKind& kind = MonsterKindFor(type);
	kind.archetype = archetype;
	kind.keepRange = keepRange;
	kind.fleeBelow = fleeBelow;
	kind.spell = spell;
	kind.threatTuning = threat;
}

std::vector<std::string> DungeonWorld::SpellIds() const {
	std::vector<std::string> ids;
	for (const CatalogEntry& e : m_project.spells.Entries()) ids.push_back(e.id);
	return ids;
}

// Whether a monster type's model file exists on disk — the editor guards the
// force-load (right-click → config dialog) with this so a catalog id whose
// model is installed as neither .gltf nor .glb shows a warning instead of
// aborting in LoadModelOrDie.
bool DungeonWorld::MonsterModelAvailable(const std::string& type) const {
	if (m_monsterKinds.contains(type)) return true; // already loaded => present
	return ModelFileInstalled(
		ModelFileOf(ModelFamily::Monster, m_project.monsters.Find(type), type));
}

DungeonWorld::MonsterPreviewData DungeonWorld::MonsterPreviewFor(const std::string& type) {
	const MonsterKind& kind = MonsterKindFor(type);
	MonsterPreviewData d;
	d.mesh = kind.mesh.get();
	d.skeleton = &kind.model->skeleton;
	d.clips = &kind.model->clips;
	// True size (metres), unless that overflows the pane: the preview camera
	// frames ~2.7 m of height and ~2 m of width at the model, which a humanoid
	// fits at true size and a giant spider (two squares of legs) does not. The
	// cap reads the bind-pose bounds of every primitive, horizontal extent over
	// both axes since a yaw fixup may turn either one to face the camera.
	constexpr float kPaneHeightM = 2.2f, kPaneWidthM = 2.0f;
	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const auto& meshData : kind.model->meshes)
		for (const auto& v : meshData.vertices) {
			lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
				  std::min(lo.z, v.position.z)};
			hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
				  std::max(hi.z, v.position.z)};
		}
	d.scale = kUnit * kind.modelScale;
	if (hi.y > lo.y) {
		const float h = hi.y - lo.y;
		const float w = std::max(hi.x - lo.x, hi.z - lo.z);
		d.scale = std::min({d.scale, kPaneHeightM / h, w > 0.0f ? kPaneWidthM / w : d.scale});
	}
	d.modelYaw = kind.modelYaw;
	d.pivot = kind.rigRest;
	const auto& idle = kind.animClips[static_cast<int>(anim::CreatureState::Idle)];
	if (!idle.empty())
		d.idleClip = idle.front();
	else if (!kind.model->clips.empty())
		d.idleClip = kind.model->clips.front().name;
	ApplyPropMaterial(d.material, kind.tex, kind.model->materials[0].baseColorFactor,
					  kind.fallbackRoughness);
	if (kind.multi) { // one drawable per primitive, each with its own material
		for (const MultiMaterialModel::Sub& sub : kind.multi->subs)
			d.subs.push_back({sub.mesh.get(), sub.material});
	} else {
		d.subs.push_back({d.mesh, d.material});
	}
	return d;
}

DungeonWorld::FixturePreviewData DungeonWorld::FixturePreviewOf(const std::string& type) {
	FixtureKind& k = FixtureKindFor(type);
	FixturePreviewData d;
	d.flameHeight = k.flame.height;
	d.flameScale = k.flameless ? 0.0f : k.flame.scale; // empty bowl: no flame
	gfx::MaterialParams mat;
	ApplyPropMaterial(mat, k.tex, k.color, 0.5f);
	if (!mat.albedo) mat.metallic = 1.0f; // flat fallback reads as metal
	if (k.mesh) d.subs.push_back({k.mesh.get(), mat});
	if (k.mesh2) { // the coal bed previews with the bowl
		gfx::MaterialParams coals;
		ApplyPropMaterial(coals, k.tex2, k.color2, 0.9f);
		d.subs.push_back({k.mesh2.get(), coals});
	}
	return d;
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::DecorationPreviewSubs(int index) const {
	std::vector<gfx::PreviewSubmesh> subs;
	if (index < 0 || index >= static_cast<int>(m_decorations.size())) return subs;
	const Decoration& d = m_decorations[static_cast<size_t>(index)];
	if (!d.kind) return subs;
	if (d.kind->multi) { // authored multi-material prop: one sub per glTF material
		for (const auto& s : d.kind->multi->subs) subs.push_back({s.mesh.get(), s.material});
	} else if (d.kind->mesh) {
		gfx::MaterialParams mat;
		mat.doubleSided = !d.kind->authored;
		ApplyPropMaterial(mat, *d.kind, 0.85f);
		mat.alphaCutoff = d.kind->alphaCutoff;
		subs.push_back({d.kind->mesh.get(), mat});
	}
	return subs;
}

namespace {

// A rotation (row-vector, v' = v * M) as its three rows: the images of +X, +Y
// and +Z.
Mat4 Basis(const Vec3& x, const Vec3& y, const Vec3& z) {
	return {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, 0, 0, 0, 1};
}

// How the item details dialog stands an item up before it turns it about the
// vertical (Michael, 2026-09-30: "spin the item along its long axis"). THREE
// POSES, chosen from the item and its box:
//  - a WEAPON stands on its long axis, handle up and point down, so it turns on
//    the line of blade and grip;
//  - a FLAT thing (a rune tablet, the placeholder slab) stands face-on, so the
//    face sweeps past rather than showing only its edge to a level camera;
//  - anything else keeps the pose it was authored in - armor is modelled
//    upright, head hole up, so it turns about the up axis as it stands.
Mat4 PreviewPose(bool weapon, int longAxis, float handleSign, const Vec3& lo,
				 const Vec3& hi) {
	const float s = handleSign;
	if (weapon) {
		// The handle end's direction (along the long axis) goes to +Y.
		switch (longAxis) {
		case 0: return Basis({0, s, 0}, {-s, 0, 0}, {0, 0, 1});  // X -> sY
		case 2: return Basis({1, 0, 0}, {0, 0, -s}, {0, s, 0});  // Z -> sY
		default:
			return s > 0 ? Basis({1, 0, 0}, {0, 1, 0}, {0, 0, 1})    // already up
						 : Basis({1, 0, 0}, {0, -1, 0}, {0, 0, -1}); // turned over
		}
	}
	const float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
	if (ey < 0.35f * std::max(ex, ez)) // lies flat: its face (+Y) turns to the camera
		return Basis({1, 0, 0}, {0, 0, -1}, {0, 1, 0});
	return Basis({1, 0, 0}, {0, 1, 0}, {0, 0, 1});
}

} // namespace

size_t DungeonWorld::FillItemPreview(const ItemKind& kind,
									 std::span<gfx::PreviewSubmesh> out, Vec3& fitMin,
									 Vec3& fitMax, Mat4* pose) const {
	size_t n = 0;
	if (kind.model) { // authored model item (weapon, ...)
		// PartMaterial: a part dressed in the item's set takes its maps as they
		// are now. (The dialog showing these closes on a quality swap - Game::
		// SetQuality - so a filled buffer never outlives the maps it names.)
		for (const auto& s : kind.model->subs)
			if (n < out.size()) out[n++] = {s.mesh.get(), PartMaterial(s)};
		fitMin = kind.model->boundsMin;
		fitMax = kind.model->boundsMax;
	} else if (m_runeMesh && !out.empty()) { // rune / placeholder: the carved tablet
		gfx::MaterialParams mat;
		if (kind.isRune) {
			RuneTabletMaterial(mat, kind); // as its icon shows it: the groove lit
		} else {
			const Vec4 base = m_runeModel.materials.empty()
								  ? Vec4{1, 1, 1, 1}
								  : m_runeModel.materials[0].baseColorFactor;
			ApplyPropMaterial(mat, kind.tex, base, 0.85f);
		}
		out[n++] = {m_runeMesh.get(), mat};
		// AABB of the tablet's vertices, for framing it.
		fitMin = {1e9f, 1e9f, 1e9f};
		fitMax = {-1e9f, -1e9f, -1e9f};
		if (!m_runeModel.meshes.empty())
			for (const auto& v : m_runeModel.meshes[0].vertices) {
				fitMin = {std::min(fitMin.x, v.position.x), std::min(fitMin.y, v.position.y),
						  std::min(fitMin.z, v.position.z)};
				fitMax = {std::max(fitMax.x, v.position.x), std::max(fitMax.y, v.position.y),
						  std::max(fitMax.z, v.position.z)};
			}
	}
	if (pose && n > 0) {
		// Only a real model knows its handle; the tablet placeholder never
		// stands as a weapon (it is flat, and poses as flat).
		const bool weapon = kind.model && kind.category == "weapon";
		*pose = PreviewPose(weapon, kind.model ? kind.model->longAxis : 1,
							kind.model ? kind.model->handleSign : 1.0f, fitMin, fitMax);
	}
	return n;
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::ItemPreviewSubs(int entityId, Vec3& fitMin,
															   Vec3& fitMax) const {
	std::vector<gfx::PreviewSubmesh> subs;
	for (const Item& item : m_items) {
		if (item.id != entityId || !item.kind) continue;
		std::array<gfx::PreviewSubmesh, 16> buf{};
		subs.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(
												  FillItemPreview(*item.kind, buf, fitMin, fitMax)));
		break;
	}
	return subs;
}

size_t DungeonWorld::ItemPreviewForType(const std::string& type,
										std::span<gfx::PreviewSubmesh> out, Vec3& fitMin,
										Vec3& fitMax, Mat4& pose) {
	// Only a type a catalog defines: ItemKindFor would mint a kind for anything.
	if (!m_project.FindItem(type)) return 0;
	return FillItemPreview(ItemKindFor(type), out, fitMin, fitMax, &pose);
}

std::optional<DungeonWorld::ItemModelLook> DungeonWorld::DescribeItemModel(
	const std::string& type) {
	// Only a type a catalog defines: ItemKindFor would mint a kind for anything.
	if (!m_project.FindItem(type)) return std::nullopt;
	const ItemKind& kind = ItemKindFor(type);
	if (!kind.model) return std::nullopt;
	ItemModelLook look;
	look.set = kind.modelSet;
	look.parts = kind.model->subs.size();
	// One word for an albedo a part was HANDED, against its set's as it is now.
	// The handed pointer is only compared - it may name a texture a quality swap
	// has freed - and the size is read off the set's own, live one.
	auto word = [](const gfx::Texture* handed, const PropTextures& set) {
		const gfx::Texture* now = set.albedo.get();
		if (!handed) return std::string("none");
		if (handed != now) return std::string("stale");
		return std::format("{}x{}", now->Width(), now->Height());
	};
	// What FillItemPreview hands the details dialog, part for part.
	std::array<gfx::PreviewSubmesh, 16> preview{};
	Vec3 fitMin, fitMax;
	const size_t previewed = FillItemPreview(kind, preview, fitMin, fitMax);
	for (size_t i = 0; i < kind.model->subs.size(); ++i) {
		const MultiMaterialModel::Sub& sub = kind.model->subs[i];
		if (!sub.set || look.wears++ > 0) continue; // the first dressed part speaks
		// What the last rendered frame's draws handed it (DrawPart's stamp) -
		// never a material worked out here, which would agree with PartMaterial
		// whatever a draw site passed.
		if (m_drawFrame != 0 && sub.drewFrame == m_drawFrame)
			look.drawn = word(sub.drewAlbedo, *sub.set);
		if (i < previewed) look.previewed = word(preview[i].material.albedo, *sub.set);
	}
	return look;
}

bool DungeonWorld::ItemDetailsFor(const std::string& type, ItemDetails& out) {
	if (!m_project.FindItem(type)) return false;
	const ItemKind& k = ItemKindFor(type);
	out = {};
	out.nameKey = k.nameKey;
	out.category = k.category;
	out.damage = k.damage;
	out.speed = k.speed;
	out.skill = k.skill;
	out.polearm = k.polearm;
	out.enchanted = k.enchanted;
	out.element = k.element;
	out.elementBonus = k.elementBonus;
	out.armor = k.armor;
	out.armorClass = k.armorClass;
	out.wear = k.wearSlot;
	for (size_t t = 0; t < m_damageTypes.Count() && t < kMaxDamageTypes; ++t) {
		const float v = k.resists.cells[t];
		if (v == 0.0f) continue;
		out.resists[out.resistCount++] = {
			m_damageTypes.NameKey(DamageType{static_cast<u8>(t)}), v};
	}
	out.nutrition = k.nutrition;
	out.hydration = k.hydration;
	out.restoreHealth = k.restoreHealth;
	out.restoreStamina = k.restoreStamina;
	out.restoreMana = k.restoreMana;
	for (const ItemKind::Cure& cure : k.cures)
		if (out.cureCount < out.cures.size())
			out.cures[out.cureCount++] = {cure.effect, cure.share};
	out.burning = ItemFlameHead(type, out.flameHead);
	out.flameTinted = ItemFlameTint(type, out.flameTint);
	return true;
}

DungeonWorld::Monster DungeonWorld::MakeMonster(MonsterKind& kind, int id, int x,
												int z, Direction facing) {
	Monster monster;
	monster.kind = &kind;
	monster.id = id;
	monster.runtimeId = m_nextMonsterId++; // stable id for async AI plan matching
	// groupId is derived each frame from co-location (ReconcileGroups), not at spawn.
	monster.x = monster.spawnX = x;
	monster.z = monster.spawnZ = z;
	monster.yaw = monster.targetYaw = DirYaw(facing);
	monster.facing = facing;
	monster.hp = kind.maxHp;
	// Take a free slot in the spawn cell so a group placed on one cell fans out
	// (the new monster isn't in m_monsters yet, so self=-1). -1 (full) → slot 0.
	monster.slot = std::max(0, FreeSlotInCell(x, z, kind.size, -1));
	monster.visualPos = SlotCenter(x, z, kind.size, monster.slot);
	// The world moves the monster; its clips only animate it in place
	// (MonsterAnimator holds the root travel to kMonsterRootReach).
	monster.animator = MonsterAnimator(*kind.model);
	// Initial resting pose; DriveMonsterAnim takes over next frame (and plays the
	// spawn clip first if the kind has one, via the default spawnReq).
	const std::string idle = PickClip(kind, anim::CreatureState::Idle);
	monster.animator.Play(idle.empty() ? "idle" : idle);
	// The chase route is REFILLED by ConsumeAIPlans every time this monster's
	// bucket publishes, and a copy into a vector too small to hold it allocates —
	// in the middle of a settled frame, whenever a monster first gets a longer
	// path than it has ever held. On the producer side a pooled plan keeps its
	// path's capacity from tick to tick (AsyncDirector::ComputeBucket) - a
	// worker's grows on the worker's own thread, the inline compute's is sized
	// at level load like this one (ReserveAIPools) - and this is the same
	// promise kept on the consumer side. A BFS route cannot
	// revisit a cell, so the cell count is the true ceiling and not an estimate —
	// a few KB per monster, taken at spawn where allocating costs nothing.
	monster.aiPath.reserve(
		static_cast<size_t>(std::max(0, m_map.Width() * m_map.Height())));
	// Same promise for its plume: the particle buffer at its full ceiling now,
	// so catching fire mid-fight only lights it (Monster::plume).
	monster.plume.Reserve(kPlumeScale);
	// ...and for the effect list that lights it: a monster's first burn lands
	// in the middle of a fight (fx::kMaxEffects).
	fx::ReserveEffects(monster.effects);
	// ...and for the WORLD's formation list, which holds every aware monster:
	// grown here, room for every monster there is plus this one (it is not in
	// m_monsters yet), it never grows when the first of them notices the party
	// mid-fight, or a new peak of them does (code-review C71).
	m_formationScratch.reserve(m_monsters.size() + 1);
	return monster;
}

void DungeonWorld::LoadMonsters() {
	// The cosmetic stream starts over with the level's monsters (C73), here
	// because this is the one place both a level load and a `reset` build them
	// (RespawnFromRecords): each then draws the same idle and rising clips, as
	// `reset` must equal a new game line for line.
	m_cosmeticRng.seed(kCosmeticSeed);
	int phase = 0;
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Monster) continue;
		MonsterKind& kind = MonsterKindFor(spawn.type);
		Monster monster = MakeMonster(kind, spawn.id, spawn.x, spawn.z, spawn.facing);
		// Per-instance AI overrides (.ent key=value). The leash anchor defaults to the
		// spawn cell; leashfrom overrides it. (patrol is parsed in P3b.)
		monster.leashX = monster.spawnX;
		monster.leashZ = monster.spawnZ;
		if (const std::string* v = spawn.Param("asleep")) monster.asleep = (*v != "0");
		if (const std::string* v = spawn.Param("leash"))
			monster.leashRange = std::strtof(v->c_str(), nullptr);
		if (const std::string* v = spawn.Param("leashfrom"))
			ParseCell(*v, monster.leashX, monster.leashZ);
		// Per-instance BEHAVIOUR overrides (else inherit the type default).
		if (const std::string* v = spawn.Param("archetype"))
			monster.archOverride = ParseArchetype(*v);
		if (const std::string* v = spawn.Param("keeprange"))
			monster.keepOverride = std::strtof(v->c_str(), nullptr);
		if (const std::string* v = spawn.Param("fleebelow"))
			monster.fleeOverride = std::strtof(v->c_str(), nullptr);
		if (const std::string* v = spawn.Param("spell")) monster.spellOverride = *v;
		// A monster MADE a caster here with nothing to cast - the instance twin of
		// the kind's own warning (code-review C99: the inspector used to show a
		// spell its Save never wrote).
		if (monster.archOverride == ai::Archetype::Caster &&
			monster.spellOverride.value_or(kind.spell).empty())
			log::Warn("{}.ent: the {} at {},{} has archetype=caster but no spell= set",
					  m_currentLevel, spawn.type, spawn.x, spawn.z);
		// Patrol route: a ;-separated list of x,z waypoints walked when idle (P3b).
		if (const std::string* v = spawn.Param("patrol")) {
			size_t start = 0;
			while (start <= v->size()) {
				const size_t semi = v->find(';', start);
				const std::string cell =
					v->substr(start, semi == std::string::npos ? std::string::npos : semi - start);
				int wx = 0, wz = 0;
				if (ParseCell(cell, wx, wz)) monster.patrol.push_back({wx, wz});
				if (semi == std::string::npos) break;
				start = semi + 1;
			}
		}
		monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
		m_monsters.push_back(std::move(monster));
	}
}

// --- the AI's pools, filled for the level (code-review C62, C66) ------------
// BuildAISnapshot reuses pooled snapshots and walkability grids instead of
// allocating each frame. How many can be in use at once is fixed by the
// threads (kAIPoolDepth), so both pools are filled HERE, each buffer sized to
// this map and these monsters, and never grow in play. They used to grow
// lazily to that high-water mark - which could first be reached minutes in,
// inside a guarded frame, whenever the thread scheduler happened to line the
// workers up - and AllocTest could only catch it by luck.
//
// A buffer still in use from before (the snapshot on show, a worker mid-tick)
// cannot be resized under its reader. One that is SHORT of this level is
// dropped from its pool instead - its reader's shared_ptr keeps it alive until
// the tick ends - and a fresh one takes its place. A dropped snapshot takes its
// grid out of the grid pool with it: that grid is in use too, and once the
// snapshot has left the pool nothing would know it. SHORT means it cannot hold
// what is there NOW (this map, every monster in the list): only a monster
// added later could outgrow one that can, and adding one calls this again
// (AddMonster), which keeps every pooled snapshot able to hold them all. So a
// spawn does not throw away the snapshots in use for want of headroom.
void DungeonWorld::ReserveAIPools() {
	const size_t cells = static_cast<size_t>(m_map.Width()) * m_map.Height();
	const size_t agents = m_monsters.size() + kAIAgentHeadroom;
	const auto size = [&](ai::Snapshot& s) {
		s.blocked.reserve(cells);
		s.occ.reserve(cells);
		s.monsters.reserve(agents);
	};
	const auto fits = [&](const ai::Snapshot& s) {
		return s.blocked.capacity() >= cells && s.occ.capacity() >= cells &&
			   s.monsters.capacity() >= m_monsters.size();
	};

	// 1. Snapshots: the free ones sized, the busy ones that fall short dropped.
	for (size_t i = 0; i < m_snapshotPool.size();) {
		ai::Snapshot& s = *m_snapshotPool[i];
		if (AISnapshotFree(s)) {
			size(s);
		} else if (!fits(s)) {
			const std::vector<uint8_t>* grid = s.walkable.get();
			std::erase_if(m_walkablePool, [grid](const auto& g) { return g.get() == grid; });
			m_snapshotPool.erase(m_snapshotPool.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		++i;
	}
	m_snapshotPool.reserve(kAIPoolDepth * 2); // a growth past the depth (warned) needs no regrow
	while (m_snapshotPool.size() < kAIPoolDepth) {
		auto s = std::make_shared<ai::Snapshot>();
		size(*s);
		m_snapshotPool.push_back(std::move(s));
	}

	// 2. Grids: the free ones sized to this map, the busy ones of another size
	//    dropped (a busy one of this size is simply still in use, and frees itself).
	for (size_t i = 0; i < m_walkablePool.size();) {
		std::vector<uint8_t>& g = *m_walkablePool[i];
		if (AIGridFree(&g)) {
			g.resize(cells);
		} else if (g.size() != cells) {
			m_walkablePool.erase(m_walkablePool.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		++i;
	}
	m_walkablePool.reserve(kAIPoolDepth * 2);
	while (m_walkablePool.size() < kAIPoolDepth)
		m_walkablePool.push_back(std::make_shared<std::vector<uint8_t>>(cells));

	// 3. The inline compute, which runs in guarded frames while resting: its
	//    brain's scratch, and a plan slot - each with a whole map's worth of
	//    path - for every monster of each bucket, plus a little headroom. Its
	//    plans used to start empty, so the first think that found a path grew
	//    one inside the frame (code-review C62).
	std::array<size_t, ai::Scheduler::kBucketCount> slots{};
	for (const Monster& m : m_monsters)
		if (m.kind) ++slots[static_cast<size_t>(ai::Scheduler::BucketForIq(m.kind->iq))];
	for (size_t& s : slots) s += kAIPlanHeadroom;
	m_director.ReserveInline(cells, slots);
}

// items.cat `liquid_color` (r, g, b[, density]) on an item whose model has a
// see-through part: the liquid inside it, generated from that glass
// (Game/Liquid.h) and appended to the kind's own copy of the model as one more
// part, so every place that draws the item - the floor, a niche, in flight, the
// icon, the details dialog - draws its contents with no code of its own.
// `liquid_fill` is how full, 0..1 of the cavity's height.
static constexpr float kFilledGlassDensity = 0.08f;   // clear glass round a liquid
static constexpr float kFilledGlassRoughness = 0.08f;
bool DungeonWorld::BuildLiquid(gfx::GraphicsDevice& device, const assets::ModelData& file,
								const CatalogEntry& def, LiquidPart& out) {
	Vec4 color;
	if (!CatalogColor(&def, "liquid_color", color)) return false;
	// Three numbers = a colour with no density given: a potion is nearly opaque
	// with its colour (CatalogColor would have made it fully opaque).
	const std::string spec = def.Get("liquid_color", "");
	int numbers = 0;
	bool inNumber = false;
	for (const char ch : spec) {
		const bool sep = std::isspace(static_cast<unsigned char>(ch)) || ch == ',';
		if (!sep && !inNumber) ++numbers;
		inNumber = !sep;
	}
	if (numbers < 4) color.w = 0.85f;
	const assets::MeshData* glass = nullptr;
	for (const assets::MeshData& m : file.meshes)
		if (m.material >= 0 && m.material < static_cast<int>(file.materials.size()) &&
			file.materials[static_cast<size_t>(m.material)].blend) {
			glass = &m;
			break;
		}
	if (!glass) return false;
	// In the space a MultiMaterialModel's parts live in - its node baked, as
	// BuildMultiMaterialModel uploads the glass beside it.
	assets::MeshData bakedGlass = *glass;
	assets::BakeNodeTransform(bakedGlass);
	const liquid::Shell shell = liquid::Build(bakedGlass);
	if (shell.mesh.indices.empty()) return false;
	out.mesh = std::make_shared<gfx::Mesh>(device, shell.mesh);
	out.material = {};
	out.material.transparent = true;
	out.material.liquid = true;
	out.material.doubleSided = false;
	out.material.baseColor = color;   // RGB the tint, alpha the density
	out.material.metallic = 0.0f;
	out.material.roughness = 0.15f;   // a wet surface: a tight highlight
	out.material.liquidLevel = liquid::Level(shell, def.GetFloat("liquid_fill", 0.6f));
	out.fromInnerWall = shell.fromInnerWall;
	return true;
}

// A FILLED bottle's glass goes clear. The frosting (tools/BuildPotion.py) is
// there so EMPTY glass reads on a dark floor (Michael's pick); in front of a
// liquid it scatters a white veil over it - under the icon rig's bright lights
// a healing potion came out pink - and the liquid gives the bottle all the
// presence it needs.
void DungeonWorld::ClearGlassForLiquid(gfx::MaterialParams& glass) {
	if (!glass.transparent) return;
	glass.baseColor.w = std::min(glass.baseColor.w, kFilledGlassDensity);
	glass.roughness = std::min(glass.roughness, kFilledGlassRoughness);
}

void DungeonWorld::AddLiquid(ItemKind& kind, const CatalogEntry& def,
							 const std::string& modelFile) {
	if (!kind.model || !def.Find("liquid_color")) return;
	const auto file = ModelFile(modelFile);
	if (!file) return;
	LiquidPart part;
	if (!BuildLiquid(m_device, *file, def, part)) {
		log::Warn("[{}]: liquid_color does not parse, or {} has no see-through part "
				  "to hold it",
				  kind.id, modelFile);
		return;
	}
	for (MultiMaterialModel::Sub& s : kind.model->subs) ClearGlassForLiquid(s.material);
	MultiMaterialModel::Sub sub;
	sub.mesh = std::move(part.mesh);
	sub.material = part.material;
	kind.model->subs.push_back(std::move(sub));
	if (!part.fromInnerWall)
		log::Info("[{}]: {} has no inner wall; its liquid is the glass shrunk", kind.id,
				  modelFile);
}

DungeonWorld::ItemKind& DungeonWorld::ItemKindFor(const std::string& type) {
	auto it = m_itemKinds.find(type);
	if (it == m_itemKinds.end()) {
		auto kind = std::make_unique<ItemKind>();
		kind->id = type;
		const CatalogEntry* def = m_project.FindItem(type);
		// Display name: catalog `name` key, else item.<id> by convention.
		kind->nameKey = CatalogGet(def, "name", std::format("item.{}", type));
		// Shared, data-driven fields: category, carry weight, hand commands.
		kind->category = CatalogGet(def, "category", "misc");
		// Weapon class (docs/skills.md): the skill a swing with this item
		// trains and is scaled by. Absent = `unarmed` (DungeonWorld::WeaponSkill).
		kind->skill = CatalogGet(def, "skill", "");
		// The attack formula's fields (docs/combat.md): weapon damage/speed
		// (absent damage = no weapon, the hand swings bare - HandWeapon; absent
		// speed = the unarmed pace), associated stats (`stats = str,
		// dex`; absent = the unarmed default), and the worn defender side
		// (per-type `resists` cells + a small flat `armor` soak).
		kind->damage = def ? def->GetFloat("damage", 0.0f) : 0.0f;
		kind->speed = def ? def->GetFloat("speed", 0.0f) : 0.0f;
		kind->stats = ParseStatList(CatalogGet(def, "stats", ""),
									"items.cat [" + type + "]");
		// Weapon reach (Phase 7): `reach = polearm` swings from the rear rank.
		kind->polearm = CatalogGet(def, "reach", "melee") == "polearm";
		// What eating or drinking it restores (docs/health-and-healing.md).
		// BOTH, on every item, because most real food is partly one and partly
		// the other — an apple waters a little, a stew does both properly — and
		// splitting them by VERB would have forced bread and a waterskin into
		// different code for the same statement. The verb is flavour; these are
		// the content. Absent = 0 = the item feeds nobody, and a consume of it
		// is refused rather than silently eating a rock.
		kind->nutrition = def ? def->GetFloat("nutrition", 0.0f) : 0.0f;
		kind->hydration = def ? def->GetFloat("hydration", 0.0f) : 0.0f;
		kind->drinkAs = CatalogGet(def, "drink_as", "");
		// A potion (transparency Phase 4): restored at once, and the effects it
		// treats - `cures = poison 0.5, bleed`, each an effects.cat id and the
		// share of its bite taken away (absent = 1, lifted outright).
		kind->restoreHealth = def ? def->GetFloat("restore_health", 0.0f) : 0.0f;
		kind->restoreStamina = def ? def->GetFloat("restore_stamina", 0.0f) : 0.0f;
		kind->restoreMana = def ? def->GetFloat("restore_mana", 0.0f) : 0.0f;
		{
			const std::string spec = CatalogGet(def, "cures", "");
			size_t start = 0;
			while (start < spec.size()) {
				size_t comma = spec.find(',', start);
				if (comma == std::string::npos) comma = spec.size();
				const std::vector<std::string> words =
					SplitTokens(spec.substr(start, comma - start));
				start = comma + 1;
				if (words.empty()) continue;
				ItemKind::Cure cure{words[0], 1.0f};
				if (words.size() > 1) cure.share = std::strtof(words[1].c_str(), nullptr);
				if (!m_effects.Find(cure.effect))
					log::Warn("items.cat [{}]: cures '{}', which is not an effect", type,
							  cure.effect);
				kind->cures.push_back(std::move(cure));
			}
		}
		// Light: a torch, lit or not, and what it becomes.
		kind->burnTime = def ? def->GetFloat("burn_time", 0.0f) : 0.0f;
		// A magical torch lasts (1 + power_level) times its authored burn. Folded
		// in HERE, so the charge, the dimming and the save all just see a longer
		// burn_time.
		kind->powerLevel = def ? std::max(def->GetFloat("power_level", 0.0f), 0.0f) : 0.0f;
		kind->burnTime *= 1.0f + kind->powerLevel;
		if (Vec4 c; CatalogColor(def, "flame_color", c)) {
			kind->flameColor = {c.x, c.y, c.z};
			kind->flameTinted = true;
		}
		kind->litAs = CatalogGet(def, "lit_as", "");
		kind->unlitAs = CatalogGet(def, "unlit_as", "");
		kind->spentAs = CatalogGet(def, "spent_as", "");
		kind->light = CatalogGet(def, "light", kind->Lit() ? "torch" : "");
		kind->trail = CatalogGet(def, "trail", "");
		// What a Splash turns it into: a container one fill level up.
		kind->fillAs = CatalogGet(def, "fill_as", "");
		// What its blows leave behind, named by effect id — the same authored
		// form a monster uses. A plain weapon has none and swings as before.
		ParseOnHit(def, kind->onHit, "weapons.cat [" + type + "]");
		fx::ParseProcs(CatalogGet(def, "on_crit", ""), kind->onCrit,
					   "weapons.cat [" + type + "]");
		// What the dice's EXTREMES do — `crit = pierce` on the way out, and the
		// fumble tables on the way back at the wielder. Left empty when
		// unauthored so the default resolves per swing (see the monster site).
		kind->critPierce = CatalogGet(def, "crit", "") == "pierce";
		fx::ParseProcs(CatalogGet(def, "on_fumble", ""), kind->onFumble,
					   "weapons.cat [" + type + "]");
		mishap::Parse(CatalogGet(def, "fumble", ""), kind->fumble,
					  "weapons.cat [" + type + "]");
		mishap::Parse(CatalogGet(def, "fumble_severe", ""), kind->fumbleSevere,
					  "weapons.cat [" + type + "]");
		// The kind's GLOW starts as its category's tint (a placeholder's look: the
		// tablet, flat-tinted). Set HERE, before the two things that replace it -
		// an enchanted weapon's element just below and a rune's symbol further
		// down - since a default set after them overwrote the element: a
		// flamebrand's floor glow came out steel grey (code-review C190).
		kind->glow = CategoryTint(kind->category);
		// ENCHANTMENT: `element = fire` turns the weapon elemental — every
		// landed blow adds `element_bonus` of its damage as that element, and
		// the element becomes the FLAVOUR its on-hit effects arrive with (so
		// the same `on_hit = burn` is fire on one blade, a freezing burn on
		// another). Absent (or "none") = an ordinary weapon.
		if (const std::string elem = CatalogGet(def, "element", "none");
			!elem.empty() && elem != "none") {
			if (ParseSymbol(elem, kind->element) &&
				kind->element <= SpellSymbol::Water) {
				kind->enchanted = true;
				kind->elementBonus = def->GetFloat("element_bonus", 0.0f);
				// An enchanted weapon lies on the floor lit by its own element
				// (UpdateLights gives it a rune-style glow) — the one visual
				// tell that this blade is not the plain one beside it.
				kind->glow = ElementColor(kind->element);
			} else {
				log::Warn("weapons.cat [{}]: element '{}' is not a school", type, elem);
			}
		}
		ParseResists(CatalogGet(def, "resists", ""), kind->resists,
					 "items.cat [" + type + "]", m_damageTypes);
		if (const std::string cls = CatalogGet(def, "class", ""); !cls.empty())
			if (!ParseArmorClass(cls, kind->armorClass))
				log::Warn("armor.cat [{}]: unknown class '{}' (light/medium/heavy)",
						  type, cls);
		if (const std::string wear = CatalogGet(def, "wear", ""); !wear.empty())
			if (!ParseWearSlot(wear, kind->wearSlot))
				log::Warn("[{}]: unknown wear slot '{}'", type, wear);
		kind->armor = def ? def->GetFloat("armor", 0.0f) : 0.0f;
		// The attacker half: what wielding or wearing this makes you potent WITH.
		ParseResists(CatalogGet(def, "powers", ""), kind->powers,
					 "[" + type + "]", m_damageTypes);
		kind->weight = def ? def->GetFloat("weight", 0.0f) : 0.0f;
		// `command` is a free-form list (whitespace/comma separated) of command ids
		// the hand right-click menu offers; runes implicitly gain "memorize" below.
		for (const std::string& cmd : SplitTokens(CatalogGet(def, "command", "")))
			kind->commands.push_back(cmd);
		kind->drinks = std::find(kind->commands.begin(), kind->commands.end(), "drink") !=
					   kind->commands.end();
		// THROWING (ui-updates Phase 10; DungeonWorld_Throw.cpp). Any item can be
		// thrown. `throw` names the ATTACK it flies as (attacks.cat - its type and
		// numbers): absent = a weapon's first command, else `throw` (bash). What it
		// leaves on what it strikes is its `on_hit`, or `throw_spell`'s whole
		// payload - a fire flask carries firebolt_burst's, blast and all. `throw_breaks`
		// = it shatters where it stops instead of landing.
		kind->throwAttack = CatalogGet(def, "throw", "");
		kind->throwBreaks = CatalogBool(def, "throw_breaks", false);
		kind->upright = CatalogBool(def, "upright", false);
		kind->throwPayload = PackPayload(kind->onHit, "[" + type + "]");
		if (kind->enchanted) kind->throwPayload.flavour = kind->element;
		// An area BLAST of its own, authored as a spell's is (blast_force ...,
		// blast_persist for a gas that fills and keeps biting), typed by
		// `blast_type` (a damagetypes.cat id; poison is earth here).
		if (def) ReadBlastRules(*def, kind->throwPayload.blast);
		kind->throwBlastType =
			m_damageTypes.FindOr(CatalogGet(def, "blast_type", "bash"), m_balance.Neutral().type);
		if (const std::string spellId = CatalogGet(def, "throw_spell", ""); !spellId.empty()) {
			if (const Spell* spell = m_magic.FindSpell(spellId)) {
				kind->throwPayload = spell->MakePayload();
				kind->throwPayload.flavour = spell->School();
				// A borrowed blast burns as the spell's school - fire, not bash.
				kind->throwBlastType = m_damageTypes.ForSchool(spell->School());
			} else {
				log::Warn("[{}]: throw_spell '{}' is not a spell", type, spellId);
			}
		}
		// `throw_scale` (transparency Phase 5): a bomb's SIZE. The fire and poison
		// flasks come in a vial, a small bottle and a flask, and the size scales
		// what the throw leaves - the blast's damage, its reach (blast_force counts
		// SQUARES, so it rounds, and never below one) and how long a gas lingers,
		// and the strength of its on-hit effects. Applied after a borrowed spell
		// payload too, which is the case it exists for: the fire flask's numbers
		// are firebolt_burst's. (ProjectilePayload::Scale, the rule a gust's toll
		// on a shot uses too.)
		if (const float s = def ? def->GetFloat("throw_scale", 1.0f) : 1.0f; s != 1.0f && s > 0.0f)
			kind->throwPayload.Scale(s);
		// A lit torch with a flame colour of its own sets alight in that colour
		// when THROWN too, as it does when swung (FlameTintOf).
		if (const Vec3* tint = FlameTintOf(*kind)) kind->throwPayload.tint = *tint;
		// Authored model (catalog `model`): the item draws as this 3D model on the
		// floor and its baked render becomes the icon/cursor. null = the tablet+tint
		// placeholder. The bought items are embedded-texture multi-material .glb; an
		// editor import is a .gltf, which loads as well (ModelFileOf takes the
		// extension that is installed - code-review C301).
		if (const std::string file = ModelFileOf(ModelFamily::Item, def, type); !file.empty()) {
			// The file's meshes + textures are shared (an enchanted blade and its
			// plain twin, five armours on one model); the materials are this
			// kind's own copy, so its overrides touch nothing else.
			kind->model = ModelMulti(file);
			// THE ENTRY'S OWN SET (`texture`, which an import writes beside the
			// model it made): bound by the rule the asset picker shows it with.
			// It was never read, so an imported weapon - a mesh with no image of
			// its own - drew WHITE. Before the material overrides, which apply on
			// top of it as they do on a prop; before the liquid, which is no part
			// of the file and wears no set.
			kind->modelSet = CatalogGet(def, "texture", "");
			if (!kind->modelSet.empty())
				WearModelSet(*kind->model, LoadPropTextures(kind->modelSet), file);
			BakeCatalogMaterial(*kind->model, def); // dialog material overrides
			if (def) AddLiquid(*kind, *def, file);
		}
		// Every item draws as the shared carved-stone tablet (loaded once) — runes
		// carve their element's set in; other categories ride the flat tint above.
		if (!m_runeMesh) {
			m_runeModel = LoadModelOrDie("rune_tablet.gltf");
			m_runeMesh = std::make_unique<gfx::Mesh>(m_device, m_runeModel.meshes[0]);
			// Cache the tablet AABB so the floor draw can tip it flat + re-ground.
			m_runeBoundsMin = {1e9f, 1e9f, 1e9f};
			m_runeBoundsMax = {-1e9f, -1e9f, -1e9f};
			for (const auto& v : m_runeModel.meshes[0].vertices) {
				m_runeBoundsMin = {std::min(m_runeBoundsMin.x, v.position.x),
								   std::min(m_runeBoundsMin.y, v.position.y),
								   std::min(m_runeBoundsMin.z, v.position.z)};
				m_runeBoundsMax = {std::max(m_runeBoundsMax.x, v.position.x),
								   std::max(m_runeBoundsMax.y, v.position.y),
								   std::max(m_runeBoundsMax.z, v.position.z)};
			}
		}
		// RUNES are the built-out specialization — category=rune, symbol=<sym>.
		if (kind->category == "rune") {
			SpellSymbol sym;
			if (ParseSymbol(CatalogGet(def, "symbol", "fire"), sym)) {
				kind->isRune = true;
				kind->runeSymbol = sym;
				// The whole tablet pulses in its element's accent colour via an
				// additive emissive term (see SubmitSceneGeometry); the shared
				// palette lives in Spells (ElementColor).
				kind->glow = ElementColor(sym);
				kind->tex = LoadPropTextures(std::string(RuneItemId(sym)));
				// A rune is always memorizable, even if the catalog omits `command`.
				if (std::find(kind->commands.begin(), kind->commands.end(),
							  "memorize") == kind->commands.end())
					kind->commands.push_back("memorize");
			} else {
				log::Warn("item {}: unknown rune symbol '{}'", type,
						  CatalogGet(def, "symbol", ""));
			}
		}
		// A model item owns a render-target texture for its baked 3D icon (drawn
		// once by BakeItemIconsIfNeeded; the icon bank points at it). Placeholder
		// items leave iconTarget null and keep their flat category swatch.
		// A RUNE bakes one too, of its carved tablet (BakeRuneIcon; Michael: the
		// pack should show the tablet, the glyph alone is for the hand and spell
		// controls).
		if (kind->model || (kind->isRune && m_runeMesh)) {
			kind->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
			kind->iconAnimated = kind->model && CatalogBool(def, "icon_spin", false);
			m_itemIconsBaked = false; // a freshly added icon needs baking
		}
		// Uniform size trim over the authored unit size, like DecorationKind's.
		kind->modelScale = def ? def->GetFloat("scale", 1.0f) : 1.0f;
		// How it lies on the floor, worked out once now that the model (or the
		// tablet's bounds) and the scale are known (code-review C359).
		LayOnFloor(*kind);
		it = m_itemKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

const gfx::Texture* DungeonWorld::ItemIconFor(const std::string& typeId) {
	return ItemKindFor(typeId).iconTarget.get();
}

void DungeonWorld::LoadItems() {
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Item) continue;
		ItemKind& kind = ItemKindFor(spawn.type);
		// A `niche=<dir>` param puts the item IN that wall niche (it piles at the
		// pocket, ignores the floor quarters). Otherwise it's a floor item — fan
		// multiples in a cell across quarters (target = cell centre, fill order).
		Direction nd;
		if (const std::string* np = spawn.Param("niche"); np && ParseDirection(*np, nd)) {
			m_items.push_back(
				{&kind, spawn.id, spawn.x, spawn.z, false, 0, static_cast<int>(nd)});
			continue;
		}
		// An authored `slot=` (the editor's sub-cell placement) is honoured as
		// written — that is the whole point of persisting it. Anything absent or
		// out of range falls back to the fill-order pick, so hand-written records
		// and everything authored before the field existed load exactly as before.
		int slot = -1;
		if (const std::string* sp = spawn.Param("slot"); sp) {
			const int v = std::atoi(sp->c_str());
			if (v >= 0 && v < 4) slot = v;
		}
		if (slot < 0) {
			const Vec3 c = m_map.CellCenter(spawn.x, spawn.z);
			slot = FreeItemSlotNear(spawn.x, spawn.z, c.x, c.z, -1);
		}
		m_items.push_back({&kind, spawn.id, spawn.x, spawn.z, false, slot});
	}
	ReserveDropRoom();
}

void DungeonWorld::LoadButtons() {
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Button) continue;
		Button b;
		b.id = spawn.id;
		b.x = spawn.x;
		b.z = spawn.z;
		b.facing = spawn.facing;
		if (const std::string* t = spawn.Param("target")) b.target = *t;
		ReadButtonFlags(spawn, b);
		// The lever's two meshes, when the catalog knows the type (a legacy
		// record with an unknown type still works — it just has no 3D presence).
		// The mount is shared by every lever type, resolved by its well-known
		// id the way a door resolves [door_frame].
		if (m_project.buttons.Contains(spawn.type)) {
			b.kind = &DecorationKindFor(spawn.type, m_project.buttons);
			if (m_project.buttons.Contains("lever_plate"))
				b.plate = &DecorationKindFor("lever_plate", m_project.buttons);
		}
		m_buttons.push_back(std::move(b));
	}
}

void DungeonWorld::LoadDoors() {
	for (const Entity& spawn : m_entities.All())
		if (spawn.kind == EntityKind::Door) SpawnDoor(spawn);
	if (!m_doors.empty()) log::Info("Placed {} doors", m_doors.size());
}

// True if (x,z) is the party cell or orthogonally adjacent — arm's reach for
// picking up or dropping a tablet.
static bool InReach(int x, int z, int px, int pz) {
	return std::abs(x - px) + std::abs(z - pz) <= 1;
}

const std::string* DungeonWorld::TryPickItem(float mx, float my, float w, float h,
											 float* charge) {
	const int best = PickItemIndex(mx, my, w, h);
	if (best < 0) return nullptr;
	Item& picked = m_items[static_cast<size_t>(best)];
	picked.collected = true; // off the floor
	NoteItemCaster(picked);  // ...and out of the cubes it cast in (code-review C178)
	if (charge) *charge = picked.charge; // what is left of it comes up too
	++m_harness.tally.lifts;
	m_audio.Play(m_sounds.click, 0.6f); // placeholder pickup cue
	// The LEADER lifts it (Phase 9) - the line names them.
	if (onMessage)
		onMessage(loc::FormatLine("log.take_item", LeaderName(), loc::View(picked.kind->nameKey)));
	return &picked.kind->id;
}

const std::string* DungeonWorld::ItemTypeUnder(float mx, float my, float w, float h) const {
	const int best = PickItemIndex(mx, my, w, h);
	return best < 0 ? nullptr : &m_items[static_cast<size_t>(best)].kind->id;
}

int DungeonWorld::PickItemIndex(float mx, float my, float w, float h) const {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	// Quarter pick: each floor item sits at the centre of one of its cell's four
	// quarters (the Medium 2x2 slot grid). A click counts if the ray, measured at
	// the item's OWN visible height, lands in that item's quarter — no per-mesh hit
	// test. Sampling at the item's height (not the floor plane y=0) is what makes a
	// standing tablet/model clickable: intersecting the floor would land the hit
	// behind the item's base (you look down at an angle), missing the quarter.
	const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
	// Top item = the last one in render order (drawn over the others in a stack).
	int best = -1;
	for (size_t i = 0; i < m_items.size(); ++i) {
		const Item& item = m_items[i];
		if (!InReach(item.x, item.z, px, pz) || !IsSeen(item.x, item.z)) continue;
		// Lifted, or in a shut niche: not there to pick (its pose says so, as it
		// does for the draw and the lights).
		ItemPose pose;
		if (!FloorItemPose(item, pose)) continue;
		if (item.niche >= 0) {
			// Niche item: the pocket's ball (the player looks roughly level at the
			// wall, so no floor-plane test) - the same ball a drop aims at.
			const Vec3 p{pose.spot.x, pose.spot.y + kNichePocketRise * kUnit, pose.spot.z};
			if (ray.HitsSphere(p, kNichePocketRadius * kUnit)) best = static_cast<int>(i);
			continue;
		}
		if (ray.dir.y >= -1e-4f) continue; // floor pick needs a look down at the floor
		// Plane at the item's DRAWN middle - half the height its kind lies to
		// (ItemKind::floorHeight, code-review C359): a torch laid flat is a few
		// centimetres, a standing bottle most of a metre.
		const float t = (pose.midY - ray.origin.y) / ray.dir.y;
		if (t <= 0.0f) continue;
		const float wx = ray.origin.x + ray.dir.x * t;
		const float wz = ray.origin.z + ray.dir.z * t;
		const int hx = static_cast<int>(std::floor(wx / kCellSize));
		const int hz = static_cast<int>(std::floor(wz / kCellSize));
		if (hx != item.x || hz != item.z) continue;
		// Quarter within the cell: col = west(0)/east(1), row = north(0)/south(1),
		// matching SlotCenter's Medium 2x2 layout (slot = row*2 + col).
		const float lx = wx / kCellSize - static_cast<float>(hx);
		const float lz = wz / kCellSize - static_cast<float>(hz);
		const int slot = (lz < 0.5f ? 0 : 2) + (lx < 0.5f ? 0 : 1);
		if (slot == item.slot) best = static_cast<int>(i);
	}
	return best;
}

bool DungeonWorld::DropItemAt(const std::string& typeId, float mx, float my,
							  float w, float h, float charge) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
	// First: does the ray land in an OPEN niche's pocket within reach? Drop into
	// it (a runtime item that piles at the pocket, saved as a `drop` with niche).
	int bestNiche = -1;
	float bestT = 1e9f;
	const std::vector<WallNiche>& niches = m_map.Niches();
	for (size_t i = 0; i < niches.size(); ++i) {
		const WallNiche& n = niches[i];
		if (!n.open || !InReach(n.x, n.z, px, pz) || !IsSeen(n.x, n.z)) continue;
		// The pocket's ball, the one PickItemIndex lifts from.
		const Vec3 p = NicheItemPos(n.x, n.z, n.wall);
		float t = 0.0f;
		if (!ray.HitsSphere({p.x, p.y + kNichePocketRise * kUnit, p.z}, kNichePocketRadius * kUnit, &t))
			continue;
		if (t < bestT) { bestT = t; bestNiche = static_cast<int>(i); }
	}
	if (bestNiche >= 0) {
		const WallNiche& n = niches[static_cast<size_t>(bestNiche)];
		ItemKind& kind = ItemKindFor(typeId);
		PlaceDrop({&kind, m_nextDropId--, n.x, n.z, false, 0, static_cast<int>(n.wall), charge});
		++m_harness.tally.drops;
		m_audio.Play(m_sounds.click, 0.5f);
		if (onMessage) onMessage(loc::FormatLine("log.drop_rune", loc::View(kind.nameKey)));
		return true;
	}
	// Then the FLOOR: where the ray meets the floor plane, when a thing can rest
	// on that square (ItemCanRest - not a pit or a stairwell, where it would hang
	// over the shaft, and not a shut door's square) and it is seen and in reach.
	// Anything else - looking above the floor's horizon, at a wall (the plane
	// meets the floor behind it), past reach, over a hole - is not a drop: the
	// caller throws (Phase 10; it used to fall at the feet).
	if (ray.dir.y >= -1e-3f) return false;
	const float t = -ray.origin.y / ray.dir.y;
	const float wx = ray.origin.x + ray.dir.x * t;
	const float wz = ray.origin.z + ray.dir.z * t;
	const int cx = static_cast<int>(std::floor(wx / kCellSize));
	const int cz = static_cast<int>(std::floor(wz / kCellSize));
	if (!ItemCanRest(cx, cz) || !IsSeen(cx, cz) || !InReach(cx, cz, px, pz)) return false;
	ItemKind& kind = ItemKindFor(typeId);
	const int slot = FreeItemSlotNear(cx, cz, wx, wz, -1); // the quarter under the cursor
	PlaceDrop({&kind, m_nextDropId--, cx, cz, false, slot, -1, charge});
	++m_harness.tally.drops;
	m_audio.Play(m_sounds.click, 0.5f);
	if (onMessage) onMessage(loc::FormatLine("log.drop_rune", loc::View(kind.nameKey)));
	return true;
}

bool DungeonWorld::DropItemOnSquare(const std::string& typeId, int x, int z) {
	// A `tp` or `face` earlier this frame has not reached the camera yet.
	UpdateCamera();
	// The square's centre on the floor, through the camera to a pixel. Any
	// viewport size will do: ScreenRay inverts the very projection this applies,
	// so the click lands on the same floor point whatever the window is.
	constexpr float kW = 1600.0f, kH = 900.0f;
	const Vec3 c = m_map.CellCenter(x, z);
	const Mat4 vp = m_camera.ViewProj();
	const XMVECTOR clip =
		XMVector4Transform(XMVectorSet(c.x, 0.0f, c.z, 1.0f), XMLoadFloat4x4(&vp));
	const float w = XMVectorGetW(clip);
	if (w <= 1e-4f) return false; // behind the eye: no pointer lands there
	const float nx = XMVectorGetX(clip) / w, ny = XMVectorGetY(clip) / w;
	return DropItemAt(typeId, (nx + 1.0f) * 0.5f * kW, (1.0f - ny) * 0.5f * kH, kW, kH);
}

void DungeonWorld::PlaceDrop(const Item& placed) {
	// Anything LIT that comes to rest on the floor STAYS lit now (Michael,
	// 2026-10-03, lighting-updates Phase 4 - it used to go out here): it burns
	// on where it lies (TickFloorTorches) and lights its square. A pack is
	// still where a torch goes out.
	const Item& item = placed;
	// Every new floor item comes through here - a drop, a throw landing, a
	// burning torch turned drop - so its shadow joins the cubes here too.
	NoteItemCaster(item);
	for (Item& dead : m_items)
		if (dead.id < 0 && dead.collected) {
			dead = item;
			return;
		}
	m_items.push_back(item);
}

namespace {
// Spare room ReserveDropRoom keeps for drops that do not land in a dead slot:
// that many DIFFERENT items can lie newly on the floor before a drop grows the
// list. A party carries about forty slots between four members.
constexpr size_t kDropRoom = 64;
} // namespace

void DungeonWorld::ReserveDropRoom() {
	if (m_items.capacity() - m_items.size() < kDropRoom)
		m_items.reserve(m_items.size() + kDropRoom);
}

void DungeonWorld::PreloadItemKinds() {
	for (const CatalogEntry* def : m_project.AllItems()) ItemKindFor(def->id);
	// What an Earth light's stone draws as, found once here: looking a kind up
	// by name builds a string, and the stones draw every frame.
	m_stoneKind = nullptr;
	if (const fx::LightEffect* light = SpellLightKind();
		light && m_project.HasItem(light->StoneItem()))
		m_stoneKind = &ItemKindFor(std::string(light->StoneItem()));
}

// Floor items occupy the Medium 2x2 quarter grid (up to 4 per cell). Pick the
// quarter nearest the world point (wx,wz) that no other floor item here holds;
// if all four are taken, fall back to the geometrically nearest (overlap).
int DungeonWorld::FreeItemSlotNear(int cx, int cz, float wx, float wz, int self) const {
	u32 used = 0;
	for (size_t i = 0; i < m_items.size(); ++i) {
		if (static_cast<int>(i) == self) continue;
		const Item& it = m_items[i];
		if (it.collected || it.x != cx || it.z != cz) continue;
		if (it.slot >= 0 && it.slot < 4) used |= (1u << it.slot);
	}
	int bestFree = -1, bestAny = 0;
	float bestFreeD = 1e9f, bestAnyD = 1e9f;
	for (int s = 0; s < 4; ++s) {
		const Vec3 c = SlotCenter(cx, cz, SizeClass::Medium, s);
		const float d = (c.x - wx) * (c.x - wx) + (c.z - wz) * (c.z - wz);
		if (d < bestAnyD) { bestAnyD = d; bestAny = s; }
		if (!(used & (1u << s)) && d < bestFreeD) { bestFreeD = d; bestFree = s; }
	}
	return bestFree >= 0 ? bestFree : bestAny;
}

// Loads a prop PBR set once and caches it (shared across decorations, fires,
// and monsters): sRGB albedo + linear normal/height + ORM, with the
// same res→2k fallback the surfaces use (props ship at 2k, so higher tiers fall
// back). Returns null only if even the 2k albedo is absent — callers then keep
// their flat glTF material color.
const DungeonWorld::PropTextures* DungeonWorld::LoadPropTextures(const std::string& set) {
	// `texture = none` declares "flat glTF material by design" (the banner) —
	// a silent null, so the missing-set warning keeps meaning a real mistake.
	if (set.empty() || set == "none") return nullptr;
	auto it = m_propTextures.find(set);
	if (it != m_propTextures.end()) return it->second.get();
	PbrMaps maps = LoadPbrSet(set, /*required*/ false);
	if (!maps.albedo) return nullptr; // missing set: caller keeps its flat material
	auto pt = std::make_unique<PropTextures>();
	pt->albedo = std::move(maps.albedo);
	pt->normal = std::move(maps.normal);
	pt->mr = std::move(maps.mr);
	pt->heightScale = 0.03f;
	pt->res = m_settings.TextureSuffix();
	pt->flatNormal = maps.flatNormal;
	return m_propTextures.emplace(set, std::move(pt)).first->second.get();
}

int DungeonWorld::ReloadPropTextures() {
	const std::string tier = m_settings.TextureSuffix();
	int reloaded = 0;
	for (auto&& [set, pt] : m_propTextures) { // a flat_map yields proxy pairs
		if (pt->res == tier) continue;
		// FREE, THEN LOAD: the old maps go before the new ones arrive, so the
		// swap never holds both tiers at once - on the SRV heap (whose peak a
		// quality swap must not raise; docs/ARCHITECTURE.md) or in VRAM.
		pt->albedo.reset();
		pt->normal.reset();
		pt->mr.reset();
		PbrMaps maps = LoadPbrSet(set, /*required*/ false);
		pt->albedo = std::move(maps.albedo); // null only if the files vanished:
		pt->normal = std::move(maps.normal); // the draw then falls back to the
		pt->mr = std::move(maps.mr);         // kind's flat colour, as at load
		pt->flatNormal = maps.flatNormal;
		pt->res = tier;
		++reloaded;
	}
	return reloaded;
}

// Binds an albedo+normal+ORM trio onto a material (ORM drives metallic/roughness
// per-texel, factors at 1.0), or a flat color + roughness fallback when there is
// no albedo. The shared core of every textured draw (props and surfaces).
void DungeonWorld::ApplyPbr(gfx::MaterialParams& m, const gfx::Texture* albedo,
							const gfx::Texture* normal, const gfx::Texture* mr,
							float heightScale, const Vec4& fallbackColor,
							float fallbackRoughness) {
	if (albedo) {
		m.albedo = albedo;
		m.normalMap = normal;
		m.heightScale = heightScale;
		if (mr) {
			m.metalRough = mr;
			m.metallic = 1.0f;
			m.roughness = 1.0f;
		}
	} else {
		m.baseColor = fallbackColor;
		m.roughness = fallbackRoughness;
	}
}

// Fills a draw's material from a prop texture set, or falls back to a flat color
// + roughness when the set is missing. Shared by every textured prop draw.
void DungeonWorld::ApplyPropMaterial(gfx::MaterialParams& m,
									 const PropTextures* tex,
									 const Vec4& fallbackColor, float fallbackRoughness) {
	ApplyPbr(m, tex ? tex->albedo.get() : nullptr, tex ? tex->normal.get() : nullptr,
			 tex ? tex->mr.get() : nullptr, tex ? tex->heightScale : 0.0f,
			 fallbackColor, fallbackRoughness);
}

void DungeonWorld::ApplyPropMaterial(gfx::MaterialParams& m,
									 const DecorationKind& kind,
									 float fallbackRoughness) {
	ApplyPropMaterial(m, kind.tex, kind.color, fallbackRoughness);
	if (kind.metallic >= 0.0f) m.metallic = kind.metallic;
	if (kind.roughness >= 0.0f) m.roughness = kind.roughness;
	if (kind.heightScale >= 0.0f && m.albedo) m.heightScale = kind.heightScale;
	if (kind.hasTint) m.baseColor = kind.tint;
	m.transparent = kind.transparent;
}

// Bakes a catalog entry's material overrides (metallic=/roughness=/color=, the
// asset dialog's sliders) into an authored model's per-submesh materials. The
// values REPLACE each submesh's factors — with the model's own maps the shader
// multiplies them per-texel, so they scale the authored material. height_scale
// does not apply here (embedded glTF textures carry no height map).
void DungeonWorld::BakeCatalogMaterial(MultiMaterialModel& model,
									   const CatalogEntry* def) {
	if (!def) return;
	const float metallic = def->GetFloat("metallic", -1.0f);
	const float roughness = def->GetFloat("roughness", -1.0f);
	Vec4 tint;
	const bool hasTint = CatalogColor(def, "color", tint);
	// `transparent = 1` makes the WHOLE model glass; absent leaves each part as its
	// glTF material says (alphaMode BLEND), which is how a bottle keeps a solid cork.
	const bool transparent = CatalogBool(def, "transparent", false);
	for (auto& sub : model.subs) {
		if (metallic >= 0.0f) sub.material.metallic = metallic;
		if (roughness >= 0.0f) sub.material.roughness = roughness;
		if (hasTint) sub.material.baseColor = tint;
		if (transparent) sub.material.transparent = true;
	}
}

// --- a catalog set on a multi-material model (code-review C301) --------------
bool DungeonWorld::SetDressesWholeModel(size_t parts, std::string_view file) {
	// A single-primitive .gltf is what an editor import writes - one merged mesh
	// whose look IS the set imported beside it - and what the world's single-mesh
	// prop draw binds a set over outright. A .glb, or a model of many parts, wears
	// its own materials (the bought packs) and the set only fills a bare part.
	return parts == 1 && file.ends_with(".gltf");
}

void DungeonWorld::WearModelSet(MultiMaterialModel& model, const PropTextures* set,
								std::string_view file) {
	if (!set) return; // a set not installed (warned by LoadPropTextures), or "none"
	const bool whole = SetDressesWholeModel(model.subs.size(), file);
	for (MultiMaterialModel::Sub& sub : model.subs) {
		if (whole) {
			// A fresh material, as the single-mesh draw builds it: the set and
			// none of the file's factors. The culling stays the file's.
			gfx::MaterialParams m;
			m.doubleSided = sub.material.doubleSided;
			ApplyPropMaterial(m, set, {1.0f, 1.0f, 1.0f, 1.0f}, 0.9f);
			sub.material = m;
		} else if (!sub.material.albedo) {
			ApplyPropMaterial(sub.material, set, sub.material.baseColor, sub.material.roughness);
		} else {
			continue; // painted by the file itself
		}
		// The maps are looked up as the part draws (PartMaterial), never kept
		// here: a quality swap frees them, and one left here would outlive them.
		sub.material.albedo = nullptr;
		sub.material.normalMap = nullptr;
		sub.material.metalRough = nullptr;
		sub.set = set;
	}
}

gfx::MaterialParams DungeonWorld::PartMaterial(const MultiMaterialModel::Sub& sub) {
	gfx::MaterialParams m = sub.material;
	if (sub.set) {
		// The set's maps as they are NOW - after a quality swap, the reloaded
		// ones. Null (the files vanished) leaves the part its flat colour.
		m.albedo = sub.set->albedo.get();
		m.normalMap = sub.set->normal.get();
		m.metalRough = sub.set->mr.get();
	}
	return m;
}

// Builds an authored model's own GPU resources: one texture per embedded glTF
// image (base-color maps sRGB, normal/MR linear) and one submesh per primitive,
// each with a MaterialParams resolved from its glTF material. Lets a bought
// multi-material model render every part with its real material.
std::unique_ptr<DungeonWorld::MultiMaterialModel> DungeonWorld::BuildMultiMaterialModel(
	gfx::GraphicsDevice& device, const assets::ModelData& model) {
	// A model the cache has already taken the images from would build every
	// texture from an empty image; ModelMulti reads the file again instead.
	DN_ASSERT(!assets::ImagesReleased(model),
			  "BuildMultiMaterialModel: the model's images were released");
	auto out = std::make_unique<DungeonWorld::MultiMaterialModel>();
	// The same answer the mip bake used for each sidecar (assets::SrgbImages).
	const std::vector<bool> srgb = assets::SrgbImages(model);
	out->textures.reserve(model.images.size());
	for (size_t i = 0; i < model.images.size(); ++i) {
		// A baked BC7 chain (AssetBaker mips) uploads as it is; otherwise the
		// decoded image gets its mips built here, as before the bake existed.
		const bool baked = i < model.imageMips.size() && !model.imageMips[i].levels.empty();
		out->textures.push_back(
			baked ? std::make_unique<gfx::Texture>(device, model.imageMips[i], srgb[i])
				  : std::make_unique<gfx::Texture>(device, model.images[i], srgb[i]));
	}

	auto texAt = [&](int img) -> const gfx::Texture* {
		return img >= 0 ? out->textures[static_cast<size_t>(img)].get() : nullptr;
	};
	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	// Each part in the MODEL's space: its glTF node transform baked into the
	// vertices (it carries ConvertMesh's normalization scale and placement).
	// The loader keeps the node beside the mesh and leaves the vertices in node
	// space; import-model does this same bake when merging, and here each part
	// keeps its own. The baked parts are kept for the handle measurement below,
	// so it reads the vertices exactly as they were uploaded.
	std::vector<assets::MeshData> baked(model.meshes.begin(), model.meshes.end());
	for (size_t i = 0; i < baked.size(); ++i) {
		const assets::MeshData& mesh = model.meshes[i];
		DungeonWorld::MultiMaterialModel::Sub sub;
		assets::BakeNodeTransform(baked[i]);
		for (const assets::Vertex& v : baked[i].vertices) {
			const Vec3& p = v.position;
			lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
			hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
		}
		sub.mesh = std::make_unique<gfx::Mesh>(device, baked[i]);
		sub.material.doubleSided = false; // authored, consistently wound -> back-cull
		if (mesh.material >= 0 &&
			mesh.material < static_cast<int>(model.materials.size())) {
			const assets::MaterialData& md = model.materials[mesh.material];
			sub.material.baseColor = md.baseColorFactor;
			sub.material.metallic = md.metallic;
			sub.material.roughness = md.roughness;
			sub.material.emissive = md.emissive;
			sub.material.albedo = texAt(md.baseColorImage);
			sub.material.normalMap = texAt(md.normalImage);
			sub.material.metalRough = texAt(md.metalRoughImage);
			sub.material.transparent = md.blend; // a glass part, a cork stays solid
		}
		out->subs.push_back(std::move(sub));
	}
	out->boundsMin = lo;
	out->boundsMax = hi;

	// The long axis, and which of its two ends is the HANDLE: the half that is
	// THICKEST, measured across the model's thinnest axis. A blade is flat - thin
	// across that axis all the way to its point - while a grip, a guard and a
	// pommel are round and stand out of the blade's plane. (Measuring the widest
	// reach instead is fooled by a curved blade like the khukri, whose belly
	// bulges IN its own plane.) On anything not long and thin the answer is never
	// asked for.
	const float ext[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
	const int a = ext[0] >= ext[1] && ext[0] >= ext[2] ? 0 : (ext[1] >= ext[2] ? 1 : 2);
	const int b = (a + 1) % 3, d = (a + 2) % 3;
	const int thin = ext[b] <= ext[d] ? b : d;
	const float c[3] = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
	float reach[2] = {0.0f, 0.0f}; // [0] the negative half, [1] the positive half
	for (const assets::MeshData& mesh : baked)
		for (const assets::Vertex& v : mesh.vertices) {
			const float p[3] = {v.position.x, v.position.y, v.position.z};
			float& r = reach[p[a] >= c[a] ? 1 : 0];
			r = std::max(r, std::abs(p[thin] - c[thin]));
		}
	out->longAxis = a;
	out->handleSign = reach[1] >= reach[0] ? 1.0f : -1.0f;
	return out;
}

// Each decoration type resolves through the decorations catalog: its model
// (assets/models/<model>, as .gltf or .glb - ModelFileOf), its texture set
// (procedural props share a dungeon-stone/wood-plank set, authored imports carry
// their own), whether it is back-face culled (authored), and whether a
// floor-standing instance blocks the party (passages like the archway don't). An
// unlisted type falls back to the old convention: same-named model + set,
// authored, solid.
DungeonWorld::DecorationKind& DungeonWorld::DecorationKindFor(const std::string& type,
															 const Catalog& catalog) {
	auto it = m_decorationKinds.find(type);
	if (it == m_decorationKinds.end()) {
		const CatalogEntry* def = catalog.Find(type);
		// Whichever of .gltf / .glb is installed, the .glb first for a
		// `multimaterial` entry - ModelFileOf's rule (code-review C301).
		const std::string file = ModelFileOf(ModelFamily::Prop, def, type);
		const std::string tex = TextureOf(def, type);
		auto kind = std::make_unique<DecorationKind>();
		kind->id = type; // the record type, for the .map writer
		kind->authored = CatalogBool(def, "authored", true);
		kind->facingArrow = CatalogBool(def, "facing_arrow", true);
		// Catalog material overrides (the asset dialog's sliders): absent = -1 /
		// no tint = the resolved material stays untouched.
		if (def) {
			kind->metallic = def->GetFloat("metallic", -1.0f);
			kind->roughness = def->GetFloat("roughness", -1.0f);
			kind->heightScale = def->GetFloat("height_scale", -1.0f);
			kind->hasTint = CatalogColor(def, "color", kind->tint);
			// Uniform size trim on top of the authored unit size (monsters' long-
			// standing `modelscale`, now available to every prop): 1 = as authored.
			kind->modelScale = def->GetFloat("scale", 1.0f);
			// BREAKABILITY, opt-in and OFF by default: a prop is scenery unless its
			// type says otherwise. `hp` is how much it takes, `armor`/`resists` how
			// it takes it — the same two fields armour wears, so a stone statue can
			// shrug off a blade and an iron grate can drink lightning.
			kind->breakable = CatalogBreakable(def);
			if (kind->breakable) {
				kind->hp = def->GetFloat("hp", 10.0f);
				kind->soak = def->GetFloat("armor", 0.0f);
				ParseResists(CatalogGet(def, "resists", ""), kind->resists,
							 "decorations.cat [" + type + "]", m_damageTypes);
			}
		}
		// Every kind bakes a whole-model map icon; a fresh kind re-arms the
		// one-shot bake pass (UpdateMapIcons).
		kind->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
		m_decorationIconsBaked = false;
		// Authored multi-material models (bought weapon/prop packs) render their
		// own glTF textures per material from a single embedded-texture .glb,
		// bypassing the single-mesh / one-bound-set path below.
		if (CatalogBool(def, "multimaterial", false)) {
			kind->model = ModelFile(file);
			kind->multi = ModelMulti(file); // shared GPU, own materials
			BakeCatalogMaterial(*kind->multi, def); // overrides baked per submesh
			kind->solidDefault = CatalogBool(def, "solid", true);
			// From the bounds of what was UPLOADED (node transforms baked). The raw
			// vertices are in node space: a bought dagger normalized by its node's
			// scale read ~115 times too big, so it was never culled (C253).
			kind->cullRadius = OriginRadius(kind->multi->boundsMin, kind->multi->boundsMax) *
							   kUnit * kind->modelScale;
			it = m_decorationKinds.emplace(type, std::move(kind)).first;
			return *it->second;
		}
		kind->model = ModelFile(file); // marble + stone columns share one
		kind->mesh = ModelMesh(file);
		kind->color = kind->model->materials[0].baseColorFactor;
		kind->tex = LoadPropTextures(tex);
		kind->solidDefault = CatalogBool(def, "solid", true);
		// Optional alpha-test cutout (a masked set like wood planks renders its
		// gaps); absent/0 = opaque, the usual case.
		kind->alphaCutoff = def ? def->GetFloat("alpha_test", 0.0f) : 0.0f;
		kind->transparent = CatalogBool(def, "transparent", false);
		kind->cullRadius = ModelOriginRadius(*kind->model) * kUnit * kind->modelScale;
		it = m_decorationKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

// Fixture counterpart of DecorationKindFor: resolves a fixtures.cat id into
// its renderable assets once and caches it. An unknown id still resolves (the
// ModelFileOf / TextureOf fallback names the id itself) so a stale record
// aborts with a clear missing-model message instead of silently vanishing.
DungeonWorld::FixtureKind& DungeonWorld::FixtureKindFor(const std::string& type) {
	auto it = m_fixtureKinds.find(type);
	if (it == m_fixtureKinds.end()) {
		const CatalogEntry* def = m_project.fixtures.Find(type);
		if (!def) log::Warn("fixture kind '{}' is not in fixtures.cat", type);
		const std::string file = ModelFileOf(ModelFamily::Fixture, def, type);
		const std::string set = TextureOf(def, type);
		auto kind = std::make_unique<FixtureKind>();
		kind->id = type;
		kind->wallMount = CatalogGet(def, "mount", "floor") == "wall";
		kind->flameless = !CatalogBool(def, "flame", true);
		kind->light = CatalogGet(def, "light", kind->wallMount ? "fire_sconce" : "fire_brazier");
		// Breakability, opt-in and OFF by default like every other kind: a torch
		// bracket can be knocked off a wall, a heavy iron brazier takes rather more,
		// and an empty one authored without the field cannot be touched at all.
		kind->breakable = CatalogBreakable(def);
		if (kind->breakable && def) {
			kind->hp = def->GetFloat("hp", 10.0f);
			kind->soak = def->GetFloat("armor", 0.0f);
			ParseResists(CatalogGet(def, "resists", ""), kind->resists,
						 "fixtures.cat [" + type + "]", m_damageTypes);
		}
		// What going out leaves on the fire (`on_douse = smoke 0.8 2.5`).
		fx::ParseProcs(CatalogGet(def, "on_douse", ""), kind->onDouse,
					   "fixtures.cat [" + type + "] on_douse");
		kind->model = ModelFile(file);
		kind->mesh = ModelMesh(file);
		kind->color = kind->model->materials[0].baseColorFactor;
		kind->tex = LoadPropTextures(set);
		// A takeable torch: the bare bracket left behind, and what it becomes.
		if (const std::string empty = ModelFileOf(ModelFamily::Fixture, def, type, "empty_model");
			!empty.empty()) {
			kind->meshEmpty = ModelMesh(empty);
			kind->torchItem = CatalogGet(def, "torch_item", "torch");
		}
		// Flame attachment: catalog fields override the mount's defaults so an
		// authored prop's fire burns where its bowl/basket actually is.
		kind->flame = kind->wallMount
						  ? FixtureFlame{kSconceFlameY, kSconceFlameScale, 0.088f}
						  : FixtureFlame{kBrazierFlameY, kBrazierFlameScale, 0.0f};
		if (def) {
			kind->modelScale = def->GetFloat("scale", 1.0f);
			kind->flame.height = def->GetFloat("flame_height", kind->flame.height);
			kind->flame.scale = def->GetFloat("flame_scale", kind->flame.scale);
			kind->flame.out = def->GetFloat("flame_out", kind->flame.out);
			// Optional second part (part2_model / part2_texture): a co-located
			// sub-prop with its own material — the bought brazier's coal bed
			// (the two models were normalized TOGETHER at import, so their
			// placements already align).
			if (const std::string model2 = def->Get("part2_model"); !model2.empty()) {
				const std::string file2 =
					ModelFileOf(ModelFamily::Fixture, def, type, "part2_model");
				kind->mesh2 = ModelMesh(file2);
				kind->color2 = ModelFile(file2)->materials[0].baseColorFactor;
				kind->tex2 = LoadPropTextures(def->Get("part2_texture", model2));
			}
		}
		// Every kind bakes a whole-model map icon; a fresh kind re-arms the
		// one-shot bake pass (UpdateMapIcons).
		kind->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
		m_fixtureIconsBaked = false;
		it = m_fixtureKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

// Loads each decoration model once (shared per type, like monsters) and bakes
// one placed instance per .map "decoration" record. Authored facing +Z, so a
// record's facing rotates the prop the same way a monster's does. Everything
// is solid (blocks the party) except open passages like the archway; a
// "solid=0"/"solid=1" param on the record overrides the default.
void DungeonWorld::LoadDecorations() {
	for (const Entity& record : m_map.Decorations()) {
		DecorationKind& kind = DecorationKindFor(record.type, m_project.decorations);
		Decoration deco;
		deco.kind = &kind;
		deco.x = record.x;
		deco.z = record.z;

		// "wall=<dir>" hangs the prop flat on that wall (offset to the wall face,
		// turned to face the room) — the same mount sconces use, so several wall
		// fixtures can share a cell on different walls. Such props sit on the
		// wall, so they don't block the floor unless solid=1 is given. Without
		// wall=, the prop stands at the cell centre with its facing rotation.
		Direction wall = Direction::North;
		const std::string* wallParam = record.Param("wall");
		const bool wallMounted = wallParam && ParseDirection(*wallParam, wall);
		deco.facing = record.facing;
		deco.wallMounted = wallMounted;
		deco.wall = wall;
		if (wallMounted) {
			const WallMount m = MountOnWall(deco.x, deco.z, wall);
			XMStoreFloat4x4(&deco.world, UnitScale(kind.modelScale) * XMMatrixRotationY(m.yaw) *
											 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
			deco.solid = false;
		} else {
			const Vec3 pos = m_map.CellCenter(deco.x, deco.z);
			XMStoreFloat4x4(&deco.world, UnitScale(kind.modelScale) *
											 XMMatrixRotationY(DirYaw(record.facing)) *
											 XMMatrixTranslation(pos.x, 0, pos.z));
			deco.solid = kind.solidDefault; // passages (archway) let the party through
		}
		if (const std::string* s = record.Param("solid")) deco.solid = *s != "0";
		SeedBreakable(deco.brk, kind);
		m_decorations.push_back(std::move(deco));
	}
	log::Info("Placed {} decorations ({} kinds)", m_decorations.size(),
			  m_decorationKinds.size());
}

// Places a stair prop per map "stairs" record (P6). Stairs render through the
// decoration machinery (kind resolved from stairs.cat) but are always non-solid
// so the party can step onto them; the transition link itself lives in
// DungeonMap::Stairs() and is consumed in the party step callback.
void DungeonWorld::LoadStairs() {
	for (const StairLink& s : m_map.Stairs()) PlaceStairProp(s);
	if (!m_map.Stairs().empty())
		log::Info("Placed {} stairs", m_map.Stairs().size());
	ReserveFallRoom();
}

// Room in the kept pit-fall transition for any level a pit here could name, so
// the step that latches one assigns into it rather than growing it in a frame
// the guard arms (code-review C210). Grow-only: a reserve never shrinks.
void DungeonWorld::ReserveFallRoom() {
	size_t room = kFallLevelRoom;
	for (const std::string& stem : m_project.levels) room = std::max(room, stem.size());
	for (const StairLink& s : m_map.Stairs()) room = std::max(room, s.destLevel.size());
	m_fall.level.reserve(room);
}

// A stair's facing is the way you face stepping OFF it (StairLink::facing), and
// every stair mesh has its FOOT - the end you step on and off at floor level -
// at +Z: BuildStairs' lowest tread sits at the front and the flight climbs
// toward -Z, and BuildStairsDown's shaft is entered at its front edge. So the
// prop turns its +Z to the facing, like any other prop (DirYaw), and the flight
// rises (or the shaft drops) BEHIND whoever steps off it.
//
// It used to turn to the OPPOSITE, on the belief that +Z was the way you travel
// on the mesh. That inverted every stair: a way out facing north climbed north,
// straight up in front of the party it had just delivered (play-test, Michael,
// 2026-09-28: "the in-game object is the wrong way around"). It survived because
// the facing's meaning had just been flipped and the opposite turn was chosen to
// keep every stair looking as it did - and as it did was already wrong.
Mat4 DungeonWorld::StairPropWorld(const DecorationKind& kind, int x, int z,
								  Direction facing) const {
	const Vec3 pos = m_map.CellCenter(x, z);
	Mat4 world;
	XMStoreFloat4x4(&world, UnitScale(kind.modelScale) *
								XMMatrixRotationY(DirYaw(facing)) *
								XMMatrixTranslation(pos.x, 0, pos.z));
	return world;
}

Direction DungeonWorld::ArrivalFacingAt(int x, int z) const {
	const StairLink* s = m_map.StairAt(x, z);
	// Scenery that only LOOKS like a stair (a pit's ceiling hole, traverse = 0)
	// is not a way you came in by.
	if (!s || !CatalogBool(m_project.stairs.Find(s->type), "traverse", true))
		return Direction::South;
	return s->facing;
}

void DungeonWorld::PlaceStairProp(const StairLink& s) {
	DecorationKind& kind = DecorationKindFor(s.type, m_project.stairs);
	Decoration deco;
	deco.kind = &kind;
	deco.x = s.x;
	deco.z = s.z;
	deco.facing = s.facing;
	deco.stair = true; // written as a stairs record, not a decoration
	deco.world = StairPropWorld(kind, s.x, s.z, s.facing);
	deco.solid = false; // the party walks onto a stair to use it
	m_decorations.push_back(std::move(deco));
}

// Origin pushed to the wall face, +Z (authored front) turned to face the room.
DungeonWorld::WallMount DungeonWorld::MountOnWall(int x, int z, Direction wall) const {
	const int dx = DirDX(wall), dz = DirDZ(wall);
	const Vec3 c = m_map.CellCenter(x, z);
	return {{c.x + dx * (kCellSize * 0.5f - 0.02f), 0.0f,
			 c.z + dz * (kCellSize * 0.5f - 0.02f)},
			std::atan2(static_cast<float>(-dx), static_cast<float>(-dz))};
}

// Places one Fire per sconce ('T') and brazier ('F') cell. Sconces mount on
// the first solid neighbor wall and face into the room; braziers stand at
// the cell center. Flame origins match the baked models (see ModelBaker).
void DungeonWorld::BuildFires() {
	u32 seed = 1234;
	// A fire's light is keyed by its index here, so a rebuilt list (a level
	// change, an edit) starts the budget fades afresh.
	ClearLightFades();

	for (const WallSconce& sconce : m_map.Sconces()) {
		const FixtureKind& kind = FixtureKindFor(sconce.type);
		// Hang on the wall resolved at map load (shared with decorations).
		const WallMount m = MountOnWall(sconce.x, sconce.z, sconce.wall);
		const float yaw = m.yaw;

		Fire fire;
		fire.kind = &kind;
		fire.brazier = false;
		fire.lit = sconce.Burning() && !kind.flameless;
		fire.x = sconce.x;
		fire.z = sconce.z;
		fire.wall = static_cast<int>(sconce.wall);
		fire.empty = sconce.empty;
		fire.lightRadius = sconce.brightness * kCellSize; // "squares" -> metres
		fire.flameColor = sconce.flameColor;
		const float fs = kind.modelScale; // fixtures.cat `scale`
		XMStoreFloat4x4(&fire.world, UnitScale(fs) * XMMatrixRotationY(yaw) *
										 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
		// Flame local offset (0, height, out) rotated by yaw (fixtures.cat
		// flame_* fields; defaults = the mount's procedural constants). Those are
		// points on the model, so they are UNITS -> metres here like the mesh.
		fire.flamePos = {m.pos.x + std::sin(yaw) * kind.flame.out * kUnit * fs,
						 kind.flame.height * kUnit * fs,
						 m.pos.z + std::cos(yaw) * kind.flame.out * kUnit * fs};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, kind.flame.scale * fs, seed++);
		fire.effect.SetFlameColor(fire.flameColor, HasFlameColor(fire.flameColor));
		fx::ReserveEffects(fire.effects); // a douse lands its smoke here mid-play
		m_fires.push_back(std::move(fire));
	}

	for (const FloorBrazier& b : m_map.Braziers()) {
		const FixtureKind& kind = FixtureKindFor(b.type);
		const Vec3 center = m_map.CellCenter(b.x, b.z);
		Fire fire;
		fire.kind = &kind;
		fire.brazier = true;
		fire.lit = b.Burning() && !kind.flameless;
		fire.x = b.x;
		fire.z = b.z;
		fire.lightRadius = b.brightness * kCellSize; // "squares" -> metres
		fire.flameColor = b.flameColor;
		const float fs = kind.modelScale; // fixtures.cat `scale`
		XMStoreFloat4x4(&fire.world,
						UnitScale(fs) * XMMatrixTranslation(center.x, 0, center.z));
		fire.flamePos = {center.x, kind.flame.height * kUnit * fs, center.z};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, kind.flame.scale * fs, seed++);
		fire.effect.SetFlameColor(fire.flameColor, HasFlameColor(fire.flameColor));
		fx::ReserveEffects(fire.effects);
		m_fires.push_back(std::move(fire));
	}
	log::Info("Lit {} fires ({} sconces, {} braziers, {} kinds)", m_fires.size(),
			  m_map.Sconces().size(), m_map.Braziers().size(),
			  m_fixtureKinds.size());
	// The fire set is what the per-frame billboard buffer is sized from, and this
	// is the only place it changes (monsters load before fires, so their plume
	// allowance is countable here too).
	ReserveParticleScratch();
}

// Per-cell turbidity as a top-down density grid: one texel per dungeon cell,
// R channel; bilinear filtering blends region borders. The scene shader
// raymarches it (see scene.hlsl).
void DungeonWorld::FillTurbidityPixels() {
	const size_t w = static_cast<size_t>(m_map.Width());
	m_turbidityPixels.assign(w * static_cast<size_t>(m_map.Height()) * 4, 0);
	for (int z = 0; z < m_map.Height(); ++z) {
		for (int x = 0; x < m_map.Width(); ++x) {
			const size_t i = (static_cast<size_t>(z) * w + x) * 4;
			m_turbidityPixels[i + 0] = static_cast<u8>(m_map.Turbidity(x, z) * 255.0f);
			m_turbidityPixels[i + 3] = 255;
		}
	}
}

void DungeonWorld::RefreshTurbidity() {
	// The in-place path: the same pixels rewritten (assign() keeps the capacity,
	// so a same-size map allocates nothing) and copied into the EXISTING texture
	// by the next RenderScene. Only when the map changed SIZE - which no fixture
	// break can do - does it fall back to building a new texture.
	if (!m_turbidityMap ||
		m_turbidityMap->Width() != static_cast<u32>(m_map.Width()) ||
		m_turbidityMap->Height() != static_cast<u32>(m_map.Height())) {
		BuildTurbidityMap();
		return;
	}
	FillTurbidityPixels();
	m_turbidityDirty = true;
}

void DungeonWorld::BuildTurbidityMap() {
	FillTurbidityPixels();
	// ONE mip, so RefreshTurbidity can rewrite the whole texture by its top level.
	// The shader samples it at level 0 (SampleLevel in scene.hlsl); the box-filtered
	// chain the ImageData constructor would build was never read.
	assets::MipChain grid;
	grid.width = static_cast<u32>(m_map.Width());
	grid.height = static_cast<u32>(m_map.Height());
	grid.levels.push_back({grid.width, grid.height, m_turbidityPixels});
	m_turbidityMap = std::make_unique<gfx::Texture>(m_device, grid);
	m_turbidityDirty = false; // the new texture already holds these pixels
	m_atmosphere.turbidityMap = m_turbidityMap.get();
	m_atmosphere.worldExtent = {m_map.Width() * kCellSize,
								m_map.Height() * kCellSize};
	// Apply the level's atmosphere overrides (the .map `atmosphere` record /
	// the editor's Level settings dialog); unset values fall back to the
	// global defaults. Done here because every path that changes the level's
	// air — initial load, level swap, fixture edits — ends in this rebuild.
	float dust, haze, ambient;
	EffectiveAtmosphere(m_map, dust, haze, ambient);
	m_atmosphere.density = dust;
	m_atmosphere.hazeAmbient = haze;
	SetAmbientScale(ambient);
}

// ============================================================================
// Quality hot-swap (see GameSettings's Quality) — the worn blocks exist at
// three baked tessellation levels and the scanned textures at three
// resolutions; switching reloads both and rebuilds the batched dungeon
// meshes in place. Every texture set loaded AT A TIER swaps: the surfaces and
// every prop set (props, doors, fixtures, monsters, runes - one cache,
// m_propTextures). What carries no tier stays: a multi-material model's
// embedded images, the UI art, the baked icons.
// ============================================================================
void DungeonWorld::ApplyQuality(bool textureResChanged) {
	// THE PROPS FIRST, and whether or not the surfaces are built: a prop set
	// cached at the old tier would otherwise be handed out at that tier for the
	// rest of the session (code-review C154 - LoadPropTextures looks up by name).
	int props = 0;
	if (textureResChanged && !m_propTextures.empty()) {
		m_device.WaitIdle(); // in-flight frames still sample the old maps
		props = ReloadPropTextures();
	}
	if (!m_walls.chunks.empty()) // not built yet: the load tasks will
		ReloadDungeonBlocks(textureResChanged);
	log::Info("Quality switched to {} ({} meshes, {} textures; {} prop set(s) reloaded)",
			  m_settings.QualityLabel(), m_settings.MeshSuffix(),
			  m_settings.TextureSuffix(), props);
}

// Reloads the worn block meshes and rebuilds the batched dungeon geometry in
// place - shared by the quality hot-swap and a surface type's RESTYLE (the type
// editor saving a `rebakes` field: Game::StartRestyleBake re-bakes the texture's
// worn_*.gltf, then calls this to swap it in live).
// The map Revision is unchanged, so the cached shadow cubes are force-refreshed.
void DungeonWorld::ReloadDungeonBlocks(bool textureResChanged) {
	if (m_walls.chunks.empty()) return; // not built yet — the load tasks will

	// The GPU may still be reading the old resources, so drain it first.
	m_device.WaitIdle();
	// Re-resolve palettes so an edited `wear` updates BOTH the worn mesh (via
	// LoadDungeonBlocks below) and the parallax depth together — the two must
	// move in lockstep or a flat mesh still shows faked relief.
	// Keep the sets we currently have LOADED: a re-resolve can now point a
	// variant at a different texture entirely (the type editor's `texture` field),
	// and the loaded texture arrays would otherwise keep painting the old set
	// under the new worn mesh — a clone repointed at another texture went on
	// looking exactly like the type it was cloned from.
	const std::vector<std::string> wallsWere = m_wallSets, floorsWere = m_floorSets,
								   ceilingsWere = m_ceilingSets;
	ResolveSurfacePalettes();
	const bool setsChanged = wallsWere != m_wallSets || floorsWere != m_floorSets ||
							 ceilingsWere != m_ceilingSets;
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	// Always a fresh read here, never the level-change skip: the restyle rebake
	// calls this precisely because a worn_*.gltf CHANGED under the same name, so
	// "same names as last time" proves nothing about the contents.
	m_loadedBlocks.reset();
	m_featureMeshCache.clear(); // the per-type maps are rebuilt from it next
	LoadDungeonBlocks();
	if (textureResChanged || setsChanged)
		LoadAllSurfaceTextures(); // re-pushes each variant's parallax depth
	else                          // textures unchanged — just refresh the depths
		for (const SurfaceDef& def : SurfaceDefs())
			def.surface.heightScale.assign(def.heights.begin(), def.heights.end());
	BuildDungeonMeshes();
	m_shadows.InvalidateCubes();
	// This IS the deferred work FlushGeometry would do (BuildDungeonMeshes
	// clears m_geometryDirty itself), so a pending restore doesn't repeat it.
	m_surfacesDirty = false;
}
} // namespace dungeon::game
