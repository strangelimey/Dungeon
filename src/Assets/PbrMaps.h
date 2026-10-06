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
#include <string_view>

namespace dungeon::assets {

// Absolute paths to the maps recognised in one source folder; empty = absent.
struct PbrMapSet {
	std::string albedo, normal, height, ao, roughness, metallic, opacity;
	// The normal map's name says OpenGL convention (green up), which the engine
	// flips on import (NormalNameLooksGl). Filename evidence only - some sources
	// omit the token and some names hold "gl" for other reasons - so the
	// importer's --flip-green / --no-flip-green overrides it either way.
	bool normalLooksGl = false;

	// Albedo is the one map with no fallback (the importer errors without it).
	bool Usable() const { return !albedo.empty(); }
};

// Scans `sourceDir` (non-recursive) and assigns each image file a role. Files
// are visited in sorted order, so a folder with two candidates for a role picks
// the same one every time. A missing/unreadable directory yields an empty set.
PbrMapSet DiscoverPbrMaps(const std::string& sourceDir);

// Whether a normal map's file STEM names the OpenGL convention: its LAST token
// - the stem split on '_', '-', '.' and spaces, a trailing resolution tag (1k,
// 2k, 4k, 8k) set aside - ends in "gl", case ignored. So Poly Haven's
// "_nor_gl_2k", FreePBR's "_normal-ogl" and ambientCG's "_NormalGL" read as
// GL, while a "gl" inside a word does not: "jungle_normal", "glossy-tile_
// normal-dx", "semigloss-normal". It used to be any "gl" anywhere in the name
// (code-review C393), which flipped a DirectX map whose SET was named, say,
// angled-tiled-floor. Pure, so tools/RollTest checks it.
bool NormalNameLooksGl(std::string_view stem);

} // namespace dungeon::assets
