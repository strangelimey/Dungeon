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

// Editor textured cell fill. The dim base view draws each cell's albedo with a
// CURVE, not a pure multiply: the texture (alpha < 1) composites over a flat
// lift ink underneath, so out = albedo * (rgb * a) + lift * (1 - a). The lift
// raises the blacks of these dark scanned sets into visibility while the sub-1
// scale keeps the highlights below the markers'. The armed view (painting that
// surface) is a plain near-full multiply — the fill is the subject then.
inline const Vec4 kTexFillLift{0.75f, 0.75f, 0.80f, 1.0f}; // under-rect
inline const Vec4 kTexFillDim{0.62f, 0.62f, 0.68f, 0.85f}; // albedo over it
inline const Vec4 kTexFillLit{0.92f, 0.92f, 0.95f, 1.0f};

} // namespace dungeon::game
