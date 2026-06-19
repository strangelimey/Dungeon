// ============================================================================
// Game/DungeonWorld.cpp — see DungeonWorld.h.
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
#include <cmath>
#include <format>
#include <queue>

using namespace DirectX;

namespace dungeon::game {

// ============================================================================
// Construction — cheap setup only (the map files parse fast); the heavy asset
// work is queued by AppendLoadTasks and runs behind the loading screen.
// ============================================================================
// The project's first level stem (the one the game opens), falling back to
// "level1" for a project whose manifest names no levels.
static std::string FirstLevel(const Project& p) {
	return p.levels.empty() ? std::string("level1") : p.levels.front();
}

// A catalog entry's model + texture asset names, both defaulting to `fallback`
// (usually the id) when the entry or field is absent. Shared by the monster,
// decoration, and fixture loaders.
static std::pair<std::string, std::string> ModelAndTexture(const CatalogEntry* e,
														   const std::string& fallback) {
	return {CatalogGet(e, "model", fallback), CatalogGet(e, "texture", fallback)};
}

DungeonWorld::DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
						   audio::AudioEngine& audio, const SoundBank& sounds,
						   const GameSettings& settings, const Project& project)
	: m_device(device), m_renderer(renderer), m_audio(audio), m_sounds(sounds),
	  m_settings(settings), m_project(project),
	  m_map(project.LevelMapPath(FirstLevel(project))),
	  m_entities(project.LevelEntPath(FirstLevel(project)), m_map),
	  m_party(m_map, m_map.StartX(), m_map.StartZ()) {
	m_currentLevel = FirstLevel(project);
	// Resolve the level's palette ids → texture set names + height scales before
	// any load task runs (SurfaceDefs and LoadDungeonBlocks read the results).
	ResolveSurfacePalettes();
	// Party event hooks (survive Party::Reset). Feedback goes out through the
	// shared sound bank and the onMessage log line.
	m_party.onStep = [this] {
		m_audio.Play(m_sounds.footstep, 0.8f);
		const int px = m_party.GridX(), pz = m_party.GridZ();
		MarkSeen(px, pz);
		// Stepping onto a stair raises a pending transition; Game polls it after
		// Update and drives the swap, so the level never changes mid-step.
		for (const StairLink& s : m_map.Stairs())
			if (s.x == px && s.z == pz) {
				m_pendingTransition =
					LevelTransition{s.destLevel, s.destX, s.destZ, s.destFacing};
				break;
			}
	};
	m_party.onBlocked = [this] {
		m_audio.Play(m_sounds.bump, 0.9f);
		onMessage(loc::Tr("log.bump"));
	};
	m_party.onTurn = [this] { m_audio.Play(m_sounds.turn, 0.6f); };
	m_party.onBumpImpact = [this] { OnBumpImpact(); };
	m_party.isOccupied = [this](int x, int z) {
		for (const Monster& monster : m_monsters) {
			if (monster.Alive() && monster.x == x && monster.z == z) {
				m_audio.Play(m_sounds.monster, 0.8f);
				onMessage(loc::Format("log.monster_blocks",
									  loc::Tr("monster." + monster.kind->name)));
				return true;
			}
		}
		for (const auto& [bx, bz] : m_map.BrazierCells()) {
			if (bx == x && bz == z) {
				m_audio.Play(m_sounds.bump, 0.7f);
				onMessage(loc::Tr("log.brazier_blocks"));
				return true;
			}
		}
		for (const Decoration& deco : m_decorations) {
			if (deco.solid && deco.x == x && deco.z == z) {
				m_audio.Play(m_sounds.bump, 0.7f);
				onMessage(loc::Tr("log.decoration_blocks"));
				return true;
			}
		}
		return false;
	};

	// Fog of war: nothing revealed until the party stands somewhere. Seed the
	// start cell so the map isn't blank the moment it opens.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	MarkSeen(m_party.GridX(), m_party.GridZ());

	// Lifted from {0.035,0.032,0.045} after the albedo sRGB switch: textures now
	// decode darker, so the unlit fill needs more to keep corridors from reading
	// pitch-black. Cool tint preserved (no sun underground).
	m_lights.ambient = {0.052f, 0.048f, 0.064f};
	m_lights.directional.color = {0, 0, 0}; // no sun underground
	// Rebuilt every frame into retained capacity — no steady-state allocation.
	m_lights.points.reserve(gfx::kMaxPointLights);
	m_shadowCandidates.reserve(gfx::kMaxPointLights);
}

// ============================================================================
// Staged loading — one queued task per frame (see LoadQueue).
// ============================================================================

// The three surface texture sets and the height scales their parallax uses —
// the single source of those constants, shared by the staged loader and the
// quality hot-swap (LoadAllSurfaceTextures).
std::array<DungeonWorld::SurfaceDef, 3> DungeonWorld::SurfaceDefs() {
	return {{{m_walls, m_wallSets, m_wallHeight},
			 {m_floors, m_floorSets, m_floorHeight},
			 {m_ceilings, m_ceilingSets, m_ceilingHeight}}};
}

// Resolves each surface palette id through its project catalog into a texture
// set name (DungeonWorld loads <set>_<res> and worn_<set>_<tier>.gltf) and the
// surface's parallax height scale (taken from the first entry — a surface's
// entries share a scale). An unknown id falls back to using the id verbatim as
// the set name, so a hand-edited level still loads something.
void DungeonWorld::ResolveSurfacePalettes() {
	struct Def {
		const std::vector<std::string>& palette;
		const Catalog& catalog;
		std::vector<std::string>& sets;
		float& height;
		float fallbackHeight;
	};
	const Def defs[] = {
		{m_map.WallPalette(), m_project.walls, m_wallSets, m_wallHeight, 0.055f},
		{m_map.FloorPalette(), m_project.floors, m_floorSets, m_floorHeight, 0.045f},
		{m_map.CeilingPalette(), m_project.ceilings, m_ceilingSets, m_ceilingHeight,
		 0.035f},
	};
	for (const Def& d : defs) {
		d.sets.clear();
		bool first = true;
		for (const std::string& id : d.palette) {
			const CatalogEntry* e = d.catalog.Find(id);
			d.sets.push_back(CatalogGet(e, "texture", id));
			if (first) {
				d.height = e ? e->GetFloat("height_scale", d.fallbackHeight)
							 : d.fallbackHeight;
				first = false;
			}
		}
	}
}

void DungeonWorld::AppendLoadTasks(LoadQueue& queue) {
	queue.Add(loc::Tr("load.blocks"), [this] { LoadDungeonBlocks(); });

	// One task per material (the scanned sets dominate the load); the first
	// material of each set resets the surface, exactly as LoadTextureSet does.
	for (const SurfaceDef& def : SurfaceDefs()) {
		Surface& surface = def.surface;
		const float heightScale = def.heightScale;
		for (size_t i = 0; i < def.names.size(); ++i) {
			const std::string& name = def.names[i];
			const bool first = i == 0; // first material resets the set
			std::string spaced = name; // asset id, shown with the '_'s opened up
			std::ranges::replace(spaced, '_', ' ');
			queue.Add(loc::Format("load.surface", spaced),
					  [this, &surface, name, heightScale, first] {
						  if (first) surface.ResetTextures(heightScale);
						  LoadSurfaceMaterial(surface, name);
					  });
		}
	}

	queue.Add(loc::Tr("load.dungeon"), [this] { BuildDungeonMeshes(); });
	queue.Add(loc::Tr("load.pillar"), [this] {
		m_pillarModel = LoadModelOrDie("pillar.gltf");
		m_pillarMesh = std::make_unique<gfx::Mesh>(m_device, m_pillarModel.meshes[0]);
		m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
		m_pillarAnimator.Play("sway");
		m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);
		m_pillarTex = LoadPropTextures("pillar"); // peacock-ore
		// Flavor for the opening level only — don't spawn it on every level.
		m_pillarActive = m_currentLevel == FirstLevel(m_project);
	});
	queue.Add(loc::Tr("load.monsters"), [this] {
		LoadMonsters();
		LoadItems();
		LoadButtons();
	});
	queue.Add(loc::Tr("load.decorations"), [this] {
		LoadDecorations();
		LoadStairs();
	});
	queue.Add(loc::Tr("load.fires"), [this] {
		// Resolve the sconce/brazier model + texture through the fixtures catalog
		// (the ids the 'T'/'F' glyphs map to); fall back to the old names.
		auto fixtureAssets = [this](const std::string& id, const char* fallback,
									std::unique_ptr<gfx::Mesh>& mesh, Vec4& color,
									const PropTextures*& tex) {
			const CatalogEntry* def = m_project.fixtures.Find(id);
			const auto [model, set] = ModelAndTexture(def, fallback);
			auto data = LoadModelOrDie(model + ".gltf");
			mesh = std::make_unique<gfx::Mesh>(m_device, data.meshes[0]);
			color = data.materials[0].baseColorFactor;
			tex = LoadPropTextures(set);
		};
		fixtureAssets(m_project.defaultSconce, "sconce", m_sconceMesh, m_sconceColor,
					  m_sconceTex);   // worn-medieval iron
		fixtureAssets(m_project.defaultBrazier, "brazier", m_brazierMesh,
					  m_brazierColor, m_brazierTex); // bronze
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

// Loads one material's PBR set and appends it to the surface's variant arrays.
void DungeonWorld::LoadSurfaceMaterial(Surface& surface, const std::string& name) {
	PbrMaps maps = LoadPbrSet(name, /*required*/ true);
	surface.albedo.push_back(std::move(maps.albedo));
	surface.normal.push_back(std::move(maps.normal));
	surface.mr.push_back(std::move(maps.mr));
}

void DungeonWorld::LoadTextureSet(const SurfaceDef& def) {
	def.surface.ResetTextures(def.heightScale); // hot-swap reuses the same Surface
	for (const std::string& name : def.names)
		LoadSurfaceMaterial(def.surface, name);
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
	DungeonGeometry geo =
		BuildDungeonGeometry(m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks);

	auto upload = [&](Surface& surface, std::vector<GeometryChunk>& chunks) {
		surface.chunks.clear();
		for (GeometryChunk& gc : chunks) surface.chunks.push_back(MakeSurfaceChunk(gc));
	};
	upload(m_walls, geo.walls);
	upload(m_floors, geo.floors);
	upload(m_ceilings, geo.ceilings);
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
		assets->tex = LoadPropTextures(tex); // <tex>_<res> PBR set, if present
		// Combat stats (defaults keep an undescribed monster fightable).
		if (def) {
			assets->maxHp = def->GetFloat("hp", 12.0f);
			assets->damage = def->GetFloat("damage", 4.0f);
			assets->accuracy = def->GetFloat("accuracy", 0.65f);
			assets->evasion = def->GetFloat("defense", 0.1f);
			assets->armor = def->GetFloat("armor", 0.0f);
			assets->attackInterval = def->GetFloat("attackcd", 1.6f);
			assets->aggroRange = def->GetFloat("aggro", 6.0f);
			assets->moveInterval = def->GetFloat("movecd", 0.6f);
			assets->facesTarget = def->GetBool("faces", true);
			assets->fallbackRoughness = def->GetFloat("roughness", 0.9f);
		}
		it = m_monsterKinds.emplace(type, std::move(assets)).first;
	}
	return *it->second;
}

DungeonWorld::Monster DungeonWorld::MakeMonster(MonsterKind& kind, int id, int x,
												int z, Direction facing) {
	Monster monster;
	monster.kind = &kind;
	monster.id = id;
	monster.x = monster.spawnX = x;
	monster.z = monster.spawnZ = z;
	monster.yaw = DirYaw(facing);
	monster.facing = facing;
	monster.hp = kind.maxHp;
	monster.visualPos = m_map.CellCenter(x, z);
	monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
	monster.animator.Play("idle");
	return monster;
}

void DungeonWorld::LoadMonsters() {
	int phase = 0;
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Monster) continue;
		MonsterKind& kind = MonsterKindFor(spawn.type);
		Monster monster = MakeMonster(kind, spawn.id, spawn.x, spawn.z, spawn.facing);
		monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
		m_monsters.push_back(std::move(monster));
	}
}

// Element glow colour per school, matching docs/magic system.md (Earth=brown,
// Air=white, Fire=red, Water=blue). The whole rune tablet pulses in this colour
// via an additive emissive term (see SubmitSceneGeometry).
Vec4 DungeonWorld::RuneGlow(SpellSymbol s) {
	switch (s) {
	case SpellSymbol::Fire:  return {1.00f, 0.13f, 0.08f, 0.0f}; // red
	case SpellSymbol::Earth: return {0.60f, 0.36f, 0.16f, 0.0f}; // brown
	case SpellSymbol::Air:   return {1.00f, 1.00f, 1.00f, 0.0f}; // white
	case SpellSymbol::Water: return {0.18f, 0.42f, 1.00f, 0.0f}; // blue
	default:                 return {1.0f, 1.0f, 1.0f, 0.0f};
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
		// MVP: the only item behaviour is "rune" — category=rune, symbol=<sym>.
		if (CatalogGet(def, "category", "") == "rune") {
			SpellSymbol sym;
			if (ParseSymbol(CatalogGet(def, "symbol", "fire"), sym)) {
				kind->isRune = true;
				kind->runeSymbol = sym;
				kind->glow = RuneGlow(sym);
				// Shared tablet mesh (loaded once) + this element's carved set.
				if (!m_runeMesh) {
					m_runeModel = LoadModelOrDie("rune_tablet.gltf");
					m_runeMesh = std::make_unique<gfx::Mesh>(m_device,
															 m_runeModel.meshes[0]);
				}
				kind->tex = LoadPropTextures(std::format("rune_{}", SymbolId(sym)));
			} else {
				log::Warn("item {}: unknown rune symbol '{}'", type,
						  CatalogGet(def, "symbol", ""));
			}
		}
		it = m_itemKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

void DungeonWorld::LoadItems() {
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Item) continue;
		ItemKind& kind = ItemKindFor(spawn.type);
		m_items.push_back({&kind, spawn.id, spawn.x, spawn.z, false});
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
		m_buttons.push_back(std::move(b));
	}
}

// True if (x,z) is the party cell or orthogonally adjacent — arm's reach for
// picking up or dropping a tablet.
static bool InReach(int x, int z, int px, int pz) {
	return std::abs(x - px) + std::abs(z - pz) <= 1;
}

std::optional<std::string> DungeonWorld::TryPickItem(float mx, float my, float w,
													 float h) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const Mat4 vp = m_camera.ViewProj();
	// Projects a world point to screen pixels; returns false if behind the eye.
	auto project = [&](const Vec3& p, float& sx, float& sy) {
		using namespace DirectX;
		XMVECTOR clip = XMVector3Transform(XMVectorSet(p.x, p.y, p.z, 1.0f),
										   XMLoadFloat4x4(&vp));
		const float cw = XMVectorGetW(clip);
		if (cw <= 1e-4f) return false;
		sx = (XMVectorGetX(clip) / cw * 0.5f + 0.5f) * w;
		sy = (1.0f - (XMVectorGetY(clip) / cw * 0.5f + 0.5f)) * h;
		return true;
	};

	int best = -1;
	float bestD = 1e9f;
	for (size_t i = 0; i < m_items.size(); ++i) {
		const Item& item = m_items[i];
		if (item.collected || !InReach(item.x, item.z, px, pz)) continue;
		if (!IsSeen(item.x, item.z)) continue;
		const Vec3 c = m_map.CellCenter(item.x, item.z);
		float cx, cy, tx, ty;
		if (!project({c.x, 0.23f, c.z}, cx, cy)) continue; // tablet centre
		project({c.x, 0.46f, c.z}, tx, ty);                // top, for hit radius
		const float radius = std::clamp(std::fabs(ty - cy) * 1.3f, 26.0f, 220.0f);
		const float d = std::hypot(mx - cx, my - cy);
		if (d <= radius && d < bestD) {
			bestD = d;
			best = static_cast<int>(i);
		}
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
	int cx = px, cz = pz; // fallback: drop at the party's feet
	const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
	if (ray.dir.y < -1e-3f) { // looking down toward the floor plane y=0
		const float t = -ray.origin.y / ray.dir.y;
		const float wx = ray.origin.x + ray.dir.x * t;
		const float wz = ray.origin.z + ray.dir.z * t;
		const int hx = static_cast<int>(std::floor(wx / kCellSize));
		const int hz = static_cast<int>(std::floor(wz / kCellSize));
		if (m_map.IsWalkable(hx, hz) && IsSeen(hx, hz) && InReach(hx, hz, px, pz)) {
			cx = hx;
			cz = hz;
		}
	}
	ItemKind& kind = ItemKindFor(typeId);
	m_items.push_back({&kind, m_nextDropId--, cx, cz, false});
	m_audio.Play(m_sounds.click, 0.5f);
	if (onMessage) onMessage(loc::Format("log.drop_rune", loc::Tr(kind.nameKey)));
}

// Loads a prop PBR set once and caches it (shared across decorations, fires,
// the pillar, and monsters): sRGB albedo + linear normal/height + ORM, with the
// same res→2k fallback the surfaces use (props ship at 2k, so higher tiers fall
// back). Returns null only if even the 2k albedo is absent — callers then keep
// their flat glTF material color.
const DungeonWorld::PropTextures* DungeonWorld::LoadPropTextures(const std::string& set) {
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
DungeonWorld::DecorationKind& DungeonWorld::DecorationKindFor(const std::string& type,
															 const Catalog& catalog) {
	auto it = m_decorationKinds.find(type);
	if (it == m_decorationKinds.end()) {
		const CatalogEntry* def = catalog.Find(type);
		const auto [model, tex] = ModelAndTexture(def, type);
		auto kind = std::make_unique<DecorationKind>();
		kind->model = LoadModelOrDie(model + ".gltf");
		kind->mesh = std::make_unique<gfx::Mesh>(m_device, kind->model.meshes[0]);
		kind->color = kind->model.materials[0].baseColorFactor;
		kind->tex = LoadPropTextures(tex);
		kind->id = type; // the record type, for the .map writer
		kind->authored = CatalogBool(def, "authored", true);
		kind->solidDefault = CatalogBool(def, "solid", true);
		it = m_decorationKinds.emplace(type, std::move(kind)).first;
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
			XMStoreFloat4x4(&deco.world, XMMatrixRotationY(m.yaw) *
											 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
			deco.solid = false;
		} else {
			const Vec3 pos = m_map.CellCenter(deco.x, deco.z);
			XMStoreFloat4x4(&deco.world, XMMatrixRotationY(DirYaw(record.facing)) *
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
	for (const StairLink& s : m_map.Stairs()) {
		DecorationKind& kind = DecorationKindFor(s.type, m_project.stairs);
		Decoration deco;
		deco.kind = &kind;
		deco.x = s.x;
		deco.z = s.z;
		deco.facing = s.facing;
		deco.stair = true; // written as a stairs record, not a decoration
		const Vec3 pos = m_map.CellCenter(s.x, s.z);
		XMStoreFloat4x4(&deco.world, XMMatrixRotationY(DirYaw(s.facing)) *
										 XMMatrixTranslation(pos.x, 0, pos.z));
		deco.solid = false; // the party walks onto a stair to use it
		m_decorations.push_back(std::move(deco));
	}
	if (!m_map.Stairs().empty())
		log::Info("Placed {} stairs", m_map.Stairs().size());
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
		// Hang on the wall resolved at map load (shared with decorations).
		const WallMount m = MountOnWall(sconce.x, sconce.z, sconce.wall);
		const float yaw = m.yaw;

		Fire fire;
		fire.brazier = false;
		XMStoreFloat4x4(&fire.world, XMMatrixRotationY(yaw) *
										 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
		// Flame local offset (0, 1.78, 0.22) rotated by yaw.
		fire.flamePos = {m.pos.x + std::sin(yaw) * 0.22f, 1.78f,
						 m.pos.z + std::cos(yaw) * 0.22f};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, 0.55f, seed++);
		m_fires.push_back(std::move(fire));
	}

	for (const auto& [bx, bz] : m_map.BrazierCells()) {
		const Vec3 center = m_map.CellCenter(bx, bz);
		Fire fire;
		fire.brazier = true;
		XMStoreFloat4x4(&fire.world, XMMatrixTranslation(center.x, 0, center.z));
		fire.flamePos = {center.x, 0.72f, center.z};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, 1.0f, seed++);
		m_fires.push_back(std::move(fire));
	}
	log::Info("Lit {} fires ({} sconces, {} braziers)", m_fires.size(),
			  m_map.Sconces().size(), m_map.BrazierCells().size());
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
}

// ============================================================================
// Quality hot-swap (see GameSettings's Quality) — the worn blocks exist at
// three baked tessellation levels and the scanned textures at three
// resolutions; switching reloads both and rebuilds the batched dungeon
// meshes in place (monsters and fires are unaffected).
// ============================================================================
void DungeonWorld::ApplyQuality(bool textureResChanged) {
	if (m_walls.chunks.empty()) return; // not built yet — the load tasks will

	// The GPU may still be reading the old resources, so drain it first.
	m_device.WaitIdle();
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	LoadDungeonBlocks();
	if (textureResChanged) LoadAllSurfaceTextures();
	BuildDungeonMeshes();
	// The surface tessellation changed but the map Revision did not, so force a
	// re-render of every cached shadow cube (otherwise standing still after a
	// quality swap leaves the torch/glow cubes showing the old geometry).
	for (ShadowSlotCache& cache : m_shadowCache) cache.lightId = -1;
	log::Info("Quality switched to {} ({} meshes, {} textures)",
			  m_settings.QualityLabel(), m_settings.MeshSuffix(),
			  m_settings.TextureSuffix());
}

// ============================================================================
// Per-frame simulation
// ============================================================================

void DungeonWorld::ResetForNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	for (Monster& monster : m_monsters) {
		monster.announced = false;
		monster.hp = monster.MaxHp();
		monster.attackCd = 0.0f;
		// Monsters roam now (AI v1) — return them to their .ent spawn cell and
		// clear any in-flight glide so a same-level new game starts clean.
		monster.x = monster.spawnX;
		monster.z = monster.spawnZ;
		monster.moving = false;
		monster.moveT = 0.0f;
		monster.moveCd = 0.0f;
		monster.visualPos = m_map.CellCenter(monster.x, monster.z);
	}
	m_partyWiped = false;
	// Rebuild items from the .ent baseline so runes return to their spawn cells
	// (and any dropped tablets from a prior session are forgotten).
	m_items.clear();
	LoadItems();
	for (Button& b : m_buttons) b.activated = false; // un-press for a fresh run
	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	MarkSeen(m_party.GridX(), m_party.GridZ());
	SetTorchPalette(0);
	m_levelStates.clear(); // forget any explored levels
}

SaveData::LevelState DungeonWorld::SnapshotActive() const {
	SaveData::LevelState ls;
	ls.stem = m_currentLevel;
	for (int z = 0; z < m_map.Height(); ++z)
		for (int x = 0; x < m_map.Width(); ++x)
			if (m_seen[static_cast<size_t>(z) * m_map.Width() + x])
				ls.seen.emplace_back(x, z);
	// Every dynamic entity round-trips through one generic EntityState, as either
	// a DIFF (a .ent baseline that drifted from its spawn, keyed by id) or a SPAWN
	// (a runtime entity with no baseline, stored whole). The two modes and the
	// per-kind fields live in SaveData::EntityState.

	// Monsters: a baseline gets a diff once it has moved off its spawn cell,
	// announced itself, or taken damage (incl. being slain). An editor-placed
	// monster (id < 0) has no baseline, so it is stored whole to recreate.
	for (const Monster& m : m_monsters) {
		SaveData::EntityState e;
		e.kind = EntityKind::Monster;
		if (m.id < 0) {
			e.id = -1;
			e.type = m.kind ? m.kind->name : std::string();
			e.x = m.x;
			e.z = m.z;
			e.facing = static_cast<int>(m.facing);
			e.announced = m.announced;
			e.hp = m.hp;
			e.spawnX = m.spawnX;
			e.spawnZ = m.spawnZ;
			ls.entities.push_back(std::move(e));
		} else if (m.x != m.spawnX || m.z != m.spawnZ || m.announced ||
				   m.hp != m.MaxHp()) {
			e.id = m.id;
			e.x = m.x;
			e.z = m.z;
			e.announced = m.announced;
			e.hp = m.hp;
			ls.entities.push_back(std::move(e));
		}
	}
	// Items: a baseline rune gets a one-bit diff once collected; a dropped tablet
	// (id < 0) still on the floor is stored whole. A collected dropped tablet is
	// simply gone — no record (it falls out of both branches).
	for (const Item& item : m_items) {
		SaveData::EntityState e;
		e.kind = EntityKind::Item;
		if (item.id >= 0) {
			if (item.collected) {
				e.id = item.id;
				e.collected = true;
				ls.entities.push_back(std::move(e));
			}
		} else if (!item.collected) {
			e.id = -1;
			e.type = item.kind->id;
			e.x = item.x;
			e.z = item.z;
			ls.entities.push_back(std::move(e));
		}
	}
	// Buttons: a baseline button gets a diff once it has been activated.
	for (const Button& b : m_buttons)
		if (b.activated) {
			SaveData::EntityState e;
			e.kind = EntityKind::Button;
			e.id = b.id;
			e.activated = true;
			ls.entities.push_back(std::move(e));
		}
	return ls;
}

void DungeonWorld::StashActive() {
	m_levelStates[m_currentLevel] = SnapshotActive();
}

void DungeonWorld::ApplyActiveSnapshot() {
	auto it = m_levelStates.find(m_currentLevel);
	if (it == m_levelStates.end()) return; // first visit — nothing to restore
	const SaveData::LevelState& ls = it->second;

	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	for (const auto& [x, z] : ls.seen)
		if (x >= 0 && z >= 0 && x < m_map.Width() && z < m_map.Height())
			m_seen[static_cast<size_t>(z) * m_map.Width() + x] = 1;
	// Editor-placed monsters and dropped tablets have no .ent baseline, so the
	// snapshot's whole SPAWN rows are authoritative: drop any live ones (e.g.
	// placed/dropped earlier this session, or the .ent baseline LoadItems rebuilt)
	// and recreate them from the save below. Baseline diffs apply onto the kept
	// baseline instances by id.
	std::erase_if(m_monsters, [](const Monster& m) { return m.id < 0; });
	std::erase_if(m_items, [](const Item& i) { return i.id < 0; });
	// v6 migration: that save stored a whole floor snapshot (no per-item diff).
	// Mark every baseline rune collected up front; the Item rows below revive the
	// ones actually on the floor — matched by cell + type, so an untouched baseline
	// keeps its .ent id (and won't re-serialize as a drop that later duplicates
	// it). A rune absent from the snapshot (picked up in the v6 save) stays gone.
	if (ls.fullFloorSnapshot)
		for (Item& item : m_items)
			if (item.id >= 0) item.collected = true;

	for (const SaveData::EntityState& e : ls.entities) {
		switch (e.kind) {
		case EntityKind::Monster:
			if (e.id < 0) {
				// Whole editor-placed monster — recreate at its spawn, then snap to
				// the saved live cell/state.
				if (!m_project.monsters.Contains(e.type)) break;
				MonsterKind& kind = MonsterKindFor(e.type);
				Monster m = MakeMonster(kind, -1, e.spawnX, e.spawnZ,
										static_cast<Direction>(e.facing));
				m.x = e.x;
				m.z = e.z;
				m.announced = e.announced;
				if (e.hp >= 0.0f) m.hp = e.hp; // -1 = older save → keep spawn hp
				m.visualPos = m_map.CellCenter(m.x, m.z);
				m_monsters.push_back(std::move(m));
			} else {
				for (Monster& m : m_monsters)
					if (m.id == e.id) {
						m.x = e.x;
						m.z = e.z;
						m.announced = e.announced;
						if (e.hp >= 0.0f) m.hp = e.hp; // -1 = older save → keep spawn hp
						m.moving = false; // snap to the saved cell, no glide from origin
						m.moveT = 0.0f;
						m.visualPos = m_map.CellCenter(m.x, m.z);
						break;
					}
			}
			break;
		case EntityKind::Item:
			if (ls.fullFloorSnapshot) {
				// v6 floor row: revive the collected baseline rune of this type at
				// this cell (keeping its id), else lay a non-baseline tablet down.
				bool revived = false;
				for (Item& item : m_items)
					if (item.id >= 0 && item.collected && item.x == e.x &&
						item.z == e.z && item.kind && item.kind->id == e.type) {
						item.collected = false;
						revived = true;
						break;
					}
				if (!revived) {
					ItemKind& kind = ItemKindFor(e.type);
					m_items.push_back({&kind, m_nextDropId--, e.x, e.z, false});
				}
			} else if (e.id < 0) {
				// Dropped tablet — lay it on the floor with a fresh runtime id.
				ItemKind& kind = ItemKindFor(e.type);
				m_items.push_back({&kind, m_nextDropId--, e.x, e.z, false});
			} else {
				// Baseline rune collected — mark the kept instance lifted.
				for (Item& item : m_items)
					if (item.id == e.id) {
						item.collected = e.collected;
						break;
					}
			}
			break;
		case EntityKind::Button:
			for (Button& b : m_buttons)
				if (b.id == e.id) {
					b.activated = e.activated;
					break;
				}
			break;
		default: break; // decorations are static — never in a save
		}
	}
	m_levelStates.erase(it); // the live state is authoritative now
}

void DungeonWorld::CaptureState(SaveData& out) const {
	out.currentLevel = m_currentLevel;
	out.partyX = m_party.GridX();
	out.partyZ = m_party.GridZ();
	out.partyFacing = m_party.Facing();
	out.lookYaw = m_party.LookYaw();
	out.lookPitch = m_party.LookPitch();
	out.looking = m_party.IsLooking();
	out.torchPalette = m_torchPalette;

	// Every inactive visited level, plus the live one.
	out.levels.clear();
	for (const auto& [stem, ls] : m_levelStates) out.levels.push_back(ls);
	out.levels.push_back(SnapshotActive());
}

void DungeonWorld::ApplyState(const SaveData& in) {
	m_party.SetGridPosition(in.partyX, in.partyZ); // keeps facing, clears interp
	m_party.SetFacing(in.partyFacing);
	// Re-layer the free-look offset on the restored facing (SetFacing cleared it).
	m_party.SetLookState(in.lookYaw, in.lookPitch, in.looking);
	SetTorchPalette(in.torchPalette);

	// Load every level's saved state into the per-level store. The active level's
	// state is applied by ApplyActiveSnapshot once Game has routed to
	// in.currentLevel (its entity diff needs the monsters built).
	m_levelStates.clear();
	for (const SaveData::LevelState& ls : in.levels) m_levelStates[ls.stem] = ls;
}

bool DungeonWorld::IsSeen(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_map.Width() || z >= m_map.Height()) return false;
	return m_seen[static_cast<size_t>(z) * m_map.Width() + x] != 0;
}

void DungeonWorld::EditCell(int x, int z, Cell cell) {
	const u32 rev = m_map.Revision();
	m_map.SetCell(x, z, cell);
	if (m_map.Revision() == rev) return; // unchanged
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

void DungeonWorld::EditVariant(int x, int z, SurfaceSel sel, int variant) {
	if (!m_map.IsWalkable(x, z)) return; // variants live on floor cells
	const u32 rev = m_map.Revision();
	switch (sel) {
	case SurfaceSel::Wall:    m_map.SetWallVariant(x, z, variant); break;
	case SurfaceSel::Floor:   m_map.SetFloorVariant(x, z, variant); break;
	case SurfaceSel::Ceiling: m_map.SetCeilingVariant(x, z, variant); break;
	}
	if (m_map.Revision() == rev) return; // unchanged
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

bool DungeonWorld::AddDecoration(const std::string& type, int x, int z,
								 Direction facing) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (!m_project.decorations.Contains(type)) return false;
	DecorationKind& kind = DecorationKindFor(type, m_project.decorations);
	Decoration deco;
	deco.kind = &kind;
	deco.x = x;
	deco.z = z;
	deco.facing = facing;
	const Vec3 pos = m_map.CellCenter(x, z);
	XMStoreFloat4x4(&deco.world, XMMatrixRotationY(DirYaw(facing)) *
									 XMMatrixTranslation(pos.x, 0, pos.z));
	deco.solid = kind.solidDefault;
	m_decorations.push_back(std::move(deco));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddMonster(const std::string& type, int x, int z,
							  Direction facing) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (!m_project.monsters.Contains(type)) return false;
	for (const Monster& m : m_monsters)
		if (m.x == x && m.z == z) return false; // one monster per cell
	MonsterKind& kind = MonsterKindFor(type);
	// id = -1 marks an editor-placed monster (no .ent baseline); the save layer
	// stores these whole (a "monster" row) rather than as a diff, so they
	// round-trip — see SnapshotActive / ApplyActiveSnapshot.
	m_monsters.push_back(MakeMonster(kind, -1, x, z, facing));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddFixture(const std::string& type, int x, int z) {
	if (!m_map.IsWalkable(x, z)) return false;
	const std::string mount = CatalogGet(m_project.fixtures.Find(type), "mount", "floor");
	const bool ok = mount == "wall" ? m_map.AddSconce(x, z) : m_map.AddBrazier(x, z);
	if (!ok) return false;
	// Rebuild the fire instances + dust from the updated map (lights pick the new
	// fire up next frame; the GPU may still read the old turbidity, so drain it).
	m_device.WaitIdle();
	m_fires.clear();
	BuildFires();
	BuildTurbidityMap();
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::RemoveEntityAt(int x, int z) {
	for (auto it = m_monsters.begin(); it != m_monsters.end(); ++it)
		if (it->x == x && it->z == z) {
			m_monsters.erase(it);
			return true;
		}
	for (auto it = m_decorations.begin(); it != m_decorations.end(); ++it)
		if (it->x == x && it->z == z) {
			m_decorations.erase(it);
			return true;
		}
	return false;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::MonsterMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_monsters.size());
	for (const Monster& m : m_monsters)
		if (m.Alive()) // a slain monster leaves no map marker
			markers.push_back({m.x, m.z, m.kind ? m.kind->name : std::string()});
	return markers;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::DecorationMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_decorations.size());
	for (const Decoration& d : m_decorations) {
		if (d.stair) continue; // stairs draw from their own (typed) marker
		markers.push_back({d.x, d.z, d.kind ? d.kind->id : std::string()});
	}
	return markers;
}

void DungeonWorld::BeginLevelLoad(const std::string& stem, bool stashCurrent) {
	m_device.WaitIdle(); // the GPU may still be reading the old level's meshes

	// Save the level we're leaving so a later return restores its fog/progress
	// (skip for a throwaway baseline being replaced by a save's level).
	if (stashCurrent) StashActive();

	// Move-assign the new level into the existing objects (Party holds a
	// reference to m_map, so the object must persist — only its data changes).
	m_map = DungeonMap(m_project.LevelMapPath(stem));
	m_entities = DungeonEntities(m_project.LevelEntPath(stem), m_map);
	m_currentLevel = stem;

	// Reset per-level state. The shared caches (m_monsterKinds, m_decorationKinds,
	// m_propTextures) persist — they are keyed by name and reused across levels.
	// Instance lists must be cleared (LoadMonsters/LoadDecorations/BuildFires
	// push_back); the surface chunks/blocks/textures self-reset when the caller
	// re-runs AppendLoadTasks.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	m_monsters.clear();
	m_items.clear();
	m_buttons.clear();
	m_decorations.clear();
	m_fires.clear();
	m_pendingTransition.reset();
	for (ShadowSlotCache& slot : m_shadowCache) slot = ShadowSlotCache{};
	ResolveSurfacePalettes();
}

std::optional<DungeonWorld::LevelTransition> DungeonWorld::ConsumeLevelTransition() {
	std::optional<LevelTransition> t = std::move(m_pendingTransition);
	m_pendingTransition.reset();
	return t;
}

namespace {
// How long a hit-feedback splat stays over a struck member's portrait.
constexpr float kHitFlashSeconds = 0.7f;

const char* DirName(Direction d) {
	switch (d) {
	case Direction::North: return "north";
	case Direction::East:  return "east";
	case Direction::West:  return "west";
	default:               return "south";
	}
}
const char* KindName(EntityKind k) {
	switch (k) {
	case EntityKind::Monster:    return "monster";
	case EntityKind::Button:     return "button";
	case EntityKind::Decoration: return "decoration";
	default:                     return "item";
	}
}
} // namespace

bool DungeonWorld::SaveLevel() const {
	const DungeonMap& map = m_map;

	// --- static layer (.map) ------------------------------------------------
	std::string m = std::format("; {} — written by the in-game editor.\n\n", m_currentLevel);
	auto palette = [&](const char* surface, const std::vector<std::string>& ids) {
		if (ids.empty()) return;
		m += std::format("palette {}", surface);
		for (const std::string& id : ids) m += " " + id;
		m += '\n';
	};
	palette("wall", map.WallPalette());
	palette("floor", map.FloorPalette());
	palette("ceiling", map.CeilingPalette());
	m += ";\n";

	// Grid: 'P' start, '#' wall, 'D' authored-dusty floor, '.' floor. Fixtures
	// are emitted as records below, so their cells stay plain floor.
	for (int z = 0; z < map.Height(); ++z) {
		std::string row(static_cast<size_t>(map.Width()), '.');
		for (int x = 0; x < map.Width(); ++x) {
			if (x == map.StartX() && z == map.StartZ()) row[x] = 'P';
			else if (map.At(x, z) == Cell::Wall) row[x] = '#';
			else if (map.AuthoredDusty(x, z)) row[x] = 'D';
		}
		m += row;
		m += '\n';
	}
	m += ";\n";

	for (const WallSconce& s : map.Sconces())
		m += std::format("fixture sconce {} {} {}\n", s.x, s.z, DirName(s.wall));
	for (const auto& [bx, bz] : map.BrazierCells())
		m += std::format("fixture brazier {} {}\n", bx, bz);

	for (const StairLink& s : map.Stairs())
		m += std::format("stairs {} {} {} {} dest={} destx={} destz={} destfacing={}\n",
						 s.type, s.x, s.z, DirName(s.facing), s.destLevel, s.destX,
						 s.destZ, DirName(s.destFacing));

	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x) {
			if (map.WallVariant(x, z) >= 0)
				m += std::format("variant wall {} {} {}\n", x, z, map.WallVariant(x, z));
			if (map.FloorVariant(x, z) >= 0)
				m += std::format("variant floor {} {} {}\n", x, z, map.FloorVariant(x, z));
			if (map.CeilingVariant(x, z) >= 0)
				m += std::format("variant ceiling {} {} {}\n", x, z, map.CeilingVariant(x, z));
		}

	// Decorations reconstructed from the live instances (so editor placements /
	// removals persist); stair props are skipped — they are stairs records.
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		if (d.wallMounted) {
			m += std::format("decoration {} {} {} wall={}", d.kind->id, d.x, d.z,
							 DirName(d.wall));
			if (d.solid) m += " solid=1";
		} else {
			m += std::format("decoration {} {} {} {}", d.kind->id, d.x, d.z,
							 DirName(d.facing));
			if (d.solid != d.kind->solidDefault)
				m += std::format(" solid={}", d.solid ? 1 : 0);
		}
		m += '\n';
	}

	// --- dynamic layer (.ent) -----------------------------------------------
	std::string e =
		std::format("; {} — written by the in-game editor (dynamic layer).\n\n", m_currentLevel);
	for (const Monster& mon : m_monsters)
		e += std::format("monster {} {} {} {}\n",
						 mon.kind ? mon.kind->name : std::string("?"), mon.x, mon.z,
						 DirName(mon.facing));
	// Items/buttons aren't editor-editable yet — carry the loaded records through.
	for (const Entity& ent : m_entities.All()) {
		if (ent.kind != EntityKind::Item && ent.kind != EntityKind::Button) continue;
		e += std::format("{} {} {} {} {}", KindName(ent.kind), ent.type, ent.x, ent.z,
						 DirName(ent.facing));
		for (const auto& [k, v] : ent.params) e += std::format(" {}={}", k, v);
		e += '\n';
	}

	const bool okMap =
		assets::WriteBinaryFile(m_project.LevelMapPath(m_currentLevel), m.data(), m.size());
	const bool okEnt =
		assets::WriteBinaryFile(m_project.LevelEntPath(m_currentLevel), e.data(), e.size());
	if (okMap && okEnt)
		log::Info("Saved level {}: {} decorations, {} monsters", m_currentLevel,
				  m_decorations.size(), m_monsters.size());
	else
		log::Warn("Failed to write level {} files", m_currentLevel);
	return okMap && okEnt;
}

void DungeonWorld::PlacePartyAt(int x, int z, Direction facing) {
	m_party.SetGridPosition(x, z);
	m_party.SetFacing(static_cast<int>(facing));
	MarkSeen(x, z);
}

void DungeonWorld::RebuildChunkRegion(int chunkX, int chunkZ) {
	const int chunksX = (m_map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunkIndex = chunkZ * chunksX + chunkX;
	DungeonGeometry r = BuildDungeonRegion(m_map, m_wallBlocks, m_floorBlocks,
										   m_ceilingBlocks, chunkX, chunkZ);
	auto replace = [&](Surface& surface, std::vector<GeometryChunk>& fresh) {
		std::erase_if(surface.chunks,
					  [&](const SurfaceChunk& sc) { return sc.chunk == chunkIndex; });
		for (GeometryChunk& gc : fresh) surface.chunks.push_back(MakeSurfaceChunk(gc));
	};
	replace(m_walls, r.walls);
	replace(m_floors, r.floors);
	replace(m_ceilings, r.ceilings);
}

void DungeonWorld::RebuildChunksAround(int x, int z) {
	if (m_walls.chunks.empty()) return; // geometry not built yet
	m_device.WaitIdle();                // old chunk meshes may still be in flight

	// The edit changes (x,z) plus the wall faces its orthogonal neighbours share
	// with it, so rebuild every distinct chunk those cells fall in (≤ 5).
	const int n[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	int doneX[5], doneZ[5], count = 0;
	for (const auto& o : n) {
		const int cx = x + o[0], cz = z + o[1];
		if (cx < 0 || cz < 0 || cx >= m_map.Width() || cz >= m_map.Height()) continue;
		const int rx = cx / kChunkCells, rz = cz / kChunkCells;
		bool seen = false;
		for (int i = 0; i < count; ++i)
			if (doneX[i] == rx && doneZ[i] == rz) seen = true;
		if (seen) continue;
		doneX[count] = rx;
		doneZ[count] = rz;
		++count;
		RebuildChunkRegion(rx, rz);
	}
}

void DungeonWorld::MarkSeen(int x, int z) {
	for (int dz = -1; dz <= 1; ++dz) {
		for (int dx = -1; dx <= 1; ++dx) {
			const int cx = x + dx, cz = z + dz;
			if (cx < 0 || cz < 0 || cx >= m_map.Width() || cz >= m_map.Height())
				continue;
			m_seen[static_cast<size_t>(cz) * m_map.Width() + cx] = 1;
		}
	}
}

void DungeonWorld::SetTorchPalette(int index) {
	m_torchPalette = index;
	switch (index) {
	case 1:  m_torchColor = {0.45f, 0.65f, 1.0f}; onMessage(loc::Tr("log.torch_cold")); break;
	case 2:  m_torchColor = {0.55f, 1.0f, 0.45f}; onMessage(loc::Tr("log.torch_eerie")); break;
	default: m_torchColor = {1.0f, 0.62f, 0.28f}; onMessage(loc::Tr("log.torch_warm")); break;
	}
}

void DungeonWorld::Update(const Input& input, float dt, float time, bool acceptInput) {
	m_time = time; // drives the rune emissive pulse in SubmitSceneGeometry
	if (acceptInput) m_party.HandleInput(input);
	m_party.Update(dt);
	if (m_pillarActive) m_pillarAnimator.Update(dt);
	UpdateMonsters(dt);
	UpdateLights(time);
	UpdateCamera();

	// Advance the fires and gather their particles, sorted back-to-front so
	// the alpha-blended smoke composites correctly (additive flames are
	// order-independent).
	m_particleScratch.clear();
	for (Fire& fire : m_fires) {
		fire.effect.Update(dt);
		fire.effect.AppendParticles(m_particleScratch);
	}
	// (Rune tablets glow as a whole via an additive emissive that pulses in their
	// element colour — applied to the mesh in SubmitSceneGeometry, not a billboard.)
	const Vec3 eye = m_party.EyePosition();
	const Vec3 fwd = m_camera.Forward();
	std::ranges::sort(m_particleScratch, std::greater{},
					  [&](const gfx::ParticleInstance& p) {
						  const Vec3 d = Sub(p.position, eye);
						  return d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
					  });
}

void DungeonWorld::SetFov(float degrees) {
	m_fovDegrees = std::clamp(degrees, 20.0f, 140.0f);
}

std::vector<std::string> DungeonWorld::MonsterList() const {
	std::vector<std::string> out;
	out.reserve(m_monsters.size());
	for (const Monster& m : m_monsters)
		out.push_back(std::format("{} @ {},{}", m.kind ? m.kind->name : "?", m.x, m.z));
	return out;
}

bool DungeonWorld::ToggleButtonAt(int x, int z, bool& out) {
	for (Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			b.activated = !b.activated;
			out = b.activated;
			return true;
		}
	return false;
}

std::vector<std::string> DungeonWorld::ButtonList() const {
	std::vector<std::string> out;
	out.reserve(m_buttons.size());
	for (const Button& b : m_buttons)
		out.push_back(std::format("{} @ {},{} = {}", b.id, b.x, b.z,
								  b.activated ? "on" : "off"));
	return out;
}

void DungeonWorld::UpdateCamera() {
	m_camera.SetPosition(m_party.EyePosition());
	m_camera.SetYawPitch(m_party.EyeYaw(), m_party.EyePitch());
	m_camera.SetLens(m_fovDegrees * kPi / 180.0f,
					 static_cast<float>(m_device.Width()) /
						 static_cast<float>(m_device.Height()),
					 0.05f, 100.0f);
}

// A rune's "breath": one multiplier (~0.2 dim .. ~1.9 bright, phase-offset per
// item) shared by BOTH its emissive self-glow (SubmitSceneGeometry) and the
// light it casts (UpdateLights), so the tablet and the light it throws pulse in
// exact lockstep. Both callers pass the same frame time and the item id.
static float RunePulse(float time, int id) {
	return 1.05f + 0.85f * std::sin(time * 3.0f + static_cast<float>(id));
}

// Rebuilds the light list every frame: the carried torch follows the camera,
// wall torches flicker with independent phases, the pillar glows. All
// flicker is product-of-sines — cheap, deterministic, and aperiodic enough.
void DungeonWorld::UpdateLights(float time) {
	m_lights.points.clear();

	const Vec3 eye = m_party.EyePosition();
	const float flicker =
		0.92f + 0.08f * std::sin(time * 9.0f) * std::sin(time * 13.7f + 1.3f);
	gfx::PointLight torch;
	torch.position = {eye.x, eye.y + 0.25f, eye.z};
	torch.radius = 9.0f;
	torch.color = m_torchColor;
	torch.intensity = 2.6f * flicker;
	m_lights.points.push_back(torch);

	// One flickering light per fire, sitting just above its flame. Braziers
	// burn bigger and a touch redder than the wall sconces. The light
	// POSITION wanders too (incommensurate sine products, per-fire phase):
	// the shadow cubes re-render from the moved origin every frame, so the
	// shadows themselves dance the way real firelight does.
	for (const Fire& fire : m_fires) {
		gfx::PointLight light;
		const float amp = fire.brazier ? 0.042f : 0.028f;
		const float wx = amp * std::sin(time * 7.3f + fire.phase) *
						 std::sin(time * 3.1f + fire.phase * 2.0f);
		const float wy = amp * 0.6f * std::sin(time * 9.1f + fire.phase * 1.3f);
		const float wz = amp * std::sin(time * 6.7f + fire.phase * 0.7f) *
						 std::sin(time * 2.6f + fire.phase);
		light.position = {fire.flamePos.x + wx, fire.flamePos.y + 0.15f + wy,
						  fire.flamePos.z + wz};
		// Braziers reach ~6 cells (14.4 m) so their shadow has room to fade in
		// gently over distance (AssignShadowSlots); inverse-square falloff keeps
		// the far tail dim, so this widens the lit pool without blowing out up
		// close. Wall sconces stay tighter.
		light.radius = fire.brazier ? 14.4f : 7.0f;
		light.color = fire.brazier
						  ? Vec3{m_torchColor.x, m_torchColor.y * 0.85f, m_torchColor.z * 0.8f}
						  : m_torchColor;
		const float base = fire.brazier ? 2.3f : 1.8f;
		light.intensity = base * (0.9f + 0.1f * std::sin(time * 11.0f + fire.phase) *
											 std::sin(time * 7.3f + fire.phase));
		light.flickerShadow = true; // wandering origin → throttle its shadow cube
		light.longShadowFade = fire.brazier; // braziers fade over their long reach;
											 // sconces keep near-field shadows crisp
		m_lights.points.push_back(light);
	}

	if (m_pillarActive) {
		gfx::PointLight glow;
		glow.position = {m_pillarPos.x, 1.3f, m_pillarPos.z};
		glow.radius = 5.0f;
		glow.color = {0.3f, 0.9f, 0.6f};
		glow.intensity = 1.2f + 0.2f * std::sin(time * 2.2f);
		// The glow casts the pillar's coil shadows from inside the mesh; that is
		// a fixture of the scene, not a fire popping in, and its short range
		// would otherwise fade the shadow out at any normal viewing distance.
		glow.fadeShadow = false;
		m_lights.points.push_back(glow);
	}

	// Each uncollected rune throws a soft pulsing light in its element colour,
	// breathing in lockstep with the tablet's emissive glow (same RunePulse).
	// Pure fill light — castsShadow=false keeps the cluster near the start from
	// stealing the few shadow cubes from the torch/fires.
	for (const Item& item : m_items) {
		if (item.collected || !item.kind->isRune) continue;
		const Vec3 c = m_map.CellCenter(item.x, item.z);
		gfx::PointLight glow;
		glow.position = {c.x, 0.4f, c.z};
		glow.radius = 4.8f;
		const Vec4& g = item.kind->glow;
		glow.color = {g.x, g.y, g.z};
		glow.intensity = 2.3f * RunePulse(time, item.id);
		glow.castsShadow = false;
		m_lights.points.push_back(glow);
	}

	// The renderer uploads only the active light budget (Settings → Video → Max
	// Lights, Low=16 .. Ultra=64) and shadow slots only consider those, so on a
	// large level the fire count alone can crowd out a light pushed late (the
	// pillar glow). Keep the ones NEAREST the eye instead of the first ones
	// pushed; the carried torch sits at the eye, so it always survives (and
	// still wins shadow slot 0 in AssignShadowSlots).
	const size_t budget = static_cast<size_t>(
		std::clamp(m_settings.maxPointLights, 1, static_cast<int>(gfx::kMaxPointLights)));
	if (m_lights.points.size() > budget) {
		auto distSq = [&](const gfx::PointLight& l) {
			const Vec3 d = Sub(l.position, eye);
			return d.x * d.x + d.y * d.y + d.z * d.z;
		};
		std::nth_element(m_lights.points.begin(),
						 m_lights.points.begin() + budget, m_lights.points.end(),
						 [&](const gfx::PointLight& a, const gfx::PointLight& b) {
							 return distSq(a) < distSq(b);
						 });
		m_lights.points.resize(budget);
	}

	AssignShadowSlots();
}

// Hands the kShadowSlots shadow cubes to the lights nearest the camera —
// slot 0 (highest resolution + PCF) to the closest, coarser slots outward,
// nothing beyond that. Only lights the renderer will upload (the first
// kMaxPointLights) compete, so a slot is never spent on a dropped light.
// The carried torch sits at the eye so it always wins slot 0: its surface
// shadows mostly hide behind their casters, but it is exactly what carves
// shafts through dusty air around nearby pillars.
// Two things smooth the hard slot boundary: assignment is hysteretic (a slot
// holder resists being bumped by a marginally-closer rival), and each slotted
// light gets a shadowStrength that fades 0->1 as it enters its shadow range,
// so a shadow dissolves in rather than popping the instant the light wins a
// slot (the original "shadow snaps on 3 squares away" artifact).
void DungeonWorld::AssignShadowSlots() {
	static_assert(gfx::kShadowSlots <= gfx::kMaxPointLights);
	const Vec3 eye = m_party.EyePosition();

	for (gfx::PointLight& light : m_lights.points) {
		light.shadowSlot = -1;
		light.shadowStrength = 1.0f;
	}
	if (!m_shadowsEnabled) { // dev console: lights stay lit, just unshadowed
		m_prevShadowPos.clear();
		return;
	}

	// Rank candidate lights by distance to the eye (linear, so the hysteresis
	// margin below is in metres). A light that held a slot last frame gets a
	// small discount so two near-equidistant fires don't trade slots — and the
	// resolution tier that rides on the slot — back and forth as the party
	// moves between them; the steadier slot also lets ShadowSlotCache reuse its
	// cube more often. Incumbents are matched by POSITION, not index: the light
	// list is rebuilt every frame and budget culling can shuffle indices, but a
	// fire only wanders a few cm between frames.
	constexpr float kHysteresis = 0.75f;   // metres of slack for a slot incumbent
	constexpr float kReMatch2 = 0.25f;     // (0.5 m)²: "still the same light"

	m_shadowCandidates.clear();
	const size_t lightCount =
		std::min<size_t>(m_lights.points.size(), gfx::kMaxPointLights);
	for (size_t i = 0; i < lightCount; ++i) {
		if (!m_lights.points[i].castsShadow) continue; // pure fill light (runes)
		const Vec3 d = Sub(m_lights.points[i].position, eye);
		float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		for (const Vec3& prev : m_prevShadowPos) {
			const Vec3 e = Sub(m_lights.points[i].position, prev);
			if (e.x * e.x + e.y * e.y + e.z * e.z <= kReMatch2) {
				dist -= kHysteresis; // incumbent: bias toward keeping its slot
				break;
			}
		}
		m_shadowCandidates.emplace_back(dist, i);
	}
	std::ranges::sort(m_shadowCandidates);

	// Two fade profiles, both ending at the light's radius and smoothstepped
	// (softer than linear), anchored to per-light distance (a STABLE quantity,
	// unlike the rank cutoff that drifts with how many lights are near):
	//   - longShadowFade (braziers): fade across most of the reach, from 12% of
	//     the radius out — a long LOD ramp the big brazier radius is sized for.
	//   - default (sconces, glows): keep full strength except in the outer band,
	//     so a caster beside a normal-radius light still casts a visible shadow
	//     at viewing distance instead of fading out under the brazier tuning.
	constexpr float kFadeStartFrac = 0.12f;       // long ramp: inner edge = 12% of radius
	constexpr float kEdgeFadeBand = 1.5f * kCellSize; // default: soften the outer ~1.5 cells
													  // (wide enough that a sconce winning a
													  // slot mostly ramps in rather than pops)

	const size_t count = std::min<size_t>(m_shadowCandidates.size(), gfx::kShadowSlots);
	m_prevShadowPos.clear();
	for (size_t slot = 0; slot < count; ++slot) {
		gfx::PointLight& light = m_lights.points[m_shadowCandidates[slot].second];
		light.shadowSlot = static_cast<int>(slot);

		// Persistent short-range lights (the pillar glow) keep full-strength
		// shadows at any distance; only fade the fires that pop in on approach.
		if (light.fadeShadow) {
			// True distance, not the hysteresis-discounted sort key.
			const Vec3 d = Sub(light.position, eye);
			const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			const float fadeEnd = light.radius;
			const float fadeStart = light.longShadowFade
				? fadeEnd * kFadeStartFrac
				: std::max(0.0f, fadeEnd - kEdgeFadeBand);
			const float t = (fadeEnd > fadeStart)
				? std::clamp((fadeEnd - dist) / (fadeEnd - fadeStart), 0.0f, 1.0f)
				: 1.0f;
			light.shadowStrength = t * t * (3.0f - 2.0f * t); // smoothstep, gentler
		}

		m_prevShadowPos.push_back(light.position);
	}
}

void DungeonWorld::UpdateMonsters(float dt) {
	const Vec3 partyPos = m_party.EyePosition();

	// Tick down each member's per-hand swing cooldowns so hands free up over
	// time, and fade out the hit-feedback splat over its portrait.
	if (m_roster)
		for (Character& member : *m_roster) {
			for (float& cd : member.handCooldown)
				if (cd > 0.0f) cd -= dt;
			if (member.hitFlash > 0.0f) member.hitFlash -= dt;
		}

	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& monster = m_monsters[i];
		if (!monster.Alive()) continue; // downed — no animation, no AI, not solid
		monster.animator.Update(dt);
		if (monster.attackCd > 0.0f) monster.attackCd -= dt;
		if (monster.moveCd > 0.0f) monster.moveCd -= dt;

		// Advance an in-flight glide; the logical cell already moved when the
		// step committed, so the tween just slides visualPos to the new centre.
		if (monster.moving) {
			monster.moveT += dt / std::max(monster.kind->moveInterval, 0.05f);
			const Vec3 target = m_map.CellCenter(monster.x, monster.z);
			if (monster.moveT >= 1.0f) {
				monster.moving = false;
				monster.moveT = 0.0f;
				monster.visualPos = target;
			} else {
				const float s = monster.moveT * monster.moveT *
								(3.0f - 2.0f * monster.moveT); // smoothstep
				monster.visualPos = {monster.moveFrom.x + (target.x - monster.moveFrom.x) * s,
									 monster.moveFrom.y + (target.y - monster.moveFrom.y) * s,
									 monster.moveFrom.z + (target.z - monster.moveFrom.z) * s};
			}
		}

		const int dx = std::abs(monster.x - m_party.GridX());
		const int dz = std::abs(monster.z - m_party.GridZ());
		const int dist = std::max(dx, dz); // Chebyshev cells to the party
		const bool engaged = static_cast<float>(dist) <= monster.kind->aggroRange;

		// Turn to face the party once engaged (radial models opt out via the
		// catalog `faces` field). Based on the visual position so the turn glides.
		if (engaged && monster.kind->facesTarget)
			monster.yaw = std::atan2(partyPos.x - monster.visualPos.x,
									 partyPos.z - monster.visualPos.z);

		// Announce once when the party first comes adjacent.
		if (!monster.announced && dist <= 1) {
			monster.announced = true;
			onMessage(loc::Format("log.monster_stirs",
								  loc::Tr("monster." + monster.kind->name)));
			m_audio.Play(m_sounds.monster, 0.7f);
		}

		// Chase: engaged and not yet adjacent -> step one cell toward the party
		// (logical cell snaps now, the glide above carries the visual). Adjacent
		// and off cooldown -> swing at a random standing member instead.
		if (dist <= 1) {
			if (monster.attackCd <= 0.0f) MonsterAttack(monster);
		} else if (engaged && !monster.moving && monster.moveCd <= 0.0f) {
			int nx = 0, nz = 0;
			if (NextStepToward(monster, i, nx, nz)) {
				monster.moveFrom = monster.visualPos;
				monster.x = nx;
				monster.z = nz;
				monster.moving = true;
				monster.moveT = 0.0f;
				monster.moveCd = monster.kind->moveInterval;
			}
		}
	}
}

bool DungeonWorld::CellFreeForMonster(int x, int z, size_t self) const {
	if (!m_map.IsWalkable(x, z)) return false;
	if (x == m_party.GridX() && z == m_party.GridZ()) return false;
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		if (i == self) continue;
		const Monster& o = m_monsters[i];
		if (o.Alive() && o.x == x && o.z == z) return false;
	}
	return true;
}

bool DungeonWorld::NextStepToward(const Monster& monster, size_t self, int& outX,
								  int& outZ) {
	const int W = m_map.Width(), H = m_map.Height();
	if (W <= 0 || H <= 0) return false;
	const int startIdx = monster.z * W + monster.x;
	const int goalIdx = m_party.GridZ() * W + m_party.GridX();

	m_pathFrom.assign(static_cast<size_t>(W) * H, -1);
	std::queue<int> open;
	m_pathFrom[startIdx] = startIdx; // self-parent = visited sentinel
	open.push(startIdx);

	static constexpr int kDX[4] = {1, -1, 0, 0};
	static constexpr int kDZ[4] = {0, 0, 1, -1};
	bool found = false;
	while (!open.empty()) {
		const int cur = open.front();
		open.pop();
		if (cur == goalIdx) { found = true; break; }
		const int cx = cur % W, cz = cur / W;
		for (int d = 0; d < 4; ++d) {
			const int nx = cx + kDX[d], nz = cz + kDZ[d];
			if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
			const int nidx = nz * W + nx;
			if (m_pathFrom[nidx] != -1) continue; // already visited
			// The goal (party cell) is reachable as the BFS target even though a
			// monster can't stand on it; every other cell must be free to walk.
			if (nidx != goalIdx && !CellFreeForMonster(nx, nz, self)) continue;
			if (nidx == goalIdx && !m_map.IsWalkable(nx, nz)) continue;
			m_pathFrom[nidx] = cur;
			open.push(nidx);
		}
	}
	if (!found) return false;

	// Walk the predecessors back from the goal to the first step off the start.
	int cur = goalIdx;
	while (m_pathFrom[cur] != startIdx) cur = m_pathFrom[cur];
	outX = cur % W;
	outZ = cur / W;
	return true;
}

// One monster strike against a random standing party member. Sets the swing
// cooldown whether or not it lands so a packed cell doesn't machine-gun.
void DungeonWorld::MonsterAttack(Monster& monster) {
	if (!m_roster || m_partyWiped) return;

	// Pick uniformly among the members still up.
	std::array<size_t, 4> alive;
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && n < alive.size(); ++i)
		if ((*m_roster)[i].IsAlive()) alive[n++] = i;
	if (n == 0) return;

	Character& target = (*m_roster)[alive[m_combatRng() % n]];
	monster.attackCd = monster.kind->attackInterval;

	const AttackProfile atk{monster.kind->damage, monster.kind->accuracy};
	const DefenseProfile def{target.Evasion(), target.Armor()};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
	const std::string name = loc::Tr("monster." + monster.kind->name);

	if (!r.hit) {
		onMessage(loc::Format("log.monster_misses", name, target.name));
		return;
	}
	target.health -= r.damage;
	if (target.health < 0.0f) target.health = 0.0f;
	// Flash a splat over the struck member's portrait. Severity by raw damage is
	// a placeholder — "what a hit means" (relative to max hp / armor / etc.) is
	// TBD; for now small < 5, medium < 10, hard otherwise.
	target.hitFlash = kHitFlashSeconds;
	target.hitSeverity = r.damage < 5.0f ? 0 : (r.damage < 10.0f ? 1 : 2);
	int dmg = static_cast<int>(r.damage + 0.5f);
	onMessage(loc::Format("log.monster_hits", name, target.name, dmg));
	m_audio.Play(m_sounds.monster, 0.6f);

	if (!target.IsAlive()) onMessage(loc::Format("log.member_down", target.name));

	// Party wipe: latch so the run ends exactly once.
	bool anyUp = false;
	for (const Character& m : *m_roster)
		if (m.IsAlive()) { anyUp = true; break; }
	if (!anyUp && !m_partyWiped) {
		m_partyWiped = true;
		onMessage(loc::Tr("log.party_wipe"));
		if (onPartyWipe) onPartyWipe();
	}
}

// A blocked move has lurched the party into the obstacle. Every standing member
// is jarred for a small flat amount, with the smallest splat over each portrait
// and a single grunt — then we re-check for a wipe so a final stumble still ends
// the run cleanly.
void DungeonWorld::OnBumpImpact() {
	if (!m_roster || m_partyWiped) return;

	constexpr float kBumpDamage = 2.0f; // small flat jar, regardless of armor
	bool anyHurt = false;
	for (Character& member : *m_roster) {
		if (!member.IsAlive()) continue;
		member.health -= kBumpDamage;
		if (member.health < 0.0f) member.health = 0.0f;
		member.hitFlash = kHitFlashSeconds;
		member.hitSeverity = 0; // always the small splat
		anyHurt = true;
		if (!member.IsAlive()) onMessage(loc::Format("log.member_down", member.name));
	}
	if (!anyHurt) return;

	onMessage(loc::Format("log.bump_hurt", static_cast<int>(kBumpDamage + 0.5f)));
	m_audio.Play(m_sounds.oof, 0.8f);

	bool anyUp = false;
	for (const Character& m : *m_roster)
		if (m.IsAlive()) { anyUp = true; break; }
	if (!anyUp && !m_partyWiped) {
		m_partyWiped = true;
		onMessage(loc::Tr("log.party_wipe"));
		if (onPartyWipe) onPartyWipe();
	}
}

bool DungeonWorld::PartyAttack(size_t member, size_t hand) {
	if (!m_roster || member >= m_roster->size() || hand > 1) return false;
	Character& attacker = (*m_roster)[member];
	if (!attacker.IsAlive() || attacker.handCooldown[hand] > 0.0f) return false;

	// The cell directly ahead of the party.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const int tx = m_party.GridX() + DirDX(faced);
	const int tz = m_party.GridZ() + DirDZ(faced);

	Monster* target = nullptr;
	for (Monster& m : m_monsters)
		if (m.Alive() && m.x == tx && m.z == tz) { target = &m; break; }

	attacker.handCooldown[hand] = attacker.AttackInterval(hand);
	if (!target) {
		onMessage(loc::Tr("log.attack_air"));
		return true;
	}

	const AttackProfile atk{attacker.AttackDamage(), attacker.Accuracy()};
	const DefenseProfile def{target->kind->evasion, target->kind->armor};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
	const std::string name = loc::Tr("monster." + target->kind->name);

	if (!r.hit) {
		onMessage(loc::Format("log.party_misses", attacker.name, name));
		return true;
	}
	target->hp -= r.damage;
	int dmg = static_cast<int>(r.damage + 0.5f);
	onMessage(loc::Format("log.party_hits", attacker.name, name, dmg));
	m_audio.Play(m_sounds.monster, 0.7f);

	if (!target->Alive()) {
		target->hp = 0.0f; // a downed monster stays in the list (so a new game /
		// save can restore it) but renders, blocks, and acts as dead.
		onMessage(loc::Format("log.monster_slain", name));
	}
	return true;
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound.
// ============================================================================

// --- view culling -----------------------------------------------------------

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromFrustum(const Mat4& m) {
	// Gribb-Hartmann from a row-vector view-proj (clip = p * M): the clip
	// components read the COLUMNS of M, so planes are column sums/differences.
	const Vec4 c1{m._11, m._21, m._31, m._41};
	const Vec4 c2{m._12, m._22, m._32, m._42};
	const Vec4 c3{m._13, m._23, m._33, m._43};
	const Vec4 c4{m._14, m._24, m._34, m._44};
	auto add = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
	};
	auto sub = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
	};
	ViewCull cull;
	cull.isSphere = false;
	const Vec4 raw[6] = {add(c4, c1), sub(c4, c1), add(c4, c2),
						 sub(c4, c2), c3,          sub(c4, c3)}; // L R B T near far
	for (int i = 0; i < 6; ++i) {
		Vec4 p = raw[i];
		const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		if (len > 1e-8f) { p.x /= len; p.y /= len; p.z /= len; p.w /= len; }
		cull.planes[i] = p;
	}
	return cull;
}

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromSphere(const Vec3& center,
														 float radius) {
	ViewCull cull;
	cull.isSphere = true;
	cull.sphereC = center;
	cull.sphereR = radius;
	return cull;
}

bool DungeonWorld::ViewCull::TestSphere(const Vec3& c, float r) const {
	if (isSphere) {
		const Vec3 d = Sub(c, sphereC);
		const float reach = sphereR + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	}
	for (const Vec4& p : planes)
		if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false; // outside
	return true;
}

bool DungeonWorld::ViewCull::TestAABB(const Vec3& lo, const Vec3& hi) const {
	if (isSphere) {
		// Squared distance from the sphere center to the AABB.
		auto axis = [](float c, float l, float h) {
			const float v = c < l ? l - c : (c > h ? c - h : 0.0f);
			return v * v;
		};
		const float d2 = axis(sphereC.x, lo.x, hi.x) + axis(sphereC.y, lo.y, hi.y) +
						 axis(sphereC.z, lo.z, hi.z);
		return d2 <= sphereR * sphereR;
	}
	for (const Vec4& p : planes) {
		// Positive vertex: the AABB corner farthest along the plane normal. If
		// it is outside, the whole box is (conservative — no false negatives).
		const float px = p.x >= 0 ? hi.x : lo.x;
		const float py = p.y >= 0 ? hi.y : lo.y;
		const float pz = p.z >= 0 ? hi.z : lo.z;
		if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) return false;
	}
	return true;
}

void DungeonWorld::NewFrame(u32 frameIndex) {
	if (m_particleBatch) m_particleBatch->NewFrame(frameIndex);
}

bool DungeonWorld::AnimatedCasterNear(const gfx::PointLight& light) const {
	auto inReach = [&](const Vec3& c, float r) {
		const Vec3 d = Sub(c, light.position);
		const float reach = light.radius + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	};
	if (m_pillarActive && inReach({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.2f))
		return true; // sway
	for (const Monster& m : m_monsters) {
		if (!m.Alive()) continue; // downed: no animation, can't dirty a cube
		const Vec3 c = m_map.CellCenter(m.x, m.z);
		if (inReach({c.x, 1.0f, c.z}, 1.5f)) return true;
	}
	return false;
}

// Renders the cube shadow maps for every light that holds a slot this frame.
// Runs before the main pass with the shadow pipeline bound. A slot's cube is
// reused from the previous frame (left in its SRV state, the barrier guard
// skips it) unless the light changed/moved, a flicker tick is due, geometry
// changed, or an animating caster sits within the light.
void DungeonWorld::RenderShadowMaps(ID3D12GraphicsCommandList* list) {
	++m_frameCounter;
	constexpr u64 kFlickerInterval = 2;  // re-render wandering fire cubes at half rate
	constexpr float kPosEps2 = 0.0004f;  // 2 cm: a steady light re-renders once it moves
	const u32 rev = m_map.Revision();

	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& light = m_lights.points[i];
		if (light.shadowSlot < 0) continue;
		const int slot = light.shadowSlot;
		ShadowSlotCache& cache = m_shadowCache[slot];

		const Vec3 d = Sub(light.position, cache.pos);
		const bool moved = d.x * d.x + d.y * d.y + d.z * d.z > kPosEps2;
		const bool flickerDue =
			(m_frameCounter + static_cast<u64>(slot)) % kFlickerInterval == 0;
		const bool needsRender = cache.lightId != static_cast<int>(i) ||
								 cache.revision != rev || AnimatedCasterNear(light) ||
								 (light.flickerShadow ? flickerDue : moved);
		if (!needsRender) continue; // reuse the cube already bound as an SRV

		const ViewCull cull = ViewCull::FromSphere(light.position, light.radius);
		for (u32 face = 0; face < 6; ++face) {
			m_renderer.BeginShadowFace(list, static_cast<u32>(slot), face,
									   light.position, light.radius);
			SubmitSceneGeometry(list, &cull);
		}
		cache = {static_cast<int>(i), light.position, rev};
	}
	m_renderer.EndShadows(list);
	m_device.BindBackBuffer(list); // the shadow pass redirected the OM
}

void DungeonWorld::RenderScene(ID3D12GraphicsCommandList* list) {
	m_renderer.BeginScene(list, m_camera, m_lights,
						  m_dustEnabled ? m_atmosphere : gfx::Atmosphere{});
	const ViewCull cull = ViewCull::FromFrustum(m_camera.ViewProj());
	SubmitSceneGeometry(list, &cull);
	// Transparent flame/spark/smoke billboards last, over the opaque scene.
	m_particleBatch->Render(list, m_camera, m_particleScratch);
}

void DungeonWorld::SubmitSceneGeometry(ID3D12GraphicsCommandList* list,
									  const ViewCull* cull) {
	// A discrete mesh draws only if its bounding sphere passes the cull (camera
	// frustum in the main pass, light reach in the shadow pass).
	auto visible = [&](const Vec3& c, float r) {
		return !cull || cull->TestSphere(c, r);
	};

	DrawSurface(list, m_walls, cull);
	DrawSurface(list, m_floors, cull);
	DrawSurface(list, m_ceilings, cull);

	// Pillar — peacock-ore stone (bump + parallax + ORM); polished jade fallback.
	if (m_pillarActive && visible({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.8f)) {
		Mat4 pillarWorld = Mat4Identity();
		pillarWorld._41 = m_pillarPos.x;
		pillarWorld._43 = m_pillarPos.z;
		gfx::MaterialParams pillarMaterial;
		ApplyPropMaterial(pillarMaterial, m_pillarTex,
						  m_pillarModel.materials[0].baseColorFactor, 0.22f);
		m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, pillarMaterial,
							m_pillarAnimator.Palette());
	}

	// Static architecture decorations (columns, archways, fountains, ...):
	// textured stone/wood with bump + parallax + ORM, falling back to the flat
	// glTF material color if the texture set is missing.
	for (const Decoration& deco : m_decorations) {
		// radius 2.0 covers the widest prop (the archway spans the full cell).
		if (!visible({deco.world._41, 1.2f, deco.world._43}, 2.0f)) continue;
		gfx::MaterialParams material;
		material.doubleSided = !deco.kind->authored; // authored meshes back-cull
		ApplyPropMaterial(material, deco.kind->tex, deco.kind->color, 0.85f);
		m_renderer.DrawMesh(list, *deco.kind->mesh, deco.world, material);
	}

	// Rune items: the shared carved-stone tablet, drawn per element with its PBR
	// set (parallax cuts the glyph in). The tablet reads as worn stone; the
	// element glow is an AURA — a faint emissive the shader concentrates at the
	// silhouette (Fresnel) plus the pulsing point light it casts (UpdateLights),
	// rather than a strong internal glow. Both pulse on the same RunePulse.
	if (m_runeMesh) {
		for (const Item& item : m_items) {
			if (item.collected || !item.kind->isRune) continue;
			const Vec3 c = m_map.CellCenter(item.x, item.z);
			if (!visible({c.x, 0.3f, c.z}, 0.8f)) continue;
			Mat4 world = Mat4Identity();
			world._41 = c.x;
			world._43 = c.z;
			gfx::MaterialParams material;
			material.doubleSided = false; // authored slab: back-cull
			ApplyPropMaterial(material, item.kind->tex,
							  m_runeModel.materials[0].baseColorFactor, 0.85f);
			// Emissive scaled well below the old internal glow — the shader's
			// Fresnel turns it into a rim aura, and the cast light (UpdateLights,
			// same RunePulse) does the surrounding glow, so they breathe together.
			const float pulse = RunePulse(m_time, item.id);
			constexpr float kAura = 0.9f; // soft aura, not a glowing panel
			const Vec4& g = item.kind->glow;
			material.emissive = {g.x * pulse * kAura, g.y * pulse * kAura,
								 g.z * pulse * kAura};
			m_renderer.DrawMesh(list, *m_runeMesh, world, material);
		}
	}

	// Monsters: bone/bandage/slime PBR sets, bound by type name. The flat-color
	// fallback keeps the old look if a set is missing (blob glistens wetly).
	for (const Monster& monster : m_monsters) {
		if (!monster.Alive()) continue; // downed monsters don't draw
		const MonsterKind& kind = *monster.kind;
		const Vec3 pos = monster.visualPos; // glides between cells while chasing
		if (!visible({pos.x, 1.0f, pos.z}, 1.5f)) continue;
		Mat4 world;
		XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
									XMMatrixTranslation(pos.x, 0, pos.z));
		gfx::MaterialParams material;
		const float fallbackRough = kind.fallbackRoughness;
		ApplyPropMaterial(material, kind.tex,
						  kind.model.materials[0].baseColorFactor, fallbackRough);
		m_renderer.DrawMesh(list, *kind.mesh, world, material,
							monster.animator.Palette());
	}

	// Fire props: worn iron sconce + bronze brazier (bump + parallax + ORM),
	// falling back to flat metallic iron if the sets are missing.
	for (const Fire& fire : m_fires) {
		if (!visible(fire.flamePos, 1.2f)) continue;
		gfx::MaterialParams metal;
		const Vec4 fallback = fire.brazier ? m_brazierColor : m_sconceColor;
		ApplyPropMaterial(metal, fire.brazier ? m_brazierTex : m_sconceTex,
						  fallback, 0.5f);
		if (!metal.albedo) metal.metallic = 1.0f; // flat fallback reads as metal
		m_renderer.DrawMesh(list, fire.brazier ? *m_brazierMesh : *m_sconceMesh,
							fire.world, metal);
	}
}

void DungeonWorld::DrawSurface(ID3D12GraphicsCommandList* list,
							   const Surface& surface, const ViewCull* cull) {
	const Mat4 identity = Mat4Identity();
	for (const SurfaceChunk& chunk : surface.chunks) {
		if (cull && !cull->TestAABB(chunk.boundsMin, chunk.boundsMax)) continue;
		const int v = chunk.variant;
		// Surfaces always carry an albedo, so the fallback color/roughness are
		// unused here; the ORM map (when present) drives roughness/metallic.
		gfx::MaterialParams material;
		ApplyPbr(material, surface.albedo[v].get(), surface.normal[v].get(),
				 surface.mr[v].get(), surface.heightScale, {}, 0.0f);
		m_renderer.DrawMesh(list, *chunk.mesh, identity, material);
	}
}

} // namespace dungeon::game
