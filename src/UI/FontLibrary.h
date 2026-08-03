// ============================================================================
// UI/FontLibrary.h — the shared home for typefaces, addressed by ROLE.
//
// Before this existed every ui::Font loaded its own copy of a .ttf and owned
// its own atlas; the game had 26 of them, all silently falling back to
// Consolas. The library fixes both halves:
//
//   FACE SHARING   — the bytes of a typeface (ui::FaceData) are loaded once per
//                    PATH and shared by every Font drawn from them. A face is
//                    immutable once loaded, so this is free.
//   (FACE, SIZE)   — Get(role, px) vends the ONE Font for that face at that
//     KEYING         rounded pixel height. Two roles pointing at the same file
//                    at the same size share an atlas.
//
// The keying is not a tidiness measure. Fonts re-raster every cached glyph in
// SetHeight and upload in Commit, which calls WaitIdle — so if two owners
// shared one Font at different sizes they would re-bake each other every frame
// and drain the GPU every frame. Keying by size makes that impossible: a
// different size is a different Font.
//
// ROLES, NOT FILENAMES. Layout and content name a role; which file a role
// resolves to is data (assets/fonts/fonts.cat). See docs/fonts.md.
//
// The library does NOT parse fonts.cat: Catalog/Serialize live in the Game lib,
// which sits ABOVE UI. Game reads the catalog and calls SetFace — the same
// split as DungeonMap taking FixtureTypes because the map has no catalog
// access.
//
// OPTICAL SCALE. Faces differ ~30% in x-height at the same em size (Petit
// Formal Script 579 vs IM Fell italic 445 per 1000 upem), so swapping a face
// would otherwise look like the layout broke. Each role carries a `scale` that
// Get() folds in, so callers pass their authored design size and never
// multiply it by hand.
//
// LIFETIME. Fonts are never evicted, and Get returns a reference that stays
// valid for the library's life — owners may cache it. The population is
// therefore bounded by the number of DISTINCT (face, integer size) pairs ever
// asked for, so callers must pass settled sizes rather than a value that moves
// every frame (GameUI::UpdateFonts already debounces window resizes for exactly
// this reason). Runaway churn is not silent: the library warns once past
// kFontCountWarn live fonts.
// ============================================================================
#pragma once

#include "UI/Font.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::ui {

// What a text surface IS, independent of which typeface currently serves it.
enum class FontRole {
	Body,    // HUD, message log, settings, sheet, dialogs — must survive 17px
	Display, // the DUNGEON title, menu and sheet headings
	Script,  // scrolls, spell descriptions, item flavour — in-world text
	Mono,    // dev console, editor numeric fields — alignment matters
};
inline constexpr int kFontRoleCount = 4;

// Role <-> the token used in fonts.cat and the dev console ("body", "display",
// "script", "mono"). FontRoleFromName returns false for an unknown token.
const char* FontRoleName(FontRole role);
bool FontRoleFromName(std::string_view name, FontRole& out);

// How a role resolves. Both fields come from fonts.cat.
struct FaceSpec {
	// Path to the .ttf. Empty = the system fallback (Consolas -> Segoe UI ->
	// Arial), which is what every role does before a face is chosen.
	std::string path;
	// Optical multiplier folded into every size drawn in this role, correcting
	// for the face's x-height so a swap does not resize the UI. 1.0 = as
	// authored.
	float scale = 1.0f;
};

class FontLibrary {
public:
	explicit FontLibrary(gfx::GraphicsDevice& device);

	// Points a role at a face. Safe at any time — existing Font references stay
	// valid (they belong to the old face); owners pick the new face up at their
	// next Get/UseFont. This is what the Phase 4 audition hot-swap drives.
	void SetFace(FontRole role, FaceSpec spec);
	const FaceSpec& Face(FontRole role) const;

	// The one Font for this role at this size. `pixelHeight` is the AUTHORED
	// design size; the role's optical scale is applied here. The reference is
	// stable for the library's lifetime.
	Font& Get(FontRole role, float pixelHeight);

	// Flushes glyphs cached during last frame's draw/measure for every live
	// font. Once per frame, before anything draws — never mid-record (Commit
	// drains the GPU). One loop that cannot forget a context the way a
	// hand-written list can.
	void CommitAll();

	// Diagnostics for the dev console (`fonts`).
	struct Live {
		std::string face; // path, or "(system fallback)"
		int pixelHeight = 0;
	};
	std::vector<Live> LiveFonts() const;

private:
	// Face bytes are shared per PATH, so blob identity is path identity and the
	// pointer is a valid key component.
	FaceData FaceFor(const std::string& path);

	// (face blob, rounded pixel height). std::map keeps it ordered for the
	// diagnostics dump and needs no hand-written hash.
	using Key = std::pair<const std::vector<u8>*, int>;

	gfx::GraphicsDevice& m_device;
	FaceSpec m_roles[kFontRoleCount];
	std::map<std::string, FaceData> m_faces; // path (""=fallback) -> bytes
	std::map<Key, std::unique_ptr<Font>> m_fonts;
	bool m_warnedCount = false;
};

} // namespace dungeon::ui
