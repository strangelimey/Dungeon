// ============================================================================
// Game/HandSlot.cpp — see HandSlot.h.
// ============================================================================
#include "Game/HandSlot.h"

#include "Game/PartyHudDraw.h"
#include "Game/Spell/Spell.h"
#include "UI/Skin.h"

#include <algorithm>
#include <span>

namespace dungeon::game {

HandSlot::HandSlot(const gfx::Rect& rect, const std::vector<Character>* roster,
				   size_t member, int hand,
				   const ItemIconBank* icons, std::function<void()> onLeft,
				   std::function<void()> onRight, std::function<void()> onMiddle)
	: m_roster(roster), m_member(member), m_hand(hand), m_icons(icons),
	  m_onLeft(std::move(onLeft)), m_onRight(std::move(onRight)),
	  m_onMiddle(std::move(onMiddle)) {
	bounds = rect;
}

void HandSlot::UpdateSelf(ui::UIContext& ctx) {
	m_character = RosterMember(m_roster, m_member);
	if (!m_character) return; // roster shorter than this slot — inert
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) {
			m_held = true;
			m_holdFired = false;
			m_pressAt = std::chrono::steady_clock::now();
		}
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		if (input->WasMousePressed(MouseButton::Middle)) m_heldMiddle = true;
		ctx.ConsumeMouse();
	}
	// The hold fires once, while the press is still down over the box; moving
	// off the box first lets it run out as an ordinary cancelled click.
	if (m_held && !m_holdFired && m_hot && input->IsMouseDown(MouseButton::Left) &&
		std::chrono::duration<float>(std::chrono::steady_clock::now() - m_pressAt).count() >=
			kHoldSeconds) {
		m_holdFired = true;
		if (onHold) onHold();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && !m_holdFired && m_onLeft) m_onLeft();
		m_held = false;
		m_holdFired = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot && m_onRight) m_onRight();
		m_heldRight = false;
	}
	if (m_heldMiddle && input->WasMouseReleased(MouseButton::Middle)) {
		if (m_hot && m_onMiddle) m_onMiddle();
		m_heldMiddle = false;
	}
}

void HandSlot::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	if (!m_character) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();
	// The socket: skinned, the stone sunk into a dark well (ui::DrawSlotFace,
	// the one socket look every item slot shares); flat, the black fill. Either
	// way the light-haloed item icons read against a dark middle. A subtle lift
	// on hover/press keeps the interaction feedback.
	const ui::Skin* skin = ctx.GetSkin();
	const bool skinned = skin && skin->slot.texture;
	// A SET hand says so: the socket behind the icon takes the theme accent - a
	// LOW tint to the edges, and a soft glow brighter at the centre (Michael,
	// 2026-09-30: the old 35% flat mix washed the box out). Over the flat fill the
	// tint is mixed to an opaque colour; over stone it is a translucent wash, so
	// the grain still shows through it. The glow then lays the accent over the
	// middle.
	constexpr float kTint = 0.14f;
	const HandSetUse use = setUse ? setUse() : HandSetUse{};
	gfx::Rect socket = px;
	if (skinned) {
		socket = ui::DrawSlotFace(ctx, batch, px, kSlotBg, m_held ? 0.12f : (m_hot ? 0.06f : 0.0f));
		if (use.set)
			batch.DrawRect(socket, {theme.accent.x, theme.accent.y, theme.accent.z, kTint});
	} else {
		Vec4 fill = m_held ? Vec4{0.22f, 0.22f, 0.24f, 1.0f}
						   : (m_hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg);
		if (use.set)
			fill = {fill.x + (theme.accent.x - fill.x) * kTint,
					fill.y + (theme.accent.y - fill.y) * kTint,
					fill.z + (theme.accent.z - fill.z) * kTint, 1.0f};
		batch.DrawRect(socket, fill);
	}
	if (use.set && glow) {
		constexpr float kGlow = 0.5f; // the accent's share at the very centre
		batch.DrawSprite(socket, {0, 0, 1, 1}, *glow,
						 {theme.accent.x, theme.accent.y, theme.accent.z, kGlow});
	}
	// The item held in this hand, if any, drawn inset from the border.
	const ItemSlot& slot = m_character->inventory.Hand(m_hand);
	const float pad = px.w * 0.12f;
	const gfx::Rect inner{px.x + pad, px.y + pad, px.w - 2 * pad, px.h - 2 * pad};
	bool drewItem = false;
	if (!slot.Empty() && m_icons) {
		if (const gfx::Texture* icon = m_icons->For(slot.typeId)) {
			batch.DrawSprite(inner, {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
			drewItem = true;
		}
	}
	// An empty hand set to a verb with a picture (punch, kick) shows the ACTION
	// (Michael, 2026-09-28: the paper doll's hand and feet slots were tried
	// first and read as body parts, not strikes). The pictures are drawn
	// striking to the right, for the right hand; the left box mirrors them, as
	// the doll mirrors its hand.
	if (slot.Empty() && !use.verb.empty() && useIcons) {
		if (const gfx::Texture* pic = useIcons->For(use.verb)) {
			const bool flip = m_hand == 0;
			batch.DrawSprite(inner, flip ? gfx::Rect{1, 0, -1, 1} : gfx::Rect{0, 0, 1, 1},
							 *pic, {1, 1, 1, 1});
		}
	}
	// A spell use spells out its recipe on top.
	if (use.spell) DrawSpellRunes(batch, inner, *use.spell, drewItem);
	// No identity stripe: whose hand this is reads from the member border its
	// HandPair draws round both hands and the effort meter (ui-updates).
	if (m_hot)
		ui::DrawBorder(batch, px, theme.accent);
	else if (!skinned)
		ui::DrawBorder(batch, px, theme.panelBorder);
}

// The hover tooltip: what a left click on this SET hand does. An unset hand
// shows none (Michael, 2026-09-28) - its left click still acts, but nothing
// was chosen, so there is nothing to name. One line, drawn every frame the
// pointer rests here, from an inline loc::Line, so it builds no string.
void HandSlot::DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_hot || !m_character || !setUse) return;
	const HandSetUse use = setUse();
	if (!use.set || use.label.empty()) return;
	const ui::Font& font = TextFont();
	const ui::Theme& theme = ctx.GetTheme();
	const float padX = Em(0.6f), padY = Em(0.35f), gapY = Em(0.3f);
	const float w = font.MeasureWidth(use.label) + 2.0f * padX;
	const float h = font.Height() + 2.0f * padY;
	// NEVER OVER THE HAND. Below it by preference, above when that would run
	// off the screen, and always wholly on it (ui::PlaceTooltip).
	const gfx::Rect tip = ui::PlaceTooltip(Pixel(), w, h, {0, 0, ctx.Width(), ctx.Height()},
										   ui::TipSide::Below, gapY, padX);
	// Near-opaque: it sits over the other hands and the world view.
	batch.DrawRect(tip, {0.10f, 0.10f, 0.13f, 0.97f});
	ui::DrawBorder(batch, tip, theme.panelBorder);
	font.Draw(batch, use.label, tip.x + padX, tip.y + padY, theme.text);
}

void HandSlot::DrawSpellRunes(gfx::SpriteBatch& batch, const gfx::Rect& area,
							  const Spell& spell, bool overItem) const {
	const std::span<const SpellSymbol> runes = spell.Sequence();
	const size_t n = runes.size();
	if (n == 0) return;
	const float gap = Em(0.12f);
	// ROWS OF TWO, every rune the size two side by side leave it (Michael,
	// 2026-09-30): a third and fourth rune go on the next row rather than
	// shrinking the first two into a strip. Sized for two across even when there
	// is one, so a lone rune never swells into what reads as HOLDING a tablet.
	// Over an item a one-row recipe sits along the bottom, out of its way; a
	// grid in an empty hand is centred.
	constexpr size_t kCols = 2;
	const size_t cols = std::min(n, kCols);
	const size_t rows = (n + kCols - 1) / kCols;
	const float fitRows = static_cast<float>(std::max<size_t>(rows, kCols));
	const float side = std::max(
		0.0f, std::min((area.w - gap * (kCols - 1)) / static_cast<float>(kCols),
					   (area.h - gap * (fitRows - 1.0f)) / fitRows));
	// The block of runes actually drawn, centred horizontally; a strip sits on
	// the area's bottom edge, a grid is centred vertically too.
	const size_t usedCols = cols;
	const float blockW = side * static_cast<float>(usedCols) + gap * static_cast<float>(usedCols - 1);
	const float blockH = side * static_cast<float>(rows) + gap * static_cast<float>(rows - 1);
	const float x0 = area.x + (area.w - blockW) * 0.5f;
	const float y0 = overItem ? area.y + area.h - blockH : area.y + (area.h - blockH) * 0.5f;
	for (size_t k = 0; k < n; ++k) {
		const size_t c = k % cols, r = k / cols;
		DrawRuneFace(batch,
					 {x0 + static_cast<float>(c) * (side + gap),
					  y0 + static_cast<float>(r) * (side + gap), side, side},
					 runes[k], m_icons, /*hot=*/false, /*disabled=*/false,
					 /*background=*/false); // the set tint shows behind it
	}
}

} // namespace dungeon::game
