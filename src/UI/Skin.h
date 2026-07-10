// ============================================================================
// UI/Skin.h — textured chrome for the widget library.
//
// A Skin is a set of 9-slice PARTS (a texture + the corner inset baked into
// it); widgets that draw large chrome (Panel, TextOutput, button faces, the
// TabControl page) render the matching part instead of their flat theme
// fills. The flat look is NOT replaced — it stays as the skinless fallback
// (UIContext::GetSkin() == nullptr), kept deliberately as a DEBUG MODE: the
// solid fills + 1px borders read widget containment and extents at a glance.
// The Settings → UI "Textured UI" toggle flips between them live (GameUI
// ApplySkin pushes the skin, or null, into every context).
//
// DrawNineSlice keeps the corner ring at fixed scale and TILES the edges and
// center (repeating quads, not a stretch), so a stone texture stays
// stone-dense on a window-wide panel. Theme colors still matter under a skin:
// hot/active button states wash the face with the theme's control colors, and
// the panel part multiplies the theme panel alpha so the user's background-
// opacity preference survives.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon::ui {

// One 9-slice image: `corner` is the fixed frame width in TEXTURE pixels
// (uniform on all sides), `scale` maps texture pixels to screen pixels for
// the frame and the tile size (1 = native).
struct SkinPart {
	const gfx::Texture* texture = nullptr;
	float corner = 0.0f;
	float scale = 1.0f;
};

// The part set the widget library knows how to use. Parts may be null
// individually — a widget only skins itself when its part has a texture.
struct Skin {
	SkinPart panel;  // framed background (Panel, TextOutput, tab pages)
	SkinPart button; // button face (DrawButtonFace, tab strip)
};

// Draws `part` into `dst` as a 9-slice: fixed corners, edges tiled along
// their axis, center tiled both ways. `tint` multiplies (white = as authored).
// No-op when the part has no texture.
void DrawNineSlice(gfx::SpriteBatch& batch, const gfx::Rect& dst,
				   const SkinPart& part, const Vec4& tint);

} // namespace dungeon::ui
