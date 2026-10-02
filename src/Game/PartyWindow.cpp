// ============================================================================
// Game/PartyWindow.cpp - see PartyWindow.h.
//
// Laid out in the cards' em (CardEm): the chrome around the cards, and the
// panel's own size (SizeForEm), so the window keeps its shape at any scale.
// ============================================================================
#include "Game/PartyWindow.h"

#include "UI/FloatingPanel.h"
#include "UI/Layout.h"

#include <algorithm>

namespace dungeon::game {

namespace {
constexpr float kCloseEm = 2.4f;      // the corner box, square
constexpr float kStatusTextEm = 1.1f; // the status line's text (the sheet's size)
constexpr float kStatusGapEm = 0.8f;  // between its name and its line
constexpr float kCardHEm = CharacterSheet::kCardNameEm + CharacterSheet::kCardTabEm;
} // namespace

Vec2 PartyWindow::SizeForEm(float em) {
	const float k = em * kCardScale;
	const float w = 2.0f * kPadEm + 2.0f * CharacterSheet::kCardWEm + kGapEm;
	const float h = 2.0f * kPadEm + kTabEm + kGapEm + 2.0f * kCardHEm + kGapEm + kStatusEm;
	return {w * k, h * k};
}

int PartyWindow::InventoryRows() const {
	int rows = 1;
	const size_t shown = std::min(m_roster->size(), kMaxCards);
	for (size_t i = 0; i < shown; ++i) {
		const int slots = static_cast<int>((*m_roster)[i].inventory.SelectedContents().size());
		rows = std::max(rows, (slots + CharacterSheet::kCardInvCols - 1) / CharacterSheet::kCardInvCols);
	}
	return rows;
}

Vec2 PartyWindow::PanelSize(ui::UIContext& ctx, float s, float em,
							CharacterSheet::Mode mode) const {
	if (mode != CharacterSheet::Mode::Inventory || !squareDesign) return SizeForEm(em);
	// The chrome stays in the window's card em; the cards are the squares'.
	const float k = em * kCardScale;
	const float sq = ctx.FontAt(ctx.RootRole(), squareDesign() * s).Height();
	const float w = (2.0f * kPadEm + kGapEm) * k + 2.0f * CharacterSheet::kCardInvWEm * sq;
	const float h = (2.0f * kPadEm + kTabEm + 2.0f * kGapEm + kStatusEm) * k +
					2.0f * CharacterSheet::CardInventoryHEm(InventoryRows()) * sq;
	return {w, h};
}

PartyWindow::PartyWindow(const ui::FloatingPanel* panel, std::vector<Character>* roster,
						 const ResourceBarStyle* barStyle, const ItemIconBank* icons,
						 const ItemWeightBank* weights, const ItemIconBank* slotIcons,
						 const ItemCategoryBank* categories, HeldItem* held,
						 const gfx::Texture* closeIcon, std::function<void()> onClose)
	: m_panel(panel), m_roster(roster) {
	debugName = "PartyWindow";
	// The cards FIRST, so the stones and the close box (added after) update
	// before them - nothing overlaps, but it keeps the sheet's order.
	for (size_t i = 0; i < kMaxCards; ++i) {
		m_cards[i] = Add<CharacterSheet>(gfx::Rect{}, roster, barStyle, icons, weights,
										 slotIcons, categories, held, true);
		m_cards[i]->SetCharacter(i);
	}
	m_strip = Add<ModeSelector>(gfx::Rect{}, 5, &m_modeIndex, [this](int i) {
		m_modeIndex = i;
		for (CharacterSheet* card : m_cards) card->SelectMode(i);
	});
	m_closeSlot = Add<ui::Box>(gfx::Rect{});
	m_closeSlot->debugName = "close";
	ui::AddCloseButton(*m_closeSlot, closeIcon, std::move(onClose));
}

void PartyWindow::Open(CharacterSheet::Mode mode) {
	if (!m_open) ++m_opens;
	m_open = true;	m_modeIndex = static_cast<int>(mode);
	for (size_t i = 0; i < m_cards.size(); ++i) {
		m_cards[i]->SetCharacter(i);
		m_cards[i]->SelectMode(m_modeIndex);
	}
}

void PartyWindow::SetModeEtches(std::span<const gfx::Texture* const> etch,
								std::span<const gfx::Texture* const> lit) {
	if (m_strip) m_strip->SetEtches(etch, lit);
}

void PartyWindow::LayoutSelf(ui::UIContext& ctx) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;
	const float k = CardEm();
	const float pad = kPadEm * k, gap = kGapEm * k;
	const auto frac = [&](float x, float y, float w, float h) {
		return gfx::Rect{x / px.w, y / px.h, w / px.w, h / px.h};
	};
	m_strip->bounds = frac(pad, pad, (5.0f * kTabEm + 4.0f * kTabGapEm) * k, kTabEm * k);
	const float close = kCloseEm * k;
	m_closeSlot->bounds = frac(px.w - pad - close, pad + (kTabEm * k - close) * 0.5f, close, close);

	// Two by two under the stones, the status band under them. The cards'
	// text follows the PANEL'S scale (fontScale is absolute, not inherited).
	const float top = pad + (kTabEm + kGapEm) * k;
	const float cardW = std::max((px.w - 2.0f * pad - gap) * 0.5f, 0.0f);
	const float cardH = std::max((px.h - top - gap - kStatusEm * k - pad) * 0.5f, 0.0f);
	// On the Inventory tab the cards' squares are the SHEET'S (its text size,
	// times this panel's scale so the window still scales as a whole).
	const float panelScale = m_panel ? m_panel->Scale() : 1.0f;
	const bool squares = CurrentMode() == CharacterSheet::Mode::Inventory && squareDesign &&
						 ctx.DesignHeight() > 0.0f;
	const float scale = squares ? squareDesign() * panelScale / ctx.DesignHeight()
								: panelScale * kCardScale;
	for (size_t i = 0; i < m_cards.size(); ++i) {
		CharacterSheet* card = m_cards[i];
		card->visible = i < m_roster->size();
		card->fontScale = scale;
		const float col = static_cast<float>(i % 2), row = static_cast<float>(i / 2);
		card->bounds = frac(pad + col * (cardW + gap), top + row * (cardH + gap), cardW, cardH);
	}
}

void PartyWindow::UpdateSelf(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	m_mouseX = input ? input->MouseX() : -1.0f;
	m_mouseY = input ? input->MouseY() : -1.0f;
}

const CharacterSheet* PartyWindow::StatusCard() const {
	const CharacterSheet* any = nullptr;
	for (const CharacterSheet* card : m_cards) {
		if (!card->visible || card->StatusName().empty()) continue;
		if (card->Pixel().Contains(m_mouseX, m_mouseY)) return card;
		if (!any) any = card;
	}
	return any;
}

std::string_view PartyWindow::StatusName() const {
	const CharacterSheet* card = StatusCard();
	return card ? card->StatusName() : std::string_view{};
}

std::string_view PartyWindow::StatusText() const {
	const CharacterSheet* card = StatusCard();
	return card ? card->StatusText() : std::string_view{};
}

void PartyWindow::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	ui::DrawPanelFace(ctx, batch, px, opacity ? *opacity : 1.0f);

	// The status line, as the sheet draws its own: a rule, the name in its
	// colour, then the line about it.
	const float k = CardEm();
	const float pad = kPadEm * k;
	const gfx::Rect band{px.x + pad, px.y + px.h - pad - kStatusEm * k, px.w - 2.0f * pad,
						 kStatusEm * k};
	const ui::Theme& theme = ctx.GetTheme();
	batch.DrawRect({band.x, band.y, band.w, 1.0f}, theme.panelBorder);
	const CharacterSheet* card = StatusCard();
	if (!card) return;
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, kStatusTextEm * k);
	const float y = band.y + (band.h - font.Height()) * 0.5f;
	const std::string_view name = ui::FitText(font, card->StatusName(), band.w);
	ui::DrawFittedText(batch, font, card->StatusName(), band.x, y, band.w, card->StatusColor());
	const float textX = band.x + font.MeasureWidth(name) + kStatusGapEm * k;
	if (card->StatusText().empty() || textX >= band.x + band.w) return;
	ui::DrawFittedText(batch, font, card->StatusText(), textX, y, band.x + band.w - textX,
					   theme.text);
}

} // namespace dungeon::game
