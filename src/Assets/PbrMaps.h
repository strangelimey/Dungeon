// ============================================================================
// Assets/PbrMaps.h — which file in a downloaded texture folder is which map.
//
// Scanned PBR sets ship as a folder of loose images with no manifest, named by
// whatever convention the source uses (Poly Haven "_diff"/"_nor_gl"/"_disp",
// FreePBR "_albedo"/"_normal-ogl", Unreal-style "T_Thing_R"/"_M", ...). The
// importer guesses each file's ROLE from its name.
//
// This lives in Assets — not in AssetBaker — because two callers need the same
// answer: the baker, which packs the maps, and the editor's asset-creation
// dialog, which reports what it found BEFORE you commit to a bake (and previews
// the albedo). A second copy of the substring table would drift the day either
// side gained a naming convention.
// ============================================================================
#pragma once

#include <string>

namespace dungeon::assets {

// Absolute paths to the maps recognised in one source folder; empty = absent.
struct PbrMapSet {
	std::string albedo, normal, height, ao, roughness, metallic, opacity;
	// The normal map's name says OpenGL convention (green up), which the engine
	// flips on import. Filename evidence only — some sources omit the token, so
	// the importer's --flip-green overrides it.
	bool normalLooksGl = false;

	// Albedo is the one map with no fallback (the importer errors without it).
	bool Usable() const { return !albedo.empty(); }
};

// Scans `sourceDir` (non-recursive) and assigns each image file a role. Files
// are visited in sorted order, so a folder with two candidates for a role picks
// the same one every time. A missing/unreadable directory yields an empty set.
PbrMapSet DiscoverPbrMaps(const std::string& sourceDir);

} // namespace dungeon::assets
