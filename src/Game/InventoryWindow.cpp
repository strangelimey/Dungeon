// ============================================================================
// Game/InventoryWindow.cpp — see InventoryWindow.h.
//
// Layout is parent-relative: the window fills its floating panel, and the slots
// are fractions of the window. No design-pixel artboard.
// ============================================================================
#include "Game/InventoryWindow.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"
#include "UI/Layout.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Interior as fractions of the window.
constexpr float kPad = 0.025f;
constexpr float kHeaderH = 0.055f; // title band
constexpr float kNameH = 0.045f;
constexpr float kGap = 0.015f;
constexpr int kInvCols = 2;
// The close box's slot in the top-right corner, about square at the window's
// 0.72 x 0.54 proportions.
constexpr float kCloseW = 0.036f, kCloseH = 0.085f;
} // namespace

InventoryWindow::InventoryWindow(std::vector<Character>* roster,
								 const ItemIconBank* icons,
								 std::optional<std::string>* held,
								 const gfx::Texture* closeIcon,
								 std::function<void()> onClose)
	: m_roster(roster), m_icons(icons), m_held(held),
	  m_title(loc::Tr("ui.inv_all")) {
	debugName = "InventoryWindow";
	auto* closeSlot = Add<ui::Box>(
		gfx::Rect{1.0f - kCloseW - kPad * 0.6f, kPad, kCloseW, kCloseH});
	closeSlot->debugName = "close";
	ui::AddCloseButton(*closeSlot, closeIcon, std::move(onClose));
}

int InventoryWindow::MemberCount() const {
	return static_cast<int>(std::min<size_t>(m_roster->size(), 4));
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

// The close box (a child) has had the pointer first. Everything here acts only
// INSIDE the window: it floats over a running game, so a click beside it is
// the world's.
void InventoryWindow::UpdateSelf(ui::UIContext& ctx) {
	if (!m_open) return;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const gfx::Rect panel = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!panel.Contains(mx, my)) return;
	const bool left = input->WasMousePressed(MouseButton::Left);
	const bool right = input->WasMousePressed(MouseButton::Right);
	const bool middle = input->WasMousePressed(MouseButton::Middle);

	// Right = details, middle = use menu, on a non-empty slot. The window stays
	// open under either, so the item is still there to act on afterwards.
	if (right || middle) {
		for (int m = 0; m < MemberCount(); ++m) {
			const auto& pack =
				(*m_roster)[static_cast<size_t>(m)].inventory.SelectedContents();
			for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
				if (!SlotRect(panel, m, i).Contains(mx, my)) continue;
				if (!pack[static_cast<size_t>(i)].Empty()) {
					const auto& fire = right ? onItemDetails : onItemUse;
					if (fire) fire(static_cast<size_t>(m), i);
				}
				ctx.ConsumeMouse();
				ctx.ConsumeWheel();
				return;
			}
		}
	}

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
	// Not a slot: the window's background, which its floating panel takes - a
	// press there moves it.
}

void InventoryWindow::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const gfx::Rect panel = Pixel();
	ui::DrawPanelFace(ctx, batch, panel, opacity ? *opacity : 1.0f);
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
			ui::DrawSlotFace(ctx, batch, r, kSlotBg);
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
