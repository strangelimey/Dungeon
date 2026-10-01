// ============================================================================
// Game/MemberCards.cpp - see MemberCards.h.
// ============================================================================
#include "Game/MemberCards.h"

#include "Game/ControlBar.h" // HandPair
#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// In EMs of the card's own type (so they follow the panel's scale), except the
// portrait band, which is a share of the card's WIDTH like the boxes below it.
constexpr float kCardPad = 0.35f;   // inside the card's edge
constexpr float kBandShare = 0.48f; // portrait band height / card width
constexpr float kInnerGap = 0.2f;   // portrait band -> hand boxes
constexpr float kCardGap = 0.45f;   // between cards, both ways

size_t Rows(size_t members) { return (std::min<size_t>(members, 4) + 1) / 2; }
} // namespace

// --- MemberCard ----------------------------------------------------------------

MemberCard::MemberCard(const std::vector<Character>* roster, size_t member,
					   const float* opacity)
	: m_roster(roster), m_member(member), m_opacity(opacity) {
	debugName = "MemberCard";
}

float MemberCard::Height(float widthPx, float emPx) {
	const float pad = emPx * kCardPad;
	const float band = widthPx * kBandShare;
	const float handsW = std::max(0.0f, widthPx - 2 * pad);
	return 2 * pad + band + emPx * kInnerGap + HandPair::NeededHeight(handsW, emPx);
}

void MemberCard::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	const bool present = RosterMember(m_roster, m_member) != nullptr;
	if (m_panel) m_panel->visible = present;
	if (m_hands) m_hands->visible = present;
	if (!present || px.w <= 0.0f || px.h <= 0.0f) return;
	const float em = Em(1.0f);
	const float pad = em * kCardPad;
	const float band = px.w * kBandShare;
	const float handsW = std::max(0.0f, px.w - 2 * pad);
	const float handsH = HandPair::NeededHeight(handsW, em);
	if (m_panel) m_panel->bounds = {0.0f, 0.0f, 1.0f, (pad + band) / px.h};
	if (m_hands)
		m_hands->bounds = {pad / px.w, (pad + band + em * kInnerGap) / px.h, handsW / px.w,
						   handsH / px.h};
}

// The ONE face behind both pieces: the CharacterPanel's own is faded to
// nothing by its owner, so the card reads as a single slab.
void MemberCard::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!RosterMember(m_roster, m_member)) return;
	ui::DrawPanelFace(ctx, batch, Pixel(), m_opacity ? *m_opacity : 1.0f);
}

// --- CardGrid ------------------------------------------------------------------

float CardGrid::CardWidth(float widthPx, float emPx) {
	return std::max(0.0f, (widthPx - emPx * kCardGap) * 0.5f);
}

float CardGrid::Height(float widthPx, float emPx, size_t members) {
	const size_t rows = Rows(members);
	if (rows == 0) return 0.0f;
	const float cardH = MemberCard::Height(CardWidth(widthPx, emPx), emPx);
	return cardH * static_cast<float>(rows) + emPx * kCardGap * static_cast<float>(rows - 1);
}

void CardGrid::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;
	const float em = Em(1.0f);
	const float gap = em * kCardGap;
	const float cardW = CardWidth(px.w, em);
	const float cardH = MemberCard::Height(cardW, em);
	size_t i = 0;
	for (const auto& child : Children()) {
		const float col = static_cast<float>(i % 2), row = static_cast<float>(i / 2);
		child->bounds = {(cardW + gap) * col / px.w, (cardH + gap) * row / px.h, cardW / px.w,
						 cardH / px.h};
		++i;
	}
}

} // namespace dungeon::game
