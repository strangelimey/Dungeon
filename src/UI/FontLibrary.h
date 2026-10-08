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
// The keying is not a tidiness measure. A Font is one size for life (there is
// no re-bake at another size any more - code-review C89 took Font::SetHeight
// out with its last caller), and its upload in Commit calls WaitIdle, so a
// different size is a different Font by construction.
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
// this reason, and a floating panel holds its text at the drag-start scale
// until a resize drag ends - FloatingPanel::TextScale, code-review C221). A
// size that is only being TRIED asks HeightAt, which makes nothing. Runaway
// churn is not silent: the library warns once past kFontCountWarn live fonts,
// and `fonts` prints the count and its peak.
//
// THE LANGUAGE'S GLYPHS. Every font bakes Latin-1 when it is made, plus the
// WARM SET: the code points the active .lang file uses, handed in by Prewarm at
// each language load (code-review C229). A Font made later bakes the set at
// birth, and Prewarm bakes it into every live one, so the game's own text never
// meets a glyph mid-play; a miss after that is a late glyph (Font::LateGlyphs),
// reported by the allocation guard rather than excused.
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

	// The pixel height Get(role, pixelHeight) would hand back - the role's
	// optical scale folded in and rounded exactly as Get does - WITHOUT making
	// the font. Font::Height() is that number, so a measure taken here is the
	// one the font would give.
	float HeightAt(FontRole role, float pixelHeight) const;

	// The language's code points (loc::CodePoints): kept as the warm set every
	// font made from now on bakes at birth, and baked now into every live font
	// that lacks them. Drains the GPU for each font that gained a glyph, so call
	// between frames - a language load is.
	void Prewarm(std::vector<u32> codepoints);

	// Flushes glyphs cached during last frame's draw/measure for every live
	// font. Once per frame, before anything draws — never mid-record (Commit
	// drains the GPU). One loop that cannot forget a context the way a
	// hand-written list can.
	void CommitAll();

	// Diagnostics for the dev console (`fonts`).
	struct Live {
		std::string face; // path, or "(system fallback)"
		int pixelHeight = 0;
		u64 late = 0;     // Font::LateGlyphs
	};
	std::vector<Live> LiveFonts() const;
	// How many fonts are live, and the most there have ever been. Fonts are
	// never evicted, so the two agree today; the peak is the number a harness
	// holds still across a resize drag (AllocTest -Panels, code-review C221).
	size_t Count() const { return m_fonts.size(); }
	size_t Peak() const { return m_peak; }
	// The warm set's size, and the late glyphs over every live font.
	size_t WarmCount() const { return m_warm.size(); }
	u64 LateGlyphs() const;

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
	std::vector<u32> m_warm; // the active language's code points (Prewarm)
	size_t m_peak = 0;
	bool m_warnedCount = false;
};

} // namespace dungeon::ui
