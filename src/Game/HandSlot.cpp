// ============================================================================
// Game/HandSlot.cpp — see HandSlot.h.
// ============================================================================
#include "Game/HandSlot.h"

#include "Game/PartyHudDraw.h"
#include "UI/Skin.h"

#include <algorithm>

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
		const float in = 4.0f;
		socket = {px.x + in, px.y + in, px.w - 2 * in, px.h - 2 * in};
	}
	// A subtle grey lift on hover/press keeps the interaction feedback.
	batch.DrawRect(socket, m_held ? Vec4{0.22f, 0.22f, 0.24f, 1.0f}
								  : (m_hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg));
	if (framed) // the ring draws over the fill; its open middle shows the socket
		ui::DrawNineSlice(batch, px, skin->slot, {1, 1, 1, 1});
	// The item held in this hand, if any, drawn inset from the border.
	const ItemSlot& slot = m_character->inventory.Hand(m_hand);
	if (!slot.Empty() && m_icons) {
		if (const gfx::Texture* icon = m_icons->For(slot.typeId)) {
			const float pad = px.w * 0.12f;
			batch.DrawSprite({px.x + pad, px.y + pad, px.w - 2 * pad, px.h - 2 * pad},
							 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
		}
	}
	// Identity stripe along the socket's bottom edge.
	batch.DrawRect({socket.x + 1, socket.y + socket.h - 4, socket.w - 2, 3},
				   m_character->portraitColor);
	if (m_hot)
		ui::DrawBorder(batch, px, theme.accent);
	else if (!skinned)
		ui::DrawBorder(batch, px, theme.panelBorder);
}

} // namespace dungeon::game
