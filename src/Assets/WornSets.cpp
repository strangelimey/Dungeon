// ============================================================================
// Assets/WornSets.cpp - the worn-block bake record of every shipped surface set
// (see WornSets.h).
// ============================================================================
#include "WornSets.h"

#include <algorithm>
#include <iterator>

namespace dungeon::assets {

namespace {

constexpr WornKind kW = WornKind::Wall;
constexpr WornKind kF = WornKind::Floor;
constexpr WornKind kC = WornKind::Ceiling;

// The order is `AssetBaker models`' bake order. Each set belongs to exactly one
// surface kind, so its worn_<set>_<tier>.gltf is unambiguous.
constexpr WornSet kShipped[] = {
	{"wall_brick", kW, 0.060f, 911u},   {"wall_stone", kW, 0.055f, 921u},
	{"wall_moss", kW, 0.040f, 931u},    {"floor_slabs", kF, 0.050f, 941u},
	{"floor_cobble", kF, 0.045f, 951u}, {"ceiling_rough", kC, 0.100f, 961u},
	{"ceiling_cracked", kC, 0.080f, 971u},
	// Scanned textures.com sets. Polished marble ships no height map, so it
	// bakes PROCEDURAL wear (as does wall_stone, whose export is flat); for those
	// the relief is the reference a type's own `relief` is measured against - a
	// type at 0.040 bakes marble's procedural field twice as deep.
	{"cobblestone_wall", kW, 0.060f, 981u}, {"stacked_stone", kW, 0.050f, 991u},
	{"brick_red", kW, 0.055f, 1001u},       {"plaster", kW, 0.030f, 1011u},
	{"rock_cliff", kW, 0.070f, 1021u},      {"marble_white", kW, 0.020f, 1031u},
	{"cobblestone_floor", kF, 0.050f, 1041u}, {"broken_tile", kF, 0.040f, 1051u},
	{"rubble", kF, 0.060f, 1061u},          {"rock_smooth", kF, 0.045f, 1071u},
	{"limestone", kC, 0.080f, 1081u},

	// --- batch 2 (2026-08-03): 36 scanned sets -------------------------------
	// Relief is chosen by what the stone DOES, not by a flat per-kind default:
	// round cobbles stand proud, dressed temple ashlar barely moves, and a
	// carved wall keeps its detail in the map rather than the mesh (displacing
	// it would smear the carving). Floors sit LOWER than the walls throughout -
	// relief you walk over reads as lumpy long before the same amplitude looks
	// wrong on a wall - and ceilings sit highest, since nothing ever gets close
	// enough to betray the silhouette.
	{"wall_cobble_mixed", kW, 0.070f, 1101u},
	{"wall_cobble_mixed4", kW, 0.065f, 1111u},
	{"wall_cobble_mossy", kW, 0.070f, 1121u},
	{"wall_cobble_round", kW, 0.075f, 1131u},
	{"wall_stone_plain", kW, 0.055f, 1141u},
	{"wall_stone_granite", kW, 0.050f, 1151u},
	{"wall_stone_28", kW, 0.060f, 1161u},
	{"wall_stone_30", kW, 0.060f, 1171u},
	{"wall_stone_34", kW, 0.055f, 1181u},
	{"wall_brick_weathered", kW, 0.050f, 1191u},
	{"wall_brick_coarse", kW, 0.055f, 1201u},
	{"wall_brick_plaster", kW, 0.040f, 1211u}, // plaster skins the courses
	{"wall_brick_distorted", kW, 0.050f, 1221u},
	{"wall_brick_old", kW, 0.045f, 1231u},
	{"wall_sandstone_blocks", kW, 0.045f, 1241u},
	{"wall_sandstone_block2", kW, 0.045f, 1251u},
	{"wall_temple_sandstone", kW, 0.040f, 1261u}, // dressed: nearly flush
	{"wall_temple_ancient", kW, 0.045f, 1271u},
	{"wall_carved", kW, 0.035f, 1281u},        // keep the carving in the map
	{"floor_medieval", kF, 0.045f, 1291u},
	{"floor_cobble_path", kF, 0.050f, 1301u},
	{"floor_cobble_medieval", kF, 0.050f, 1311u},
	{"floor_cobble_mossy", kF, 0.050f, 1321u},
	{"floor_stone_pavement", kF, 0.040f, 1331u},
	{"floor_temple", kF, 0.035f, 1341u},
	{"floor_ancient_stone", kF, 0.040f, 1351u},
	{"floor_paving_mossy", kF, 0.050f, 1361u},
	{"floor_slate", kF, 0.035f, 1371u},
	// A stair TREAD surface first, but worn as a floor too so the brush can
	// place it - the mesh can still bind the plain texture either way.
	{"floor_stairs", kF, 0.045f, 1381u},
	{"ground_rockbed", kF, 0.055f, 1391u},
	{"ground_soil_dusty", kF, 0.030f, 1401u}, // soil slumps; it does not jut
	{"ground_soil_rocky", kF, 0.050f, 1411u},
	{"ground_gravel", kF, 0.050f, 1421u},
	{"ceiling_rock", kC, 0.100f, 1431u},
	{"ceiling_rock_layered", kC, 0.090f, 1441u},
	{"ceiling_rock_porous", kC, 0.070f, 1451u},
	// The three wood sets are PROP textures (door panel, crate, barrel), not
	// cell surfaces, so they get no worn block: a worn mesh would commit them to
	// one surface kind for nothing.
};

// FNV-1a, 32-bit: a fixed function of the name, so a derived seed is the same
// on every compiler and every run.
u32 NameSeed(std::string_view name) {
	u32 h = 2166136261u;
	for (const char c : name) {
		h ^= static_cast<u8>(c);
		h *= 16777619u;
	}
	return h | 1u;
}

} // namespace

std::span<const WornSet> ShippedWornSets() { return kShipped; }

const WornSet* FindShippedWornSet(std::string_view texture) {
	const auto it = std::find_if(std::begin(kShipped), std::end(kShipped),
								 [&](const WornSet& s) { return s.texture == texture; });
	return it == std::end(kShipped) ? nullptr : &*it;
}

float DefaultWornRelief(WornKind kind) {
	// A rough ceiling hangs further down than a floor stands proud, and a floor
	// sits lower than a wall (see the batch-2 note above).
	switch (kind) {
	case WornKind::Floor: return 0.045f;
	case WornKind::Ceiling: return 0.08f;
	case WornKind::Wall: break;
	}
	return 0.055f;
}

WornSet WornSetFor(std::string_view texture, WornKind kind) {
	if (const WornSet* shipped = FindShippedWornSet(texture)) return *shipped;
	return {texture, kind, DefaultWornRelief(kind), NameSeed(texture)};
}

std::optional<WornKind> ParseWornKind(std::string_view name) {
	if (name == "wall") return WornKind::Wall;
	if (name == "floor") return WornKind::Floor;
	if (name == "ceiling") return WornKind::Ceiling;
	return std::nullopt;
}

std::string_view WornKindName(WornKind kind) {
	switch (kind) {
	case WornKind::Floor: return "floor";
	case WornKind::Ceiling: return "ceiling";
	case WornKind::Wall: break;
	}
	return "wall";
}

} // namespace dungeon::assets
