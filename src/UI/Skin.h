// ============================================================================
// UI/Skin.h — textured chrome for the widget library.
//
// A skinned face is drawn in LAYERS (ui-panels, docs/ui-panels-plan.md P1):
//   1. the STONE - one seamless tile (assets/ui/stones/, tools/BuildUiStones.py)
//      repeated across the face in SCREEN space, so a slot reads as cut from the
//      same slab as the panel around it and the grain is the same density on a
//      button as on a window-wide panel;
//   2. a BEVEL overlay per face kind - a 9-slice carrying only light (white
//      highlight top/left, black shadow bottom/right, a dark rim, a darkened
//      well for a slot; tools/BuildUiFrames.py), so any stone wears any frame;
//   3. for a panel, a stretched SHEEN - the polish.
// That split is what lets the stone be PICKED (Settings -> UI) and switched
// live without re-baking anything: only the stone pointer changes.
//
// The flat look is NOT replaced — it stays as the skinless fallback
// (UIContext::GetSkin() == nullptr), kept deliberately as a DEBUG MODE: the
// solid fills + 1px borders read widget containment and extents at a glance.
// The Settings → UI "Textured UI" toggle flips between them live (GameUI
// ApplySkin pushes the skin, or null, into every context). Theme colors still
// matter under a skin: hot/active states wash the theme's control colors over
// the face, and a panel multiplies the theme panel alpha so the user's
// background-opacity preference survives.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon::ui {

// One 9-slice image: `corner` is the fixed frame width in TEXTURE pixels
// (uniform on all sides), `scale` maps texture pixels to screen pixels for
// the frame and the tile size (1 = native). `stretch` switches the edges and
// center from TILED (material textures — stone stays stone-dense at any
// panel size) to STRETCHED (authored faces with baked lighting — a plaque's
// gradient spans the widget once; tiling it repeats as light/dark bands).
// `inset` is how much of the corner is VISIBLE frame (rim + bevel), in texture
// pixels: content laid inside the face starts that far in (FaceInset).
struct SkinPart {
	const gfx::Texture* texture = nullptr;
	float corner = 0.0f;
	float scale = 1.0f;
	bool stretch = false;
	float inset = 0.0f;
};

// The face kinds a skin draws. ButtonDown is a held or active button (the
// bevel inverted); Slot is an item socket, sunk into the stone with a dark well
// the item icons read against. Block / BlockDown are a CUT-STONE block
// (more-ui-updates): its own square of stone in a dark joint with a wide
// chamfer for an edge, standing proud of the panel - and down in its joint when
// pressed. See DrawCutStone.
enum class Face { Panel, Button, ButtonDown, Slot, Block, BlockDown };

// The part set the widget library knows how to use. Parts may be null
// individually — a widget only skins itself when its part has a texture.
struct Skin {
	// The stone every face is cut from. Null = `stoneFallback`, a flat colour,
	// under the same frames (a missing stones folder still reads as chrome).
	const gfx::Texture* stone = nullptr;
	float stoneTile = 1024.0f; // screen pixels one stone tile covers
	Vec4 stoneFallback{0.16f, 0.16f, 0.17f, 1.0f};

	// The bevel overlays, one per face kind, and the panel's polish.
	SkinPart panel;      // windows, docks, popups, tab pages
	SkinPart button;     // every button face and tab
	SkinPart buttonDown; // held / active button; falls back to `button`
	SkinPart slot;       // item sockets
	SkinPart sheen;      // stretched over a panel (corner 0); optional
	SkinPart block;      // a cut-stone block (frame_block)
	SkinPart blockDown;  // the same, pressed; falls back to `block`

	// The ring under every glyph a skinned context draws (UIContext::Render ->
	// SpriteBatch::SetTextOutline): stone is mid-toned and busy, so light text
	// on it washes out without a dark edge. Alpha 0 turns it off.
	Vec4 textOutline{0.03f, 0.025f, 0.02f, 0.85f};

	// LEGIBILITY ON THE MATERIAL SHOWN (more-ui-updates, the contrast pass): set
	// with the stone from assets/ui/stones/stones.cat (GameUI::ApplyStone).
	// `luma` is the material's toned luminance - a light one carves darker
	// words and rings its text harder (ui::CarvedGold, textOutline); `calm` is
	// the alpha of a wash of the stone's own mean colour over every face but a
	// slot, which quiets a BUSY texture (leaves, lava) without changing its hue.
	float luma = 0.20f;
	float calm = 0.0f;
	Vec4 stoneMean{0.2f, 0.2f, 0.2f, 1.0f};
	// The carved INKS on this material (ui::CarvedGold & co. return them), SOLVED
	// against `stoneMean` by ui::ResolveInks whenever the material changes: each
	// is the authored gold kept wherever it already reads, and otherwise moved
	// toward pale gold or dark bronze until it clears a contrast ratio (Michael:
	// the gold on a light stone blended into it - the brightness-only rule
	// darkened it on mid-toned materials, the wrong way). The defaults are the
	// authored dark-stone inks, so a skin never resolved draws as it always did.
	Vec4 inkGold{0.80f, 0.62f, 0.26f, 1.0f};
	Vec4 inkLit{1.0f, 0.86f, 0.46f, 1.0f};
	Vec4 inkTitle{0.86f, 0.68f, 0.30f, 1.0f};
	Vec4 inkPlain{0.78f, 0.74f, 0.66f, 0.80f};
	Vec4 inkDisabled{0.45f, 0.40f, 0.30f, 1.0f};
};

// Draws `part` into `dst` as a 9-slice: fixed corners, edges tiled along
// their axis, center tiled both ways. `tint` multiplies (white = as authored).
// No-op when the part has no texture.
void DrawNineSlice(gfx::SpriteBatch& batch, const gfx::Rect& dst,
				   const SkinPart& part, const Vec4& tint);

// Draws a whole skinned face: the stone (tinted by `tint` - darken it to dim a
// disabled control), the kind's bevel over it, and a panel's sheen. `tint.w`
// fades the face as a whole (a panel's background opacity). No-op when the
// kind's part has no texture.
void DrawFace(gfx::SpriteBatch& batch, const gfx::Rect& dst, const Skin& skin,
			  Face face, const Vec4& tint);

// How far in from a face's edge its visible frame reaches, in screen pixels -
// where content laid on the face (a slot's icon, a member button's colour)
// starts.
float FaceInset(const Skin& skin, Face face);

// A CUT-STONE button (more-ui-updates): a Block face, sunk into its joint by
// `depth` (0 up .. 1 pressed - Button's push), with an ETCHED symbol over it
// (assets/ui/etch_<name>.png, tools/BuildEtchGlyphs.py: a lit groove with gold
// on its floor, light only, so it suits every material). The symbol moves down
// and right with the press, as the block does. `tint` dims (a disabled
// stone); `hot` lifts the face a touch. No-op without the block part.
void DrawCutStone(gfx::SpriteBatch& batch, const gfx::Rect& dst, const Skin& skin,
				  const gfx::Texture* etch, float depth, bool hot, const Vec4& tint);

} // namespace dungeon::ui
