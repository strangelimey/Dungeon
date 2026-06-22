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
		// Tucked into the NW corner of the start room (one cell up-left of the
		// start) so it greets the player without standing on the cells in front
		// of the entrance.
		m_pillarPos = m_map.CellCenter(m_map.StartX() - 1, m_map.StartZ() - 1);
		// Borrow the peacock-ore stone set for its NORMAL + ORM maps only (carved
		// micro-relief + roughness variation, so the pillar reads as stone, not a
		// smooth wet tube). The albedo is discarded in DrawProps — that texture is
		// purple; the jade color comes from the model's flat baseColorFactor.
		m_pillarTex = LoadPropTextures("pillar");
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
			assets->iq = def->GetFloat("iq", 100.0f);
			assets->facesTarget = def->GetBool("faces", true);
			assets->fallbackRoughness = def->GetFloat("roughness", 0.9f);
			assets->size = ParseSizeClass(CatalogGet(def, "size", "large"));
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
		kind->weight = def ? def->GetFloat("weight", 0.0f) : 0.0f;
		// `command` is a free-form list (whitespace/comma separated) of command ids
		// the hand right-click menu offers; runes implicitly gain "memorize" below.
		for (const std::string& cmd : SplitTokens(CatalogGet(def, "command", "")))
			kind->commands.push_back(cmd);
		// Placeholder look: non-rune items reuse the tablet mesh tinted by category
		// (runes overwrite this with their element colour just below).
		kind->glow = CategoryTint(kind->category);
		// Every item draws as the shared carved-stone tablet (loaded once) — runes
		// carve their element's set in; other categories ride the flat tint above.
		if (!m_runeMesh) {
			m_runeModel = LoadModelOrDie("rune_tablet.gltf");
			m_runeMesh = std::make_unique<gfx::Mesh>(m_device, m_runeModel.meshes[0]);
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
		it = m_itemKinds.emplace(type, std::move(kind)).first;
	}
	return *it->second;
}

void DungeonWorld::LoadItems() {
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Item) continue;
		ItemKind& kind = ItemKindFor(spawn.type);
		// Baseline items have no authored slot — fan multiples in a cell out across
		// quarters (target = cell centre, so it fills by quarter index, fill order).
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
		const Vec3 c = SlotCenter(item.x, item.z, SizeClass::Medium, item.slot);
		float cx, cy, tx, ty;
		if (!project({c.x, 0.23f, c.z}, cx, cy)) continue; // tablet centre
		// Sample the top of the rendered tablet for the hit radius — non-rune
		// placeholders are drawn scaled up (kItemPlaceholderScale) and stand taller,
		// so they take a correspondingly taller sample and a larger click target.
		const float topY = item.kind->isRune ? 0.46f : kItemPickTopY;
		project({c.x, topY, c.z}, tx, ty);
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
	// Desired drop point in world space — used to pick the nearest quarter slot.
	// Defaults to the fallback cell's centre (feet); a floor hit overrides it.
	Vec3 feet = m_map.CellCenter(cx, cz);
	float wx = feet.x, wz = feet.z;
	const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
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
	m_shadows.InvalidateCubes();
	log::Info("Quality switched to {} ({} meshes, {} textures)",
			  m_settings.QualityLabel(), m_settings.MeshSuffix(),
			  m_settings.TextureSuffix());
}
} // namespace dungeon::game
