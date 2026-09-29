// ============================================================================
// Game/DungeonWorld_Editing.cpp - split out of DungeonWorld.cpp to keep files
// small (see DungeonWorld.h). Holds the editor's cell/variant edits, surface
// palette membership, type rename/delete sweeps, placement and the targeted
// removals. Split further 2026-09-29 by concern: doors/buttons/niches ->
// DungeonWorld_Doors.cpp; stair pairs, remote (browsed-level) editing and
// map markers -> DungeonWorld_Remote.cpp; level load/stash/serialize ->
// DungeonWorld_LevelIO.cpp; editor undo/redo -> DungeonWorld_Undo.cpp.
// (Saving every level at once, renaming and deleting levels:
// DungeonWorld_Levels.cpp.)
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cstdlib> // atof — the .ent `seconds=` override
#include <filesystem>
#include <format>
#include <optional>

using namespace DirectX;

namespace dungeon::game {
void DungeonWorld::EditCell(int x, int z, Cell cell) {
	const u32 rev = m_map.Revision();
	m_map.SetCell(x, z, cell);
	if (m_map.Revision() == rev) return; // unchanged
	// A structural edit can strand fixtures (buried under a painted wall, or a
	// sconce whose mount wall just opened up) — prune them with the cell, or the
	// saved .map would assert on its next load.
	if (m_map.PruneFixturesForCell(x, z)) RebuildFiresAndDust();
	// Same for the dynamic layer and decorations: nothing may keep standing on
	// (or mounting against) the repainted cell — SaveLevel and the level stash
	// persist live state, and the loaders reject records contradicting the grid.
	PruneEntitiesForCell(x, z);
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

void DungeonWorld::PruneEntitiesForCell(int x, int z) {
	if (!m_map.IsWalkable(x, z)) {
		// Cell painted solid: nothing can stand on it any more. Stairs go through
		// RemoveStairAt so the paired return record on the other level dies too;
		// that also erases the stair prop, leaving plain decorations to the sweep.
		if (m_map.StairAt(x, z)) RemoveStairAt(x, z);
		std::erase_if(m_monsters,
					  [&](const Monster& m) { return m.x == x && m.z == z; });
		std::erase_if(m_items, [&](const Item& i) { return i.x == x && i.z == z; });
		std::erase_if(m_buttons,
					  [&](const Button& b) { return b.x == x && b.z == z; });
		std::erase_if(m_decorations,
					  [&](const Decoration& d) { return d.x == x && d.z == z; });
		std::erase_if(m_doors, [&](const Door& d) { return d.x == x && d.z == z; });
		// The .ent baseline records under the new wall. Record edits diverge
		// m_entities from the file on disk — flag it so a level swap stashes
		// them (see BeginLevelLoad) instead of re-parsing the stale file.
		if (m_entities.RemoveAt(x, z) > 0) m_entsDirty = true;
		return;
	}

	// Cell painted open: buttons and wall decorations in the neighbouring cells
	// that mounted on it hang on air now. Re-mount each on a solid wall of its
	// own cell, else drop it (the sconce treatment in PruneFixturesForCell).
	auto solidWall = [&](int cx, int cz, Direction& out) {
		constexpr Direction kScan[4] = {Direction::North, Direction::East,
										Direction::South, Direction::West};
		for (const Direction d : kScan)
			if (!m_map.IsWalkable(cx + DirDX(d), cz + DirDZ(d))) {
				out = d;
				return true;
			}
		return false;
	};
	for (size_t i = m_buttons.size(); i-- > 0;) {
		Button& b = m_buttons[i];
		if (b.x + DirDX(b.facing) != x || b.z + DirDZ(b.facing) != z) continue;
		Direction d;
		if (solidWall(b.x, b.z, d)) {
			b.facing = d;
			// The writer emits buttons from their .ent record, so re-face it too.
			if (Entity* e = m_entities.MutableById(b.id)) e->facing = d;
			m_entsDirty = true;
		} else {
			m_entities.RemoveById(b.id);
			m_buttons.erase(m_buttons.begin() + static_cast<ptrdiff_t>(i));
			m_entsDirty = true;
		}
	}
	for (size_t i = m_decorations.size(); i-- > 0;) {
		Decoration& deco = m_decorations[i];
		if (!deco.wallMounted) continue;
		if (deco.x + DirDX(deco.wall) != x || deco.z + DirDZ(deco.wall) != z)
			continue;
		Direction d;
		if (solidWall(deco.x, deco.z, d)) {
			deco.wall = d;
			const WallMount m = MountOnWall(deco.x, deco.z, d);
			XMStoreFloat4x4(&deco.world,
							UnitScale(deco.kind->modelScale) * XMMatrixRotationY(m.yaw) *
								XMMatrixTranslation(m.pos.x, 0, m.pos.z));
		} else {
			m_decorations.erase(m_decorations.begin() + static_cast<ptrdiff_t>(i));
		}
	}
}

void DungeonWorld::EditVariant(int x, int z, SurfaceSel sel, int variant) {
	// Wall variants live on the SOLID cell (the block owns its texture — all
	// four faces); floor/ceiling variants on the floor cell they surface. A
	// paint on the wrong cell type is a no-op.
	if ((sel == SurfaceSel::Wall) == m_map.IsWalkable(x, z)) return;
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

const gfx::Texture* DungeonWorld::SurfaceAlbedoForId(SurfaceSel sel,
													 const std::string& id) const {
	// The loaded albedo arrays sit in ACTIVE-palette order (LoadTextureSet), so
	// finding the id's palette index finds its texture.
	const std::vector<std::string>& pal = sel == SurfaceSel::Wall	 ? m_map.WallPalette()
										  : sel == SurfaceSel::Floor ? m_map.FloorPalette()
																	 : m_map.CeilingPalette();
	const Surface& surface = sel == SurfaceSel::Wall	? m_walls
							 : sel == SurfaceSel::Floor ? m_floors
														: m_ceilings;
	for (size_t i = 0; i < pal.size() && i < surface.albedo.size(); ++i)
		if (pal[i] == id) return surface.albedo[i].get();
	return nullptr;
}

// --- surface palette membership (editor) ------------------------------------

const Catalog& DungeonWorld::SurfaceCatalog(SurfaceSel sel) const {
	return sel == SurfaceSel::Wall	  ? m_project.walls
		   : sel == SurfaceSel::Floor ? m_project.floors
									  : m_project.ceilings;
}

bool DungeonWorld::SurfaceAssetsAvailable(SurfaceSel sel,
										  const std::string& id) const {
	const CatalogEntry* def = SurfaceCatalog(sel).Find(id);
	if (!def) return false;
	const std::string set = CatalogGet(def, "texture", id);
	// A texture stem resolves as .dds (baked) else .png (source) — TryLoadTextureFile's
	// order.
	const auto textureAt = [](const std::string& stem) {
		return std::filesystem::exists(paths::Asset("textures\\" + stem + ".dds")) ||
			   std::filesystem::exists(paths::Asset("textures\\" + stem + ".png"));
	};
	// The worn block mesh is baked per texture set AND per mesh tier, and its
	// load is a LoadModelOrDie — a missing one would abort, so it gates first.
	if (!std::filesystem::exists(paths::Asset(
			std::format("models\\worn_{}_{}.gltf", set, m_settings.MeshSuffix()))))
		return false;
	// The tier's texture resolution may be uninstalled; LoadPbrSet falls back
	// to the always-present 2k set, so either satisfies the load.
	return textureAt(std::format("{}_{}", set, m_settings.TextureSuffix())) ||
		   textureAt(set + "_2k");
}

bool DungeonWorld::AddPaletteEntry(SurfaceSel sel, const std::string& id) {
	if (!SurfaceAssetsAvailable(sel, id)) return false;
	const bool added = sel == SurfaceSel::Wall	  ? m_map.AddToWallPalette(id)
					   : sel == SurfaceSel::Floor ? m_map.AddToFloorPalette(id)
												  : m_map.AddToCeilingPalette(id);
	if (!added) return false; // unknown/duplicate: nothing to load
	// The palette IS the variant order, so the loaded texture arrays and worn
	// block meshes must grow with it before anything can paint the new index.
	// The full reload is the quality-swap path (drains the GPU, re-resolves,
	// reloads both, rebuilds the chunks) — an interactive edit, not per-frame.
	ReloadDungeonBlocks(/*textureResChanged*/ true);
	return true;
}

void DungeonWorld::ReloadTypeKind(const std::string& catalogKey,
								  const std::string& id) {
	// The caches are keyed by catalog id and hold GPU resources the live objects
	// point INTO, so drop the entry only after the instances are gone — the
	// respawn below rebuilds both. Draining first: in-flight frames may still
	// reference the mesh/textures we are about to free.
	m_device.WaitIdle();
	m_monsters.clear(); // they hold MonsterKind pointers
	m_items.clear();
	m_buttons.clear();
	m_doors.clear();
	m_decorations.clear();
	if (catalogKey == "monsters") m_monsterKinds.erase(id);
	else if (catalogKey == "fixtures") m_fixtureKinds.erase(id);
	else m_decorationKinds.erase(id); // decorations/doors/buttons/stairs/items
	// The rebuilt kind reads its model file fresh, as it did when every kind
	// parsed its own copy - so a file changed on disk shows on save. Other kinds
	// sharing the file keep the copy they hold until they are reloaded themselves.
	if (const Catalog* cat = m_project.CatalogForKey(catalogKey)) {
		const CatalogEntry* def = cat->Find(id);
		for (const std::string& model : {CatalogGet(def, "model", id),
										 CatalogGet(def, "part2_model", "")})
			if (!model.empty()) {
				ForgetModelFile(model + ".gltf");
				ForgetModelFile(model + ".glb");
			}
	}
	// Fixtures are props AND light sources, so their rebuild goes through the
	// fire/turbidity path; everything else just re-spawns.
	RespawnFromRecords(catalogKey == "wallfeatures");
	RebuildFiresAndDust();
}

// --- type rename / delete (editor) ------------------------------------------

DungeonWorld::TypeUsage DungeonWorld::SweepTypeRefs(const std::string& catalogKey,
													const std::string& id,
													const std::string* newId) {
	TypeUsage usage;
	// Which record family holds this category's ids. A category the static and
	// dynamic layers both ignore (none today) would simply find nothing.
	using TR = DungeonMap::TypeRecords;
	std::optional<TR> statics;
	std::optional<EntityKind> dynamics;
	if (catalogKey == "walls") statics = TR::WallPalette;
	else if (catalogKey == "floors") statics = TR::FloorPalette;
	else if (catalogKey == "ceilings") statics = TR::CeilingPalette;
	else if (catalogKey == "decorations") statics = TR::Decoration;
	else if (catalogKey == "fixtures") statics = TR::Fixture;
	else if (catalogKey == "wallfeatures") statics = TR::WallFeature;
	else if (catalogKey == "stairs") statics = TR::Stair;
	else if (catalogKey == "monsters") dynamics = EntityKind::Monster;
	// Weapons and armor place as Item entities too, so a rename/delete of one
	// sweeps the same .ent record family.
	else if (catalogKey == "items" || catalogKey == "weapons" ||
			 catalogKey == "armor")
		dynamics = EntityKind::Item;
	else if (catalogKey == "buttons") dynamics = EntityKind::Button;
	else if (catalogKey == "doors") dynamics = EntityKind::Door;
	if (!statics && !dynamics) return usage;

	// The ACTIVE level's decorations live as instances, not records — sync them
	// back first (the stash/save rule) so the sweep sees the truth and the
	// respawn afterwards reads what we wrote.
	if (statics == TR::Decoration && newId)
		m_map.SetDecorationRecords(LiveDecorationRecords());

	for (const std::string& stem : m_project.levels) {
		const bool active = stem == m_currentLevel;
		int hits = 0;
		if (statics) {
			// A level not in memory is parsed on demand — the same lazy stash
			// the map overlay uses to edit a level it isn't standing on.
			DungeonMap& map = active ? m_map : EnsureMapStash(stem);
			hits += map.SweepTypeRefs(*statics, id, newId);
		}
		if (dynamics) {
			DungeonEntities& ents = active ? m_entities : EnsureEntStash(stem);
			const int n = ents.SweepTypeRefs(*dynamics, id, newId);
			hits += n;
			// The active level's records diverge from its file once touched;
			// the writer only rewrites a .ent it knows is dirty.
			if (n > 0 && newId && active) m_entsDirty = true;
		}
		if (hits > 0) {
			usage.count += hits;
			usage.levels.push_back(stem);
		}
	}
	return usage;
}

bool DungeonWorld::AddPaletteEntryRemote(const std::string& stem, SurfaceSel sel,
										 const std::string& id) {
	if (!SurfaceCatalog(sel).Contains(id)) return false;
	// No texture/mesh work: a browsed level isn't rendered in 3D. Its assets are
	// checked when the party (or the editor) enters it.
	DungeonMap& map = EnsureMapStash(stem);
	return sel == SurfaceSel::Wall	  ? map.AddToWallPalette(id)
		   : sel == SurfaceSel::Floor ? map.AddToFloorPalette(id)
									  : map.AddToCeilingPalette(id);
}

int DungeonWorld::EnsureSurfaceVariant(const std::string& stem, SurfaceSel sel,
									   const std::string& id) {
	const auto indexIn = [&](const DungeonMap& map) -> int {
		const std::vector<std::string>& pal = sel == SurfaceSel::Wall	? map.WallPalette()
											  : sel == SurfaceSel::Floor ? map.FloorPalette()
																		 : map.CeilingPalette();
		const auto it = std::find(pal.begin(), pal.end(), id);
		return it == pal.end() ? -1 : static_cast<int>(it - pal.begin());
	};
	if (stem == m_currentLevel) {
		if (const int i = indexIn(m_map); i >= 0) return i;
		if (!AddPaletteEntry(sel, id)) return -1; // assets missing / unknown
		return indexIn(m_map);
	}
	// A browsed level: the stash is truth (ViewedMap is a snapshot copy that
	// only refreshes after the paint, so read the stash directly here).
	if (const int i = indexIn(EnsureMapStash(stem)); i >= 0) return i;
	if (!AddPaletteEntryRemote(stem, sel, id)) return -1;
	return indexIn(EnsureMapStash(stem));
}

int DungeonWorld::EnsureComboVariant(const std::string& stem, const std::string& id) {
	const CatalogEntry* def = m_project.combos.Find(id);
	if (!def) return -1;
	const ComboMembers members = ComboMembersOf(*def);
	// Members FIRST: a slot resolves its members against the palette, and only
	// palette entries have textures loaded. A member whose assets are missing is
	// simply left out of the palette, and ComboMembersOf skips what is absent.
	for (int s = 0; s < 3; ++s)
		for (const std::string& member : members[static_cast<size_t>(s)])
			EnsureSurfaceVariant(stem, static_cast<SurfaceSel>(s), member);
	DungeonMap& map = stem == m_currentLevel ? m_map : EnsureMapStash(stem);
	return DungeonMap::ComboVariant(map.ComboSlot(id, members));
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
	XMStoreFloat4x4(&deco.world, UnitScale(kind.modelScale) * XMMatrixRotationY(DirYaw(facing)) *
									 XMMatrixTranslation(pos.x, 0, pos.z));
	deco.solid = kind.solidDefault;
	m_decorations.push_back(std::move(deco));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddWallDecoration(const std::string& type, int x, int z,
									 Direction wall) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (m_map.IsWalkable(x + DirDX(wall), z + DirDZ(wall))) return false; // nothing to hang on
	if (!m_project.decorations.Contains(type)) return false;
	DecorationKind& kind = DecorationKindFor(type, m_project.decorations);
	Decoration deco;
	deco.kind = &kind;
	deco.x = x;
	deco.z = z;
	deco.facing = wall;
	deco.wallMounted = true; // written back as the `wall=` record param
	deco.wall = wall;
	// Offset to the wall face and turned to look into the room — the same mount
	// helper the sconces use, so hung props line up with them.
	const WallMount m = MountOnWall(x, z, wall);
	XMStoreFloat4x4(&deco.world, UnitScale(kind.modelScale) * XMMatrixRotationY(m.yaw) *
									 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
	deco.solid = false; // it's on the wall — the floor stays walkable
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
	const CatalogEntry* def = m_project.fixtures.Find(type);
	// A flameless kind (empty bowl) places UNLIT so the map's turbidity grid —
	// which only smokes lit fixtures — stays truthful in the record too.
	const bool lit = CatalogBool(def, "flame", true);
	const bool ok = CatalogGet(def, "mount", "floor") == "wall"
						? m_map.AddSconce(x, z, type, lit)
						: m_map.AddBrazier(x, z, type, lit);
	if (!ok) return false;
	RebuildFiresAndDust(); // lights/flame/smoke pick the new fire up next frame
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddFixture(const std::string& type, int x, int z,
							  Direction wall) {
	if (!m_map.IsWalkable(x, z)) return false;
	const CatalogEntry* def = m_project.fixtures.Find(type);
	// Only a `mount = wall` kind has a face to hang on; a floor kind (brazier)
	// ignores the pick and stands at the cell centre as usual.
	if (CatalogGet(def, "mount", "floor") != "wall") return AddFixture(type, x, z);
	if (!m_map.AddSconce(x, z, type, CatalogBool(def, "flame", true), wall))
		return false;
	RebuildFiresAndDust();
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddNiche(const std::string& type, int x, int z) {
	if (!m_map.AddNiche(x, z, type)) return false; // no free solid wall
	RebuildChunksAround(x, z); // re-stamp the cell's wall panel as the niche
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddNiche(const std::string& type, int x, int z, Direction wall) {
	if (!m_map.AddNiche(x, z, type, wall)) return false; // not solid, or face taken
	RebuildChunksAround(x, z); // re-stamp that face's wall panel as the niche
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::RemountNiche(int x, int z, Direction from, Direction to) {
	if (from == to) return true;
	if (!m_map.SetNicheWall(x, z, from, to)) return false;
	// Treasure in the pocket is keyed by the wall it sits in, so it has to travel
	// with the niche — otherwise it would be stranded on a face with no niche
	// (invisible and unpickable, since niche items only draw while their niche is
	// open). The live item AND its .ent record both move; a runtime drop (id < 0)
	// has no record to keep in step.
	static const char* kDirName[4] = {"north", "east", "south", "west"}; // Direction order
	for (Item& it : m_items) {
		if (it.x != x || it.z != z || it.niche != static_cast<int>(from)) continue;
		it.niche = static_cast<int>(to);
		if (Entity* rec = m_entities.MutableById(it.id)) {
			bool set = false;
			for (auto& p : rec->params)
				if (p.first == "niche") {
					p.second = kDirName[static_cast<int>(to)];
					set = true;
					break;
				}
			if (!set) rec->params.emplace_back("niche", kDirName[static_cast<int>(to)]);
			m_entsDirty = true;
		}
	}
	RebuildChunksAround(x, z); // old face becomes plain wall, new face the niche
	return true;
}

bool DungeonWorld::RemoveNicheAtFace(int x, int z, Direction wall) {
	if (!m_map.RemoveNiche(x, z, wall)) return false;
	RebuildChunksAround(x, z); // re-stamp that face as a plain wall panel again
	return true;
}

bool DungeonWorld::RemoveNicheAtWall(int wx, int wz) {
	if (!m_map.RemoveNicheFacingWall(wx, wz)) return false;
	RebuildChunksAround(wx, wz); // covers the adjacent floor cell's chunk too
	return true;
}

bool DungeonWorld::AddSurfaceFeature(const std::string& type, int x, int z) {
	// The TYPE names the surface, so no caller has to thread it through.
	if (!m_map.AddFeature(x, z, type, FeatureIsCeiling(type))) return false;
	RebuildChunksAround(x, z); // re-stamp that surface's block as the feature
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::RemoveFeatureAt(int x, int z) {
	if (!m_map.RemoveAnyFeature(x, z)) return false;
	RebuildChunksAround(x, z); // the plain block comes back
	return true;
}

bool DungeonWorld::AddBore(const std::string& type, int x, int z) {
	if (!m_map.AddBore(type, x, z)) return false;
	RebuildChunksAround(x, z); // re-stamps the two flanking floor cells' faces
	return true;
}

bool DungeonWorld::AddBore(const std::string& type, int x, int z, int axis) {
	if (!m_map.AddBore(type, x, z, axis)) return false;
	RebuildChunksAround(x, z); // re-stamps the two flanking floor cells' faces
	return true;
}

bool DungeonWorld::RemoveFixtureAtFace(int x, int z, Direction wall) {
	if (!m_map.RemoveSconceAt(x, z, wall)) return false;
	RebuildFiresAndDust(); // the light/flame/smoke instance goes with it
	return true;
}

bool DungeonWorld::RemoveBoreAt(int x, int z) {
	if (!m_map.RemoveBoreAt(x, z)) return false;
	RebuildChunksAround(x, z);
	return true;
}

bool DungeonWorld::WallSeeThrough(int x, int z, int axis) const {
	// Authored bores only — the Sight spells are a shader peephole, not a hole
	// in the world (see the declaration in DungeonWorld.h).
	return m_map.WallBoredAlong(x, z, axis);
}

bool DungeonWorld::RemoveEntityAt(int x, int z) {
	for (auto it = m_monsters.begin(); it != m_monsters.end(); ++it)
		if (it->x == x && it->z == z) {
			m_monsters.erase(it);
			return true;
		}
	for (auto it = m_doors.begin(); it != m_doors.end(); ++it)
		if (it->x == x && it->z == z) {
			// Doors are record-backed: the .ent record goes with the instance.
			m_entities.RemoveById(it->id);
			m_entsDirty = true;
			m_doors.erase(it);
			return true;
		}
	for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it)
		if (it->x == x && it->z == z) {
			m_entities.RemoveById(it->id); // record-backed, like doors
			m_entsDirty = true;
			m_buttons.erase(it);
			return true;
		}
	for (auto it = m_decorations.begin(); it != m_decorations.end(); ++it)
		if (it->x == x && it->z == z && !it->stair) { // stairs: RemoveStairAt only
			m_decorations.erase(it);
			return true;
		}
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
		if (!it->collected && it->x == x && it->z == z) {
			// Record-backed when authored (id >= 0); a session-dropped tablet
			// (id < 0) has no record to remove.
			if (it->id >= 0) {
				m_entities.RemoveById(it->id);
				m_entsDirty = true;
			}
			m_items.erase(it);
			return true;
		}
	return false;
}

// --- targeted removals, for the instance inspectors' Delete button ----------
// RemoveEntityAt above is a LADDER: it takes whatever it finds FIRST on the
// cell. That is right for the middle-click erase tool, where the user points at
// a square, and WRONG for a dialog, where they are looking at one specific
// object — press Delete in the door inspector on a cell that also holds a
// monster and the ladder would take the monster. These take exactly what was
// inspected, like RemoveDecorationByIndex and RemoveItemById already do.

// Keyed by the STABLE runtimeId rather than the cell, because monsters MOVE:
// the editor need not be paused, so a cell lookup could resolve to a different
// monster than the one the dialog was opened on.
bool DungeonWorld::RemoveMonsterByRuntimeId(u32 runtimeId) {
	if (runtimeId == 0) return false;
	for (auto it = m_monsters.begin(); it != m_monsters.end(); ++it)
		if (it->runtimeId == runtimeId) {
			m_monsters.erase(it);
			return true;
		}
	return false;
}

bool DungeonWorld::RemoveDoorAt(int x, int z) {
	for (auto it = m_doors.begin(); it != m_doors.end(); ++it)
		if (it->x == x && it->z == z) {
			m_entities.RemoveById(it->id); // record-backed, like the ladder's rung
			m_entsDirty = true;
			m_doors.erase(it);
			return true;
		}
	return false;
}

bool DungeonWorld::RemoveButtonAt(int x, int z) {
	for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it)
		if (it->x == x && it->z == z) {
			m_entities.RemoveById(it->id);
			m_entsDirty = true;
			m_buttons.erase(it);
			return true;
		}
	return false;
}

} // namespace dungeon::game
