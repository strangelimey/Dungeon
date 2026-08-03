// ============================================================================
// Game/InventoryWindow.cpp — see InventoryWindow.h.
//
// Layout is parent-relative: the panel is a fraction of the window; slots are
// fractions of the panel. No design-pixel artboard.
// ============================================================================
#include "Game/InventoryWindow.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Panel as fractions of the window.
constexpr float kPanelW = 0.72f;
constexpr float kPanelH = 0.54f;
// Interior as fractions of the panel.
constexpr float kPad = 0.025f;
constexpr float kHeaderH = 0.055f; // title band
constexpr float kNameH = 0.045f;
constexpr float kGap = 0.015f;
constexpr int kInvCols = 2;
} // namespace

InventoryWindow::InventoryWindow(std::vector<Character>* roster,
								 const ItemIconBank* icons,
								 std::optional<std::string>* held)
	: m_roster(roster), m_icons(icons), m_held(held),
	  m_title(loc::Tr("ui.inv_all")) {}

int InventoryWindow::MemberCount() const {
	return static_cast<int>(std::min<size_t>(m_roster->size(), 4));
}

gfx::Rect InventoryWindow::PanelRect(const ui::UIContext& ctx) const {
	const float w = ctx.Width() * kPanelW;
	const float h = ctx.Height() * kPanelH;
	return {(ctx.Width() - w) * 0.5f, (ctx.Height() - h) * 0.5f, w, h};
}

gfx::Rect InventoryWindow::SlotRect(const gfx::Rect& panel, int member,
									int slot) const {
	const float pad = kPad * panel.w;
	const float gap = kGap * panel.w;
	const float colW = (panel.w - 2 * pad) / static_cast<float>(MemberCount());
	const float colX = panel.x + pad + static_cast<float>(member) * colW;
	const float slotsTop =
		panel.y + kPad * panel.h + kHeaderH * panel.h + kNameH * panel.h;
	const float innerW = colW - 2 * gap;
	const float slotW = (innerW - gap) / static_cast<float>(kInvCols);
	const int sc = slot % kInvCols, row = slot / kInvCols;
	return {colX + gap + static_cast<float>(sc) * (slotW + gap),
			slotsTop + static_cast<float>(row) * (slotW + gap), slotW, slotW};
}

void InventoryWindow::UpdateSelf(ui::UIContext& ctx) {
	if (!m_open) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const gfx::Rect panel = PanelRect(ctx);
	const bool left = input->WasMousePressed(MouseButton::Left);
	const bool right = input->WasMousePressed(MouseButton::Right);
	const float mx = input->MouseX(), my = input->MouseY();

	if (left) {
		for (int m = 0; m < MemberCount(); ++m) {
			auto& pack = (*m_roster)[static_cast<size_t>(m)].inventory.SelectedContents();
			for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
				if (!SlotRect(panel, m, i).Contains(mx, my)) continue;
				ItemSlot& s = pack[static_cast<size_t>(i)];
				if (m_held && m_held->has_value()) {
					std::string incoming = **m_held;
					if (s.Empty()) m_held->reset();
					else *m_held = s.typeId;
					s.typeId = std::move(incoming);
				} else if (!s.Empty()) {
					*m_held = s.typeId;
					s.Clear();
				}
				ctx.ConsumeMouse();
				return;
			}
		}
	}
	if ((left || right) && !panel.Contains(mx, my)) m_open = false;
	ctx.ConsumeMouse();
}

void InventoryWindow::DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	batch.DrawRect({0, 0, ctx.Width(), ctx.Height()}, {0, 0, 0, 0.5f});
	const gfx::Rect panel = PanelRect(ctx);
	ui::DrawPanelFace(ctx, batch, panel);
	const float padX = kPad * panel.w, padY = kPad * panel.h;
	font.Draw(batch, m_title, panel.x + padX, panel.y + padY, theme.accent);

	const float colW = (panel.w - 2 * padX) / static_cast<float>(MemberCount());
	for (int m = 0; m < MemberCount(); ++m) {
		const auto& pack =
			(*m_roster)[static_cast<size_t>(m)].inventory.SelectedContents();
		const float colX = panel.x + padX + static_cast<float>(m) * colW;
		font.Draw(batch, (*m_roster)[static_cast<size_t>(m)].name, colX + padX * 0.3f,
				  panel.y + padY + kHeaderH * panel.h, theme.text);
		for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
			const gfx::Rect r = SlotRect(panel, m, i);
			batch.DrawRect(r, kSlotBg);
			ui::DrawBorder(batch, r, theme.panelBorder);
			const ItemSlot& s = pack[static_cast<size_t>(i)];
			if (!s.Empty() && m_icons) {
				if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
					const float p = r.w * 0.1f;
					batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p},
									 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
				}
			}
		}
	}
}

} // namespace dungeon::game
