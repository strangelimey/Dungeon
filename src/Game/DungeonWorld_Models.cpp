// ============================================================================
// Game/DungeonWorld_Models.cpp - the model cache: one parse and one GPU upload
// per model FILE, however many catalog kinds use it (declarations in
// DungeonWorld.h, "THE MODEL CACHE").
//
// Why it exists: the kind caches (MonsterKindFor, ItemKindFor, DecorationKindFor,
// FixtureKindFor) key by CATALOG ID, and content deliberately shares files - six
// skeleton variants on skeleton.gltf, five armours on leather_armor.glb, every
// enchanted blade on its plain twin's mesh, marble and stone columns on one
// column.gltf. Each of those kinds used to parse the file and upload its own
// meshes and textures, so the armour's textures sat in VRAM five times over.
//
// What is shared and what is not: the parsed ModelData, meshes[0]'s gfx::Mesh,
// and a multi-material model's GPU textures and submesh geometry. NOT shared: a
// multi-material model's MATERIALS - every kind gets its own copy of the
// template (MultiMaterialModel is copyable for exactly this), because a kind may
// bake catalog overrides into them (BakeCatalogMaterial) that must not repaint
// the others. Per-kind icon targets stay per kind for the same reason.
//
// Lifetime: everything is shared_ptr, so the cache holding a reference only ever
// DELAYS a free, never causes one early. The one path that drops kinds while
// the GPU may still read them (ReloadTypeKind) drains first, as it always did.
// ============================================================================
#include "Game/AssetUtil.h"
#include "Game/DungeonWorld.h"

namespace dungeon::game {

std::shared_ptr<const assets::ModelData> DungeonWorld::ModelFile(const std::string& file) {
	CachedModel& entry = m_modelCache[file];
	if (!entry.data)
		entry.data = std::make_shared<const assets::ModelData>(LoadModelOrDie(file));
	return entry.data;
}

std::shared_ptr<gfx::Mesh> DungeonWorld::ModelMesh(const std::string& file) {
	const std::shared_ptr<const assets::ModelData> data = ModelFile(file);
	CachedModel& entry = m_modelCache[file];
	if (!entry.mesh) entry.mesh = std::make_shared<gfx::Mesh>(m_device, data->meshes[0]);
	return entry.mesh;
}

std::unique_ptr<DungeonWorld::MultiMaterialModel> DungeonWorld::ModelMulti(
	const std::string& file) {
	const std::shared_ptr<const assets::ModelData> data = ModelFile(file);
	CachedModel& entry = m_modelCache[file];
	if (!entry.multi) entry.multi = BuildMultiMaterialModel(m_device, *data);
	return std::make_unique<MultiMaterialModel>(*entry.multi);
}

void DungeonWorld::ForgetModelFile(const std::string& file) {
	m_modelCache.erase(file);
}

} // namespace dungeon::game
