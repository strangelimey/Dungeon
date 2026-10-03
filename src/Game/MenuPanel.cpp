// ============================================================================
// Game/MenuPanel.cpp - see MenuPanel.h.
// ============================================================================
#include "Game/MenuPanel.h"

#include "Graphics/SpriteBatch.h"
#include "UI/Font.h"
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <utility>

namespace dungeon::game {

MenuPanel::MenuPanel(std::string title, size_t entries, float top)
	: m_title(std::move(title)), m_entries(std::max<size_t>(entries, 1)), m_top(top) {
	bounds = {0, 0, 1, 1};
	debugName = "MenuPanel";
	// The panel paints only its card; under the rest of the window lies the
	// page it sits on, which it neither claims nor covers.
	overlapOk = true;
	m_list = Add<ui::MenuList>(gfx::Rect{0, 0, 1, 1}, 1.0f / static_cast<float>(m_entries));
	m_list->gapRem = kGapRem;
}

gfx::Rect MenuPanel::Card() const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(kPadRem);
	const float listH = Rem(kEntryRem + kGapRem) * static_cast<float>(m_entries) - Rem(kGapRem);
	const float h = pad * 2.0f + (m_title.empty() ? 0.0f : Rem(kTitleRem)) + listH;
	// Wide enough for the longest word with a stone's worth of air either side.
	const float w = std::max(Rem(kMinWidthRem), m_labelW + Rem(4.0f) + pad * 2.0f);
	const float y = m_top < 0.0f ? px.y + (px.h - h) * 0.5f : px.y + px.h * m_top;
	const float x = std::clamp(px.x + px.w * centreX - w * 0.5f, px.x,
							   std::max(px.x, px.x + px.w - w));
	return {x, y, w, h};
}

void MenuPanel::LayoutSelf(ui::UIContext& ctx) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f || !m_list) return;
	// The list's labels in this panel's own face and size (the list inherits
	// both), so the card fits the words it will draw.
	m_labelW = 0.0f;
	for (size_t i = 0; i < m_list->Count(); ++i)
		m_labelW = std::max(m_labelW, TextFont().MeasureWidth(m_list->Label(i)));
	m_titleFont = m_title.empty() ? nullptr
								  : &ctx.FontAt(ui::FontRole::Display, Rem(kTitleFontRem));

	const gfx::Rect card = Card();
	const float pad = Rem(kPadRem);
	const float top = card.y + pad + (m_title.empty() ? 0.0f : Rem(kTitleRem));
	// The list ends a gap below its last stone (MenuList cuts each slot's gap
	// from its bottom), so it is one gap taller than the stones it shows.
	const float listH = Rem(kEntryRem + kGapRem) * static_cast<float>(m_entries);
	m_list->bounds = {(card.x + pad - px.x) / px.w, (top - px.y) / px.h,
					  (card.w - pad * 2.0f) / px.w, listH / px.h};
}

void MenuPanel::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect card = Card();
	ui::DrawPanelFace(ctx, batch, card);
	if (m_titleFont) DrawCardTitle(ctx, batch, *m_titleFont, m_title, card, Rem(kPadRem), Rem(kTitleRem));
}

void DrawCardTitle(ui::UIContext& ctx, gfx::SpriteBatch& batch, const ui::Font& font,
				   const std::string& title, const gfx::Rect& card, float pad, float band) {
	const float x = card.x + (card.w - font.MeasureWidth(title)) * 0.5f;
	const float y = card.y + pad + (band - font.Height()) * 0.5f;
	if (ctx.GetSkin())
		ui::DrawCarvedText(batch, font, title, x, y, ui::CarvedTitle(ctx.GetSkin()));
	else
		font.Draw(batch, title, x, y, ctx.GetTheme().accent);
}

// --- PageCard ----------------------------------------------------------------

PageCard::PageCard(const gfx::Rect& rect, std::string title) : m_title(std::move(title)) {
	bounds = rect;
	debugName = "PageCard";
}

gfx::Rect PageCard::ContentRect() const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(MenuPanel::kPadRem);
	const float band = m_title.empty() ? 0.0f : Rem(MenuPanel::kTitleRem);
	return {px.x + pad, px.y + pad + band, px.w - pad * 2.0f, px.h - pad * 2.0f - band};
}

void PageCard::LayoutSelf(ui::UIContext& ctx) {
	m_titleFont = m_title.empty() ? nullptr
								  : &ctx.FontAt(ui::FontRole::Display, Rem(MenuPanel::kTitleFontRem));
}

void PageCard::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	ui::DrawPanelFace(ctx, batch, Pixel());
	if (m_titleFont)
		DrawCardTitle(ctx, batch, *m_titleFont, m_title, Pixel(), Rem(MenuPanel::kPadRem),
					  Rem(MenuPanel::kTitleRem));
}

} // namespace dungeon::game
