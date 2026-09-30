// ============================================================================
// UI/ControlIcons.h — the glyph textures the control library draws ITSELF with.
//
// Distinct from Skin (UI/Skin.h), which is per-CONTEXT chrome the owner toggles
// (Settings → Textured UI) and which only GameUI's pages carry. These are the
// small fixed glyphs a control has always drawn for itself — the drop-down's
// expander box — and they belong to every context alike, including the editor
// dialogs that own a bare UIContext and never see a Skin. A control falls back
// to its text glyph when the texture is absent, so a missing asset is a look,
// never a crash.
//
// LIFETIME: the registry BORROWS. The Game side owns the textures (AssetUtil's
// LoadSharedControlIcons / ReleaseSharedIcons) and must clear the registry
// before dropping them — a gfx::Texture returns its SRV slot on destruction, so
// a stale pointer here would name a slot something else has taken.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon::ui {

struct ControlIcons {
	// DropDown's expander: an authored box with a down triangle, drawn at the
	// right end of the control, and its open twin with the triangle up. Two
	// files rather than one turned half a rotation, because the turn would put
	// the box's top-lit rim at the bottom too. No open icon = the closed one,
	// turned.
	const gfx::Texture* dropDown = nullptr;
	const gfx::Texture* dropDownOpen = nullptr;
};

// Installs the shared set (one call at startup). Pass a default-constructed
// ControlIcons to clear it.
void SetControlIcons(const ControlIcons& icons);
const ControlIcons& GetControlIcons();

} // namespace dungeon::ui
