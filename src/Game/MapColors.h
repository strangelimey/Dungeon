// ============================================================================
// Game/MapColors.h — the dungeon map's own stylized ink palette.
//
// Shared by MapView (the viewport renderer + symbol key) and MapEditor (the
// brush-palette swatches), so the two stay in lockstep — a wall swatch in the
// editor is the same colour the map paints walls with. These are deliberately
// NOT the UI theme: the map has its own look, independent of the live theme.
// ============================================================================
#pragma once

#include "Core/MathTypes.h" // Vec4

namespace dungeon::game {

// Inline (one definition across TUs). Map palette, not the UI theme.
inline const Vec4 kMapBg{0.04f, 0.04f, 0.06f, 1.0f}; // panel fill (opaque: full-screen editor covers all)
inline const Vec4 kWall{0.46f, 0.42f, 0.36f, 1.0f};  // solid structure (the bright ink)
inline const Vec4 kFloor{0.13f, 0.13f, 0.16f, 1.0f}; // walkable (recedes)
inline const Vec4 kTorch{1.0f, 0.62f, 0.28f, 1.0f};
inline const Vec4 kBrazier{1.0f, 0.45f, 0.16f, 1.0f};
inline const Vec4 kMonster{0.85f, 0.22f, 0.22f, 1.0f};
inline const Vec4 kItem{0.42f, 0.85f, 0.42f, 1.0f};
inline const Vec4 kButton{0.42f, 0.62f, 0.95f, 1.0f};
inline const Vec4 kDecoration{0.74f, 0.54f, 0.92f, 1.0f}; // static props (columns, fountains, ...)
inline const Vec4 kCeiling{0.30f, 0.30f, 0.34f, 1.0f};    // ceiling palette swatch
inline const Vec4 kDoor{0.78f, 0.60f, 0.35f, 1.0f};       // door category
inline const Vec4 kStair{0.60f, 0.72f, 0.78f, 1.0f};      // stair category
inline const Vec4 kProjParty{0.55f, 0.85f, 1.0f, 1.0f};   // in-flight party shot
inline const Vec4 kProjMonster{1.0f, 0.72f, 0.35f, 1.0f}; // in-flight monster shot
inline const Vec4 kMarkerInk{0.96f, 0.96f, 0.98f, 1.0f};  // initials drawn over markers
inline const Vec4 kFacingArrow{0.25f, 1.0f, 0.40f, 1.0f}; // editor: which way a placed thing faces
// Editor: the wall FACE a wall-mounted brush (niche/sconce/hung prop) will use.
inline const Vec4 kFaceHighlight{0.40f, 0.95f, 1.0f, 0.95f};
// Editor: the placement GHOST — where the armed brush would land. Two inks, and
// the refused one is deliberately DRAWN rather than the ghost being hidden: a
// preview that silently vanishes says only "no", where one that stays and turns
// red says "here, and no" — and which cell it was refusing is the useful half.
inline const Vec4 kGhostOk{0.45f, 1.0f, 0.65f, 0.55f};
inline const Vec4 kGhostNo{1.0f, 0.35f, 0.35f, 0.45f};
// Editor: live validation's boxes (MapView_Issues.cpp) - red for an error, amber
// for a warning. The fill is these at a low alpha, the ring at full.
inline const Vec4 kIssueError{1.0f, 0.30f, 0.28f, 1.0f};
inline const Vec4 kIssueWarning{1.0f, 0.72f, 0.20f, 1.0f};

// Editor cell fill. At rest the editor draws STRUCTURE: every wall one dark
// ink, every floor one light ink (the reverse of the Player map's kWall/kFloor,
// so a dungeon reads as rooms carved out of rock). A surface's textures show
// only while its brush is armed, at a plain near-full multiply - the fill is
// the subject then. (Textures at rest, dimmed, and then their average colours
// were both tried and buried the layout.)
inline const Vec4 kEditorWall{0.15f, 0.14f, 0.13f, 1.0f};
inline const Vec4 kEditorFloor{0.52f, 0.50f, 0.46f, 1.0f};
inline const Vec4 kTexFillLit{0.92f, 0.92f, 0.95f, 1.0f};

} // namespace dungeon::game
