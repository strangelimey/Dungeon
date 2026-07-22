// ============================================================================
// Game/DungeonWorld_Load.cpp — asset loading + content construction for
// DungeonWorld (declarations in DungeonWorld.h). Split out of DungeonWorld.cpp:
// the staged-load tasks, surface palettes/textures/worn blocks, the monster/
// item/decoration/fixture kind factories, fires, the turbidity map, item
// pickup/drop, and the quality hot-swap.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <queue>

using namespace DirectX;

namespace dungeon::game {

// A catalog entry's model + texture asset names, both defaulting to `fallback`
// (usually the id) when the entry or field is absent. Shared by the monster,
// decoration, and fixture loaders.
static std::pair<std::string, std::string> ModelAndTexture(const CatalogEntry* e,
														   const std::string& fallback) {
	return {CatalogGet(e, "model", fallback), CatalogGet(e, "texture", fallback)};
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

// Parses a catalog "color" field — "r,g,b[,a]" floats 0..1 — into `out`.
// Malformed values leave `out` untouched and return false (the field is then
// ignored, like an absent one).
static bool ParseColorField(const std::string& s, Vec4& out) {
	const std::vector<std::string> t = SplitTokens(s);
	if (t.size() < 3) return false;
	Vec4 c{0, 0, 0, 1};
	float* dst[4] = {&c.x, &c.y, &c.z, &c.w};
	for (size_t i = 0; i < t.size() && i < 4; ++i) {
		char* end = nullptr;
		*dst[i] = std::strtof(t[i].c_str(), &end);
		if (end == t[i].c_str()) return false;
	}
	out = c;
	return true;
}

// monsters.cat `archetype` token -> the behaviour strategy enum. Unknown tokens
// warn and fall back to brute (the pre-archetype behaviour), so a typo is loud but
// never fatal and an undescribed monster keeps working.
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
	return {{{m_walls, m_wallSets, m_wallHeights},
			 {m_floors, m_floorSets, m_floorHeights},
			 {m_ceilings, m_ceilingSets, m_ceilingHeights}}};
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
		float fallbackHeight;
	};
	const Def defs[] = {
		{m_map.WallPalette(), m_project.walls, m_wallSets, m_wallHeights, 0.055f},
		{m_map.FloorPalette(), m_project.floors, m_floorSets, m_floorHeights, 0.045f},
		{m_map.CeilingPalette(), m_project.ceilings, m_ceilingSets, m_ceilingHeights,
		 0.035f},
	};
	for (const Def& d : defs) {
		d.sets.clear();
		d.heights.clear();
		for (const std::string& id : d.palette) {
			const CatalogEntry* e = d.catalog.Find(id);
			d.sets.push_back(CatalogGet(e, "texture", id));
			const float h = e ? e->GetFloat("height_scale", d.fallbackHeight)
							  : d.fallbackHeight;
			const float wear = e ? std::clamp(e->GetFloat("wear", 1.0f), 0.0f, 1.0f)
								 : 1.0f;
			d.heights.push_back(h * wear);
		}
	}
}

void DungeonWorld::AppendLoadTasks(LoadQueue& queue) {
	queue.Add(loc::Tr("load.blocks"), [this] { LoadDungeonBlocks(); });

	// One task per material (the scanned sets dominate the load); the first
	// material of each set resets the surface, exactly as LoadTextureSet does.
	for (const SurfaceDef& def : SurfaceDefs()) {
		Surface& surface = def.surface;
		for (size_t i = 0; i < def.names.size(); ++i) {
			const std::string& name = def.names[i];
			const float heightScale = def.heights[i]; // per-variant parallax depth
			const bool first = i == 0; // first material resets the set
			std::string spaced = name; // asset id, shown with the '_'s opened up
			std::ranges::replace(spaced, '_', ' ');
			queue.Add(loc::Format("load.surface", spaced),
					  [this, &surface, name, heightScale, first] {
						  if (first) surface.ResetTextures();
						  LoadSurfaceMaterial(surface, name, heightScale);
					  });
		}
	}

	queue.Add(loc::Tr("load.dungeon"), [this] { BuildDungeonMeshes(); });
	queue.Add(loc::Tr("load.monsters"), [this] {
		LoadMonsters();
		LoadItems();
		LoadButtons();
	});
	queue.Add(loc::Tr("load.decorations"), [this] {
		LoadDecorations();
		LoadStairs();
		LoadDoors(); // after decorations: shares the prop texture/model caches
	});
	queue.Add(loc::Tr("load.fires"), [this] {
		// Fixture kinds resolve lazily per placed record (FixtureKindFor, the
		// DecorationKind pattern) — BuildFires pulls in whatever the level uses.
		m_particleBatch = std::make_unique<gfx::ParticleBatch>(m_device);
		BuildFires();
	});
	queue.Add(loc::Tr("load.dust"), [this] { BuildTurbidityMap(); });
}

void DungeonWorld::LoadDungeonBlocks() {
	// The old dungeon uses the worn, crumbling block set — one mesh per
	// texture variant, displaced at bake time by that texture's height map
	// so geometry relief matches the painted bricks/slabs. The clean
	// *_block.gltf models remain baked for newer areas of the game.
	auto load = [&](std::vector<assets::MeshData>& blocks,
					std::span<const std::string> names) {
		blocks.clear();
		for (const std::string& name : names)
			blocks.push_back(
				LoadModelOrDie(
					std::format("worn_{}_{}.gltf", name, m_settings.MeshSuffix()))
					.meshes[0]);
	};
	load(m_wallBlocks, m_wallSets);
	load(m_floorBlocks, m_floorSets);
	load(m_ceilingBlocks, m_ceilingSets);

	// Wall-feature niche panels, one per wallfeatures.cat type (its `model`.gltf),
	// stamped per niche edge into the wall's variant bucket so they take the wall
	// texture (see DungeonMeshBuilder). Loaded once; a level references types.
	m_nicheMeshes.clear();
	m_boreMeshes.clear();
	for (const CatalogEntry& e : m_project.wallfeatures.Entries()) {
		const std::string model = CatalogGet(&e, "model", e.id);
		// A `bore` feature is a see-through window (its own mesh map); everything
		// else is a niche.
		(e.GetBool("bore", false) ? m_boreMeshes : m_nicheMeshes)
			.emplace(e.id, LoadModelOrDie(model + ".gltf").meshes[0]);
	}
}

const assets::MeshData* DungeonWorld::BoreMeshFor(const std::string& type) const {
	const auto it = m_boreMeshes.find(type);
	return it != m_boreMeshes.end() ? &it->second : nullptr;
}

const assets::MeshData* DungeonWorld::NicheMeshFor(const std::string& type) const {
	const auto it = m_nicheMeshes.find(type);
	return it != m_nicheMeshes.end() ? &it->second : nullptr;
}

// Loads a PBR set (albedo sRGB + normal/height + ORM) by base name at the
// current quality tier. Higher tiers' sets are fetchable content
// (tools/FetchTextures.ps1), so a missing one drops to the always-present 2k
// set. `required` (surfaces) dies if even the albedo is absent; otherwise
// (props) returns maps with a null albedo and the caller keeps its flat color.
// The single source of the res→2k fallback, shared by surfaces and props.
DungeonWorld::PbrMaps DungeonWorld::LoadPbrSet(const std::string& name, bool required) {
	const char* res = m_settings.TextureSuffix();
	std::string stem = paths::Asset(std::format("textures\\{}_{}", name, res));
	PbrMaps maps;
	maps.albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
	if (!maps.albedo) {
		stem = paths::Asset(std::format("textures\\{}_2k", name));
		if (required) {
			log::Warn("{} not found at {} — falling back to 2k", name, res);
			maps.albedo = LoadTextureFile(m_device, stem, /*srgb*/ true); // dies if absent
		} else {
			maps.albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
			if (!maps.albedo) {
				log::Warn("texture set '{}' not found — using flat material", name);
				return maps; // null albedo: caller falls back to a flat material
			}
		}
	}
	maps.normal = LoadTextureFile(m_device, stem + "_n"); // linear
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
	surface.albedo.push_back(std::move(maps.albedo));
	surface.normal.push_back(std::move(maps.normal));
	surface.mr.push_back(std::move(maps.mr));
	surface.heightScale.push_back(heightScale);
}

void DungeonWorld::LoadTextureSet(const SurfaceDef& def) {
	def.surface.ResetTextures(); // hot-swap reuses the same Surface
	for (size_t i = 0; i < def.names.size(); ++i)
		LoadSurfaceMaterial(def.surface, def.names[i], def.heights[i]);
}

void DungeonWorld::LoadAllSurfaceTextures() {
	for (const SurfaceDef& def : SurfaceDefs()) LoadTextureSet(def);
}

DungeonWorld::SurfaceChunk DungeonWorld::MakeSurfaceChunk(GeometryChunk& gc) {
	SurfaceChunk sc;
	sc.variant = gc.variant;
	sc.chunk = gc.chunk;
	sc.boundsMin = gc.boundsMin;
	sc.boundsMax = gc.boundsMax;
	sc.mesh = std::make_unique<gfx::Mesh>(m_device, gc.mesh);
	return sc;
}

void DungeonWorld::BuildDungeonMeshes() {
	DungeonGeometry geo = BuildDungeonGeometry(
		m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks,
		[this](int x, int z) {
			return CellHoles{FloorHoleAt(x, z), CeilingHoleAt(x, z)};
		},
		[this](const std::string& type) { return NicheMeshFor(type); },
		[this](const std::string& type) { return BoreMeshFor(type); });

	auto upload = [&](Surface& surface, std::vector<GeometryChunk>& chunks) {
		surface.chunks.clear();
		for (GeometryChunk& gc : chunks) surface.chunks.push_back(MakeSurfaceChunk(gc));
	};
	upload(m_walls, geo.walls);
	upload(m_floors, geo.floors);
	upload(m_ceilings, geo.ceilings);
	m_geometryDirty = false; // any full bake pays the deferred-undo debt
}

// Loads each monster model once (shared per kind) and creates one animator
// per spawn. The shared ModelData must stay alive for the animators' sake —
// it lives in m_monsterKinds for the app's lifetime.
DungeonWorld::MonsterKind& DungeonWorld::MonsterKindFor(const std::string& type) {
	auto it = m_monsterKinds.find(type);
	if (it == m_monsterKinds.end()) {
		// Resolve model + texture set through the monsters catalog; an unlisted
		// type falls back to the old name convention (<type>.gltf).
		const CatalogEntry* def = m_project.monsters.Find(type);
		const auto [model, tex] = ModelAndTexture(def, type);
		auto assets = std::make_unique<MonsterKind>();
		assets->model = LoadModelOrDie(model + ".gltf");
		assets->name = type; // catalog id — drives the monster.<id> loc key
		assets->mesh = std::make_unique<gfx::Mesh>(m_device, assets->model.meshes[0]);
		// A bound PBR set serves the single-mesh path; an authored
		// multi-material rig carries its textures EMBEDDED and its entry
		// usually names no set — don't warn-hunt one by the id (the skeleton
		// kit's four kinds fired a bogus missing-set warning each) unless the
		// catalog names one explicitly.
		const bool multi = assets->model.meshes.size() > 1;
		if (!multi || (def && def->Find("texture")))
			assets->tex = LoadPropTextures(tex); // <tex>_<res> PBR set, if present
		// Authored multi-material rig (bones/armor/weapons primitives, embedded
		// textures): build the per-material submeshes the draw paths loop.
		if (multi)
			assets->multi = BuildMultiMaterialModel(m_device, assets->model);
		// Map head-shot icon RT; a fresh kind re-arms the one-shot bake pass.
		assets->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
		m_monsterIconsBaked = false;
		// Combat stats (defaults keep an undescribed monster fightable).
		if (def) {
			assets->maxHp = def->GetFloat("hp", 12.0f);
			assets->damage = def->GetFloat("damage", 4.0f);
			assets->accuracy = def->GetFloat("accuracy", 0.65f);
			assets->evasion = def->GetFloat("defense", 0.1f);
			assets->armor = def->GetFloat("armor", 0.0f);
			// The defender side (docs/combat.md part 4): per-type resist
			// cells + what this monster's melee deals AS (default bash).
			ParseResists(CatalogGet(def, "resists", ""), assets->resists,
						 "monsters.cat [" + type + "]");
			if (const std::string t = CatalogGet(def, "dmgtype", "");
				!t.empty() && !ParseDamageType(t, assets->damageType))
				log::Warn("monsters.cat [{}]: unknown dmgtype '{}'", type, t);
			// On-hit DoTs (Phase 6): "<dps> <seconds> [chance]" per kind.
			const auto hitEffect = [&](const char* key,
									   MonsterKind::HitEffect& fx) {
				const std::vector<std::string> t =
					SplitTokens(CatalogGet(def, key, ""));
				if (t.empty()) return;
				if (t.size() < 2) {
					log::Warn("monsters.cat [{}]: {}= needs <dps> <seconds>",
							  type, key);
					return;
				}
				fx.dps = std::strtof(t[0].c_str(), nullptr);
				fx.duration = std::strtof(t[1].c_str(), nullptr);
				if (t.size() >= 3) fx.chance = std::strtof(t[2].c_str(), nullptr);
			};
			hitEffect("poison", assets->poison);
			hitEffect("bleed", assets->bleed);
			// Melee reach in cells (Phase 7): 2 = a pike melees from its
			// queue post down a clear shared row/column.
			assets->reach = std::max(
				1, static_cast<int>(def->GetFloat("reach", 1.0f) + 0.5f));
			assets->attackInterval = def->GetFloat("attackcd", 1.6f);
			assets->aggroRange = def->GetFloat("aggro", 6.0f);
			assets->moveInterval = def->GetFloat("movecd", 0.6f);
			assets->iq = def->GetFloat("iq", 100.0f);
			assets->archetype = ParseArchetype(CatalogGet(def, "archetype", "brute"));
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
					if (ModelHasClip(assets->model, c)) clips.push_back(c);
			} else if (const std::string dflt(anim::StateName(st));
					   ModelHasClip(assets->model, dflt)) {
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
	names.reserve(kind.model.clips.size());
	for (const auto& c : kind.model.clips) names.push_back(c.name);
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
			if (ModelHasClip(kind.model, name)) filtered.push_back(name);
		kind.animClips[i] = std::move(filtered);
	}
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
// <model>.gltf is missing shows a warning instead of aborting in LoadModelOrDie.
bool DungeonWorld::MonsterModelAvailable(const std::string& type) const {
	if (m_monsterKinds.contains(type)) return true; // already loaded => present
	const CatalogEntry* def = m_project.monsters.Find(type);
	const auto [model, tex] = ModelAndTexture(def, type);
	return std::filesystem::exists(paths::Asset("models\\" + model + ".gltf"));
}

DungeonWorld::MonsterPreviewData DungeonWorld::MonsterPreviewFor(const std::string& type) {
	const MonsterKind& kind = MonsterKindFor(type);
	MonsterPreviewData d;
	d.mesh = kind.mesh.get();
	d.skeleton = &kind.model.skeleton;
	d.clips = &kind.model.clips;
	d.modelScale = kind.modelScale;
	d.modelYaw = kind.modelYaw;
	ApplyPropMaterial(d.material, kind.tex, kind.model.materials[0].baseColorFactor,
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

std::vector<gfx::PreviewSubmesh> DungeonWorld::ItemPreviewSubs(int entityId, Vec3& fitMin,
															   Vec3& fitMax) const {
	std::vector<gfx::PreviewSubmesh> subs;
	// AABB of a ModelData mesh's vertices (for framing the tablet placeholder).
	auto meshBounds = [](const assets::ModelData& m, Vec3& mn, Vec3& mx) {
		mn = {1e9f, 1e9f, 1e9f};
		mx = {-1e9f, -1e9f, -1e9f};
		if (m.meshes.empty()) return;
		for (const auto& v : m.meshes[0].vertices) {
			mn = {std::min(mn.x, v.position.x), std::min(mn.y, v.position.y),
				  std::min(mn.z, v.position.z)};
			mx = {std::max(mx.x, v.position.x), std::max(mx.y, v.position.y),
				  std::max(mx.z, v.position.z)};
		}
	};
	for (const Item& item : m_items) {
		if (item.id != entityId || !item.kind) continue;
		if (item.kind->model) { // authored model item (weapon, ...)
			for (const auto& s : item.kind->model->subs) subs.push_back({s.mesh.get(), s.material});
			fitMin = item.kind->model->boundsMin;
			fitMax = item.kind->model->boundsMax;
		} else if (m_runeMesh) { // rune / placeholder: the shared carved tablet
			gfx::MaterialParams mat;
			const Vec4 base = m_runeModel.materials.empty()
								  ? Vec4{1, 1, 1, 1}
								  : m_runeModel.materials[0].baseColorFactor;
			ApplyPropMaterial(mat, item.kind->tex, base, 0.85f);
			subs.push_back({m_runeMesh.get(), mat});
			meshBounds(m_runeModel, fitMin, fitMax);
		}
		break;
	}
	return subs;
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
	monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
	// Initial resting pose; DriveMonsterAnim takes over next frame (and plays the
	// spawn clip first if the kind has one, via the default spawnReq).
	const std::string idle = PickClip(kind, anim::CreatureState::Idle);
	monster.animator.Play(idle.empty() ? "idle" : idle);
	return monster;
}

void DungeonWorld::LoadMonsters() {
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

DungeonWorld::ItemKind& DungeonWorld::ItemKindFor(const std::string& type) {
	auto it = m_itemKinds.find(type);
	if (it == m_itemKinds.end()) {
		auto kind = std::make_unique<ItemKind>();
		kind->id = type;
		const CatalogEntry* def = m_project.items.Find(type);
		// Display name: catalog `name` key, else item.<id> by convention.
		kind->nameKey = CatalogGet(def, "name", std::format("item.{}", type));
		// Shared, data-driven fields: category, carry weight, hand commands.
		kind->category = CatalogGet(def, "category", "misc");
		// Weapon class (docs/skills.md): the skill a swing with this item
		// trains and is scaled by. Absent = the swing trains nothing.
		kind->skill = CatalogGet(def, "skill", "");
		// The attack formula's fields (docs/combat.md): weapon damage/speed
		// (absent = 0 = the unarmed knobs), associated stats (`stats = str,
		// dex`; absent = the unarmed default), and the worn defender side
		// (per-type `resists` cells + a small flat `armor` soak).
		kind->damage = def ? def->GetFloat("damage", 0.0f) : 0.0f;
		kind->speed = def ? def->GetFloat("speed", 0.0f) : 0.0f;
		kind->stats = ParseStatList(CatalogGet(def, "stats", ""),
									"items.cat [" + type + "]");
		// Weapon reach (Phase 7): `reach = polearm` swings from the rear rank.
		kind->polearm = CatalogGet(def, "reach", "melee") == "polearm";
		ParseResists(CatalogGet(def, "resists", ""), kind->resists,
					 "items.cat [" + type + "]");
		kind->armor = def ? def->GetFloat("armor", 0.0f) : 0.0f;
		kind->weight = def ? def->GetFloat("weight", 0.0f) : 0.0f;
		// `command` is a free-form list (whitespace/comma separated) of command ids
		// the hand right-click menu offers; runes implicitly gain "memorize" below.
		for (const std::string& cmd : SplitTokens(CatalogGet(def, "command", "")))
			kind->commands.push_back(cmd);
		// Placeholder look: non-rune items reuse the tablet mesh tinted by category
		// (runes overwrite this with their element colour just below).
		kind->glow = CategoryTint(kind->category);
		// Authored model (catalog `model`): the item draws as this 3D model on the
		// floor and its baked render becomes the icon/cursor. null = the tablet+tint
		// placeholder. Items ship as embedded-texture multi-material .glb.
		if (const std::string modelName = CatalogGet(def, "model", ""); !modelName.empty()) {
			kind->model = BuildMultiMaterialModel(m_device, LoadModelOrDie(modelName + ".glb"));
			BakeCatalogMaterial(*kind->model, def); // dialog material overrides
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
				kind->tex = LoadPropTextures(RuneItemId(sym));
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
		if (kind->model) {
			kind->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
			kind->iconAnimated = CatalogBool(def, "icon_spin", false);
			m_itemIconsBaked = false; // a freshly added icon needs baking
		}
		// Uniform size trim over the authored unit size, like DecorationKind's.
		kind->modelScale = def ? def->GetFloat("scale", 1.0f) : 1.0f;
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
		const Vec3 c = m_map.CellCenter(spawn.x, spawn.z);
		const int slot = FreeItemSlotNear(spawn.x, spawn.z, c.x, c.z, -1);
		m_items.push_back({&kind, spawn.id, spawn.x, spawn.z, false, slot});
	}
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
		// The lever mesh, when the catalog knows the type (a legacy record with
		// an unknown type still works — it just has no 3D presence).
		if (m_project.buttons.Contains(spawn.type))
			b.kind = &DecorationKindFor(spawn.type, m_project.buttons);
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

std::optional<std::string> DungeonWorld::TryPickItem(float mx, float my, float w,
													 float h) {
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
		if (item.collected || !InReach(item.x, item.z, px, pz)) continue;
		if (!IsSeen(item.x, item.z)) continue;
		if (item.niche >= 0) {
			// Niche item: a small sphere at the pocket (the player looks roughly
			// level at the wall, so no floor-plane test), and only while open.
			const Direction wall = static_cast<Direction>(item.niche);
			if (!NicheOpenAt(item.x, item.z, wall)) continue;
			const Vec3 p = NicheItemPos(item.x, item.z, wall);
			const Vec3 oc{ray.origin.x - p.x, ray.origin.y - (p.y + 0.35f),
						  ray.origin.z - p.z};
			const float bb = oc.x * ray.dir.x + oc.y * ray.dir.y + oc.z * ray.dir.z;
			const float cc = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - 0.4f * 0.4f;
			const float disc = bb * bb - cc;
			if (disc >= 0.0f && -bb - std::sqrt(disc) > 0.0f) best = static_cast<int>(i);
			continue;
		}
		if (ray.dir.y >= -1e-4f) continue; // floor pick needs a look down at the floor
		// Plane at the item's visible mid-height — a model spans 0..Height; the
		// tablet placeholders sit low (rune slab shorter than the scaled-up others).
		const float centreY = item.kind->model
								  ? std::max(item.kind->model->Height(), 0.1f) * 0.5f
								  : (item.kind->isRune ? 0.23f : 0.45f);
		const float t = (centreY - ray.origin.y) / ray.dir.y;
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
	if (best < 0) return std::nullopt;
	Item& picked = m_items[static_cast<size_t>(best)];
	picked.collected = true; // off the floor
	m_audio.Play(m_sounds.click, 0.6f); // placeholder pickup cue
	if (onMessage) onMessage(loc::Format("log.take_rune", loc::Tr(picked.kind->nameKey)));
	return picked.kind->id;
}

void DungeonWorld::DropItemAt(const std::string& typeId, float mx, float my,
							  float w, float h) {
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
		const Vec3 p = NicheItemPos(n.x, n.z, n.wall);
		const Vec3 oc{ray.origin.x - p.x, ray.origin.y - (p.y + 0.35f), ray.origin.z - p.z};
		const float bb = oc.x * ray.dir.x + oc.y * ray.dir.y + oc.z * ray.dir.z;
		const float cc = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - 0.4f * 0.4f;
		const float disc = bb * bb - cc;
		if (disc < 0.0f) continue;
		const float t = -bb - std::sqrt(disc);
		if (t > 0.0f && t < bestT) { bestT = t; bestNiche = static_cast<int>(i); }
	}
	if (bestNiche >= 0) {
		const WallNiche& n = niches[static_cast<size_t>(bestNiche)];
		ItemKind& kind = ItemKindFor(typeId);
		m_items.push_back(
			{&kind, m_nextDropId--, n.x, n.z, false, 0, static_cast<int>(n.wall)});
		m_audio.Play(m_sounds.click, 0.5f);
		if (onMessage) onMessage(loc::Format("log.drop_rune", loc::Tr(kind.nameKey)));
		return;
	}
	int cx = px, cz = pz; // fallback: drop at the party's feet
	// Desired drop point in world space — used to pick the nearest quarter slot.
	// Defaults to the fallback cell's centre (feet); a floor hit overrides it.
	Vec3 feet = m_map.CellCenter(cx, cz);
	float wx = feet.x, wz = feet.z;
	if (ray.dir.y < -1e-3f) { // looking down toward the floor plane y=0
		const float t = -ray.origin.y / ray.dir.y;
		const float hxw = ray.origin.x + ray.dir.x * t;
		const float hzw = ray.origin.z + ray.dir.z * t;
		const int hx = static_cast<int>(std::floor(hxw / kCellSize));
		const int hz = static_cast<int>(std::floor(hzw / kCellSize));
		if (m_map.IsWalkable(hx, hz) && IsSeen(hx, hz) && InReach(hx, hz, px, pz)) {
			cx = hx;
			cz = hz;
			wx = hxw; // snap to the quarter under the cursor
			wz = hzw;
		}
	}
	ItemKind& kind = ItemKindFor(typeId);
	const int slot = FreeItemSlotNear(cx, cz, wx, wz, -1);
	m_items.push_back({&kind, m_nextDropId--, cx, cz, false, slot});
	m_audio.Play(m_sounds.click, 0.5f);
	if (onMessage) onMessage(loc::Format("log.drop_rune", loc::Tr(kind.nameKey)));
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
	return m_propTextures.emplace(set, std::move(pt)).first->second.get();
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
	const bool hasTint = ParseColorField(CatalogGet(def, "color", ""), tint);
	for (auto& sub : model.subs) {
		if (metallic >= 0.0f) sub.material.metallic = metallic;
		if (roughness >= 0.0f) sub.material.roughness = roughness;
		if (hasTint) sub.material.baseColor = tint;
	}
}


// Loads each decoration model once (shared per type, like monsters) and bakes
// one placed instance per .map "decoration" record. Authored facing +Z, so a
// record's facing rotates the prop the same way a monster's does. Everything
// is solid (blocks the party) except open passages like the archway; a
// "solid=0"/"solid=1" param on the record overrides the default.
// Each decoration type resolves through the decorations catalog: its model
// (assets/models/<model>.gltf), its texture set (procedural props share a
// dungeon-stone/wood-plank set, authored imports carry their own), whether it is
// back-face culled (authored), and whether a floor-standing instance blocks the
// party (passages like the archway don't). An unlisted type falls back to the
// old convention: same-named model + set, authored, solid.
// Builds an authored model's own GPU resources: one texture per embedded glTF
// image (base-color maps sRGB, normal/MR linear) and one submesh per primitive,
// each with a MaterialParams resolved from its glTF material. Lets a bought
// multi-material model render every part with its real material.
std::unique_ptr<DungeonWorld::MultiMaterialModel> DungeonWorld::BuildMultiMaterialModel(
	gfx::GraphicsDevice& device, const assets::ModelData& model) {
	auto out = std::make_unique<DungeonWorld::MultiMaterialModel>();
	std::vector<bool> srgb(model.images.size(), false);
	for (const assets::MaterialData& m : model.materials)
		if (m.baseColorImage >= 0) srgb[m.baseColorImage] = true;
	out->textures.reserve(model.images.size());
	for (size_t i = 0; i < model.images.size(); ++i)
		out->textures.push_back(
			std::make_unique<gfx::Texture>(device, model.images[i], srgb[i]));

	auto texAt = [&](int img) -> const gfx::Texture* {
		return img >= 0 ? out->textures[static_cast<size_t>(img)].get() : nullptr;
	};
	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const assets::MeshData& mesh : model.meshes) {
		DungeonWorld::MultiMaterialModel::Sub sub;
		// Bake the glTF node transform into the vertices (it carries ConvertMesh's
		// normalization scale/placement). The loader stores it but leaves the
		// vertices in local space — import-model does this same bake when merging;
		// here we keep the submeshes separate, so each bakes its own node.
		assets::MeshData baked = mesh;
		const XMMATRIX node = XMLoadFloat4x4(&mesh.worldTransform);
		for (assets::Vertex& v : baked.vertices) {
			XMFLOAT3 pf, nf;
			XMStoreFloat3(&pf, XMVector3Transform(
								   XMVectorSet(v.position.x, v.position.y, v.position.z, 1.0f),
								   node));
			XMStoreFloat3(&nf, XMVector3Normalize(XMVector3TransformNormal(
								   XMVectorSet(v.normal.x, v.normal.y, v.normal.z, 0.0f),
								   node)));
			v.position = {pf.x, pf.y, pf.z};
			v.normal = {nf.x, nf.y, nf.z};
			lo = {std::min(lo.x, pf.x), std::min(lo.y, pf.y), std::min(lo.z, pf.z)};
			hi = {std::max(hi.x, pf.x), std::max(hi.y, pf.y), std::max(hi.z, pf.z)};
		}
		sub.mesh = std::make_unique<gfx::Mesh>(device, baked);
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
		}
		out->subs.push_back(std::move(sub));
	}
	out->boundsMin = lo;
	out->boundsMax = hi;
	return out;
}

DungeonWorld::DecorationKind& DungeonWorld::DecorationKindFor(const std::string& type,
															 const Catalog& catalog) {
	auto it = m_decorationKinds.find(type);
	if (it == m_decorationKinds.end()) {
		const CatalogEntry* def = catalog.Find(type);
		const auto [model, tex] = ModelAndTexture(def, type);
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
			kind->hasTint = ParseColorField(CatalogGet(def, "color", ""), kind->tint);
			// Uniform size trim on top of the authored unit size (monsters' long-
			// standing `modelscale`, now available to every prop): 1 = as authored.
			kind->modelScale = def->GetFloat("scale", 1.0f);
		}
		// Every kind bakes a whole-model map icon; a fresh kind re-arms the
		// one-shot bake pass (UpdateMapIcons).
		kind->iconTarget = gfx::Texture::RenderTarget(m_device, kIconSize);
		m_decorationIconsBaked = false;
		// Authored multi-material models (bought weapon/prop packs) render their
		// own glTF textures per material from a single embedded-texture .glb,
		// bypassing the single-mesh / one-bound-set path below.
		if (CatalogBool(def, "multimaterial", false)) {
			kind->model = LoadModelOrDie(model + ".glb");
			kind->multi = BuildMultiMaterialModel(m_device, kind->model);
			BakeCatalogMaterial(*kind->multi, def); // overrides baked per submesh
			kind->solidDefault = CatalogBool(def, "solid", true);
			it = m_decorationKinds.emplace(type, std::move(kind)).first;
			return *it->second;
		}
		kind->model = LoadModelOrDie(model + ".gltf");
		kind->mesh = std::make_unique<gfx::Mesh>(m_device, kind->model.meshes[0]);
		kind->color = kind->model.materials[0].baseColorFactor;
		kind->tex = LoadPropTextures(tex);
		kind->solidDefault = CatalogBool(def, "solid", true);
		// Optional alpha-test cutout (a masked set like wood planks renders its
		// gaps); absent/0 = opaque, the usual case.
		kind->alphaCutoff = def ? def->GetFloat("alpha_test", 0.0f) : 0.0f;
		it = m_decorationKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

// Fixture counterpart of DecorationKindFor: resolves a fixtures.cat id into
// its renderable assets once and caches it. An unknown id still resolves (the
// ModelAndTexture fallback names the id itself) so a stale record aborts with
// a clear missing-model message instead of silently vanishing.
DungeonWorld::FixtureKind& DungeonWorld::FixtureKindFor(const std::string& type) {
	auto it = m_fixtureKinds.find(type);
	if (it == m_fixtureKinds.end()) {
		const CatalogEntry* def = m_project.fixtures.Find(type);
		if (!def) log::Warn("fixture kind '{}' is not in fixtures.cat", type);
		const auto [model, set] = ModelAndTexture(def, type);
		auto kind = std::make_unique<FixtureKind>();
		kind->id = type;
		kind->wallMount = CatalogGet(def, "mount", "floor") == "wall";
		kind->flameless = !CatalogBool(def, "flame", true);
		kind->model = LoadModelOrDie(model + ".gltf");
		kind->mesh = std::make_unique<gfx::Mesh>(m_device, kind->model.meshes[0]);
		kind->color = kind->model.materials[0].baseColorFactor;
		kind->tex = LoadPropTextures(set);
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
				const assets::ModelData data = LoadModelOrDie(model2 + ".gltf");
				kind->mesh2 = std::make_unique<gfx::Mesh>(m_device, data.meshes[0]);
				kind->color2 = data.materials[0].baseColorFactor;
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
}

void DungeonWorld::PlaceStairProp(const StairLink& s) {
	DecorationKind& kind = DecorationKindFor(s.type, m_project.stairs);
	Decoration deco;
	deco.kind = &kind;
	deco.x = s.x;
	deco.z = s.z;
	deco.facing = s.facing;
	deco.stair = true; // written as a stairs record, not a decoration
	const Vec3 pos = m_map.CellCenter(s.x, s.z);
	XMStoreFloat4x4(&deco.world, UnitScale(kind.modelScale) * XMMatrixRotationY(DirYaw(s.facing)) *
									 XMMatrixTranslation(pos.x, 0, pos.z));
	deco.solid = false; // the party walks onto a stair to use it
	m_decorations.push_back(std::move(deco));
}

// Places one Fire per sconce ('T') and brazier ('F') cell. Sconces mount on
// the first solid neighbor wall and face into the room; braziers stand at
// the cell center. Flame origins match the baked models (see ModelBaker).
// Origin pushed to the wall face, +Z (authored front) turned to face the room.
DungeonWorld::WallMount DungeonWorld::MountOnWall(int x, int z, Direction wall) const {
	const int dx = DirDX(wall), dz = DirDZ(wall);
	const Vec3 c = m_map.CellCenter(x, z);
	return {{c.x + dx * (kCellSize * 0.5f - 0.02f), 0.0f,
			 c.z + dz * (kCellSize * 0.5f - 0.02f)},
			std::atan2(static_cast<float>(-dx), static_cast<float>(-dz))};
}

void DungeonWorld::BuildFires() {
	u32 seed = 1234;

	for (const WallSconce& sconce : m_map.Sconces()) {
		const FixtureKind& kind = FixtureKindFor(sconce.type);
		// Hang on the wall resolved at map load (shared with decorations).
		const WallMount m = MountOnWall(sconce.x, sconce.z, sconce.wall);
		const float yaw = m.yaw;

		Fire fire;
		fire.kind = &kind;
		fire.brazier = false;
		fire.lit = sconce.lit && !kind.flameless;
		fire.lightRadius = sconce.brightness * kCellSize; // "squares" -> metres
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
		m_fires.push_back(std::move(fire));
	}

	for (const FloorBrazier& b : m_map.Braziers()) {
		const FixtureKind& kind = FixtureKindFor(b.type);
		const Vec3 center = m_map.CellCenter(b.x, b.z);
		Fire fire;
		fire.kind = &kind;
		fire.brazier = true;
		fire.lit = b.lit && !kind.flameless;
		fire.lightRadius = b.brightness * kCellSize; // "squares" -> metres
		const float fs = kind.modelScale; // fixtures.cat `scale`
		XMStoreFloat4x4(&fire.world,
						UnitScale(fs) * XMMatrixTranslation(center.x, 0, center.z));
		fire.flamePos = {center.x, kind.flame.height * kUnit * fs, center.z};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, kind.flame.scale * fs, seed++);
		m_fires.push_back(std::move(fire));
	}
	log::Info("Lit {} fires ({} sconces, {} braziers, {} kinds)", m_fires.size(),
			  m_map.Sconces().size(), m_map.Braziers().size(),
			  m_fixtureKinds.size());
}

// Per-cell turbidity as a top-down density grid: one texel per dungeon cell,
// R channel; bilinear filtering blends region borders. The scene shader
// raymarches it (see scene.hlsl).
void DungeonWorld::BuildTurbidityMap() {
	assets::ImageData grid;
	grid.width = static_cast<u32>(m_map.Width());
	grid.height = static_cast<u32>(m_map.Height());
	grid.pixels.resize(static_cast<size_t>(grid.width) * grid.height * 4);
	for (int z = 0; z < m_map.Height(); ++z) {
		for (int x = 0; x < m_map.Width(); ++x) {
			const size_t i = (static_cast<size_t>(z) * grid.width + x) * 4;
			grid.pixels[i + 0] = static_cast<u8>(m_map.Turbidity(x, z) * 255.0f);
			grid.pixels[i + 3] = 255;
		}
	}
	m_turbidityMap = std::make_unique<gfx::Texture>(m_device, grid);
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
// meshes in place (monsters and fires are unaffected).
// ============================================================================
void DungeonWorld::ApplyQuality(bool textureResChanged) {
	if (m_walls.chunks.empty()) return; // not built yet — the load tasks will
	ReloadDungeonBlocks(textureResChanged);
	log::Info("Quality switched to {} ({} meshes, {} textures)",
			  m_settings.QualityLabel(), m_settings.MeshSuffix(),
			  m_settings.TextureSuffix());
}

// Reloads the worn block meshes and rebuilds the batched dungeon geometry in
// place — shared by the quality hot-swap and the editor's Wall Style rebake
// (which re-bakes a texture's worn_*.gltf then calls this to swap it in live).
// The map Revision is unchanged, so the cached shadow cubes are force-refreshed.
void DungeonWorld::ReloadDungeonBlocks(bool textureResChanged) {
	if (m_walls.chunks.empty()) return; // not built yet — the load tasks will

	// The GPU may still be reading the old resources, so drain it first.
	m_device.WaitIdle();
	// Re-resolve palettes so an edited `wear` updates BOTH the worn mesh (via
	// LoadDungeonBlocks below) and the parallax depth together — the two must
	// move in lockstep or a flat mesh still shows faked relief.
	ResolveSurfacePalettes();
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	LoadDungeonBlocks();
	if (textureResChanged)
		LoadAllSurfaceTextures(); // re-pushes each variant's parallax depth
	else                          // textures unchanged — just refresh the depths
		for (const SurfaceDef& def : SurfaceDefs())
			def.surface.heightScale.assign(def.heights.begin(), def.heights.end());
	BuildDungeonMeshes();
	m_shadows.InvalidateCubes();
}
} // namespace dungeon::game
