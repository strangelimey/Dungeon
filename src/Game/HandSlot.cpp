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
				   std::function<void()> onRight)
	: m_roster(roster), m_member(member), m_hand(hand), m_icons(icons),
	  m_onLeft(std::move(onLeft)), m_onRight(std::move(onRight)) {
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
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && m_onLeft) m_onLeft();
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot && m_onRight) m_onRight();
		m_heldRight = false;
	}
}

void HandSlot::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	if (!m_character) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();
	// Skinned: prefer the dedicated SOCKET FRAME part (an open-centred ring
	// with transparent middle texels, drawn OVER the socket fill), falling
	// back to the button part (opaque face, the socket inset into it). The
	// black socket stays in every mode — the light-haloed item icons read
	// against it.
	const ui::Skin* skin = ctx.GetSkin();
	const bool framed = skin && skin->slot.texture;
	const bool skinned = framed || (skin && skin->button.texture);
	gfx::Rect socket = px;
	if (framed) {
		// Content lives inside the frame ring; the fill overlaps the ring by a
		// couple of px so no seam shows at the hole's antialiased edge.
		const float ring = skin->slot.corner * skin->slot.scale;
		const float in = std::max(2.0f, ring - 2.0f);
		socket = {px.x + in, px.y + in, px.w - 2 * in, px.h - 2 * in};
	} else if (skinned) {
		ui::DrawNineSlice(batch, px, skin->button, {1, 1, 1, 1});
		const float in = Rem(0.24f);
		socket = {px.x + in, px.y + in, px.w - 2 * in, px.h - 2 * in};
	}
	// A subtle grey lift on hover/press keeps the interaction feedback.
	Vec4 fill = m_held ? Vec4{0.22f, 0.22f, 0.24f, 1.0f}
					   : (m_hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg);
	// A SET hand says so: the socket behind the icon takes the theme accent.
	// Mixed to an opaque colour rather than drawn translucent over the fill, so
	// it reads the same whatever the batch's blend mode, and the hover/press
	// lift still shows through it.
	const HandSetUse use = setUse ? setUse() : HandSetUse{};
	if (use.set) {
		constexpr float kTint = 0.35f;
		fill = {fill.x + (theme.accent.x - fill.x) * kTint,
				fill.y + (theme.accent.y - fill.y) * kTint,
				fill.z + (theme.accent.z - fill.z) * kTint, 1.0f};
	}
	batch.DrawRect(socket, fill);
	if (framed) // the ring draws over the fill; its open middle shows the socket
		ui::DrawNineSlice(batch, px, skin->slot, {1, 1, 1, 1});
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
	// Identity stripe along the socket's bottom edge.
	batch.DrawRect({socket.x + 1, socket.y + socket.h - 4, socket.w - 2, 3},
				   m_character->portraitColor);
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
	const float padX = Rem(0.6f), padY = Rem(0.35f), gapY = Rem(0.3f);
	const float w = font.MeasureWidth(use.label) + 2.0f * padX;
	const float h = font.Height() + 2.0f * padY;
	// NEVER OVER THE HAND. Below it by preference, above when that would run
	// off the screen, and pulled in from the right edge the column sits on (the
	// sheet's armor tooltip and the dev console's follow the same rule).
	const gfx::Rect& px = Pixel();
	const float screenW = ctx.Width(), screenH = ctx.Height();
	float tx = px.x + (px.w - w) * 0.5f;
	if (tx + w > screenW - padX) tx = screenW - padX - w;
	if (tx < padX) tx = padX;
	float ty = px.y + px.h + gapY;
	if (ty + h > screenH - padY) ty = px.y - h - gapY;
	const gfx::Rect tip{tx, ty, w, h};
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
	const float gap = Rem(0.12f);
	// Over an item the runes keep out of its way: one strip along the bottom,
	// sized as if four sat there so a short recipe does not balloon. In an
	// empty hand they ARE the content, so they fill it: one big rune, two side
	// by side, then rows of two (three across past four).
	size_t cols = 0, rows = 0;
	if (overItem) {
		cols = std::max<size_t>(n, 4);
		rows = 1;
	} else {
		cols = n <= 2 ? n : (n <= 4 ? 2 : 3);
		rows = (n + cols - 1) / cols;
	}
	float side = std::max(
		0.0f, std::min((area.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols),
					   (area.h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows)));
	// A lone rune filling the box would read as HOLDING a rune tablet, and would
	// hide the set tint - so a grid rune never exceeds 60% of the box.
	if (!overItem) side = std::min(side, 0.6f * std::min(area.w, area.h));
	// The block of runes actually drawn, centred horizontally; a strip sits on
	// the area's bottom edge, a grid is centred vertically too.
	const size_t usedCols = std::min(n, cols);
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
