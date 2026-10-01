// ============================================================================
// Game/HudTray.cpp - see HudTray.h.
// ============================================================================
#include "Game/HudTray.h"

#include <algorithm>

namespace dungeon::game {

HudTray::HudTray(const float* opacity) : m_opacity(opacity) {
	debugName = "HudTray";
}

void HudTray::AddPanel(const ui::FloatingPanel* panel, bool* hidden, const gfx::Texture* glyph,
					   std::string name, std::function<void()> onRestore) {
	if (!panel || !hidden) return;
	// The words are the fallback face when the glyph is missing, and the
	// tooltip either way.
	auto* button = Add<ui::Button>(gfx::Rect{}, name,
		[hidden, restore = std::move(onRestore)] {
			*hidden = false;
			if (restore) restore();
		});
	button->faceIcon = glyph;
	button->tooltip = std::move(name);
	button->visible = false;
	m_entries.push_back({panel, hidden, button});
}

bool HudTray::Wanted(const Entry& e) const {
	return *e.hidden && (!e.panel->shownWhen || e.panel->shownWhen());
}

size_t HudTray::ShownCount() const {
	size_t n = 0;
	for (const Entry& e : m_entries)
		if (Wanted(e)) ++n;
	return n;
}

Vec2 HudTray::Size(size_t count, float em) {
	const float n = static_cast<float>(std::max<size_t>(count, 1));
	return {em * (2.0f * kPad + n * kCell + (n - 1.0f) * kGap), em * (2.0f * kPad + kCell)};
}

// The shown buttons in a row, in the panels' table order, so a panel's button
// keeps its place among the others whatever order they were minimized in.
void HudTray::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;
	const float em = Em(1.0f);
	const float pad = em * kPad, cell = em * kCell, step = cell + em * kGap;
	float x = pad;
	for (Entry& e : m_entries) {
		e.button->visible = Wanted(e);
		if (!e.button->visible) continue;
		e.button->bounds = {x / px.w, pad / px.h, cell / px.w, cell / px.h};
		x += step;
	}
}

void HudTray::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	ui::DrawPanelFace(ctx, batch, Pixel(), m_opacity ? *m_opacity : 1.0f);
}

} // namespace dungeon::game
