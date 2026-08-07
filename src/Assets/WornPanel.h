// ============================================================================
// Assets/WornPanel.h — the worn wall panel file-naming convention.
//
// A wall surface does not bake ONE mesh. It bakes one per combination of:
//
//   * PHASE — which slice of the texture the square shows. A square scan has a
//     single phase (the whole image); a 2:1 scan has two, so consecutive
//     squares show consecutive halves and the pattern walks across the pair
//     instead of restarting at every boundary.
//   * OPEN SIDES — which of the panel's two side edges skip the pin back to the
//     flat wall plane, because the neighbour there is the same surface and the
//     two displacement fields already meet. Bit 1 = the -X edge, bit 2 = +X.
//
// The name is `worn_<set>_<tier><suffix>.gltf`, and PHASE 0 WITH BOTH SIDES
// PINNED TAKES THE BARE NAME — it is the panel every fallback lands on, and
// predates the rest, so it must keep the name the rest of the codebase knows.
//
// This lives in Assets because the baker WRITES these files and the game READS
// them, out of two separate binaries. A private copy on each side would be two
// things to keep in step, and a mismatch fails silently: the loader simply
// finds nothing and every wall quietly falls back to the pinned panel.
// ============================================================================
#pragma once

#include <string>

namespace dungeon::assets {

// The suffix for one worn wall panel. `openSides` is the bit pair above.
inline std::string WornPanelSuffix(int phase, int openSides) {
	if (phase == 0 && openSides == 0) return {};
	std::string s = "_" + std::to_string(phase);
	if (openSides & 1) s += "l";
	if (openSides & 2) s += "r";
	return s;
}

// How many phases a wall panel set may have. Only a bound for sweeping stale
// files and for the loader's probe — the real count is whatever the baker wrote,
// which is the texture's aspect rounded to whole squares.
inline constexpr int kMaxWornPhases = 4;

} // namespace dungeon::assets
