// ============================================================================
// UI/Controls_Detail.h - the two drawing helpers the control families share
// and nothing outside them should call (code-review C127 split Controls.cpp,
// where they were file-local). Included by Controls*.cpp only.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

#include <string_view>

namespace dungeon::ui {

class Font;
class UIContext;
struct Skin;

namespace detail {

// The context's skin when it can draw a panel face, or null when unskinned
// (flat debug mode) - the widget-side gate for every "skin or flat?" draw
// decision.
const Skin* PanelSkin(const UIContext& ctx);

// A small opaque box naming something in full, above `anchor` - or below it
// when that leaves the window - and clamped sideways. A Button's tooltip and a
// trimmed drop-down's face share it, so every tip reads the same.
void DrawTooltip(UIContext& ctx, gfx::SpriteBatch& batch, const Font& font,
				 std::string_view text, const gfx::Rect& anchor);

} // namespace detail

using detail::DrawTooltip;
using detail::PanelSkin;

} // namespace dungeon::ui
