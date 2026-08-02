// ============================================================================
// Game/CharacterSheet_Inventory.cpp — paper doll, packs, contents grid.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"

#include "Core/Loc.h"

#include <algorithm>
#include <format>

namespace dungeon::game {
using namespace sheet;

gfx::Rect CharacterSheet::EquipRect(const gfx::Rect& px, int i) const {
	const DollCell c = kDollCells[i];
	return At(px, kLeft + c.col * kDollStepX, kBodyTop + c.row * kDollStepY,
			  kEquipW, kEquipH);
}

gfx::Rect CharacterSheet::PackRect(const gfx::Rect& px, int i) const {
	const float x =
		kPackX + static_cast<float>(i % kPackCols) * (kPackW + kPackGapX);
	const float y =
		kPackY + static_cast<float>(i / kPackCols) * (kPackH + kPackGapY);
	return At(px, x, y, kPackW, kPackH);
}
gfx::Rect CharacterSheet::PackRowRect(const gfx::Rect& px, int i) const {
	const float x = kPackX + static_cast<float>(i) * (kPackW + kPackGapX);
	return At(px, x, kPackRowY, kPackW, kPackH);
}
void CharacterSheet::ClickSlot(ItemSlot& slot) {
	if (!m_held) return;
	if (m_held->has_value()) {
		std::string incoming = **m_held; // place; any occupant returns to cursor
		if (slot.Empty()) m_held->reset();
		else *m_held = slot.typeId;
		slot.typeId = std::move(incoming);
	} else if (!slot.Empty()) {
		*m_held = slot.typeId; // pick the slot's item up onto the cursor
		slot.Clear();
	}
}
void CharacterSheet::EquipOrSelectPack(int i) {
	if (!m_character) return;
	Inventory& inv = m_character->inventory;
	Pack& slot = inv.packs[static_cast<size_t>(i)];
	if (m_held && m_held->has_value()) {
		// Only a CONTAINER can be equipped into a pack slot. Holding a non-pack,
		// a click just SELECTS the pack (so you can drop the item into its grid).
		if (!m_categories || !m_categories->Is(**m_held, "container")) {
			if (!slot.Empty()) inv.selectedPack = i;
			return;
		}
		// Refuse to drop onto a pack that holds items (its contents would be lost).
		if (slot.HasItems()) return;
		std::string incoming = **m_held;
		// Fresh capacity from the catalog (this pack type's content slots).
		int cap = m_categories->Capacity(incoming);
		if (cap <= 0) cap = kBackpackStart;
		if (slot.Empty()) m_held->reset();   // equip into an empty slot
		else *m_held = slot.typeId;           // swap the (empty) pack onto the cursor
		slot.typeId = std::move(incoming);
		slot.contents.assign(static_cast<size_t>(cap), {});
		inv.selectedPack = i;                 // view the newly equipped pack
	} else if (!slot.Empty()) {
		inv.selectedPack = i; // empty-handed: select this pack
	}
}
bool CharacterSheet::PackAccepts(const std::string& itemId) const {
	if (!m_categories || !m_character) return true; // no info → don't block
	const Inventory& inv = m_character->inventory;
	const std::string& packId = inv.packs[static_cast<size_t>(inv.selectedPack)].typeId;
	return m_categories->PackAcceptsItem(packId, itemId);
}
float CharacterSheet::CarryLoad() const {
	if (!m_character || !m_weights) return 0.0f;
	float total = 0.0f;
	for (const ItemSlot& s : m_character->inventory.equipment)
		if (!s.Empty()) total += m_weights->For(s.typeId);
	for (const Pack& p : m_character->inventory.packs) {
		if (!p.Empty()) total += m_weights->For(p.typeId); // the bag itself
		for (const ItemSlot& s : p.contents)               // and everything in it
			if (!s.Empty()) total += m_weights->For(s.typeId);
	}
	return total;
}

void CharacterSheet::UpdateInventory(ui::UIContext& ctx, const gfx::Rect& px,
									 float mx, float my, bool clicked) {
	if (!m_character) return;

	// Item slots are only live (and only hit-tested) in Inventory mode.
	if (clicked && !ctx.IsMouseConsumed()) {
		for (int i = 0; i < kDollCellCount; ++i)
			if (EquipRect(px, i).Contains(mx, my)) {
				const size_t s = static_cast<size_t>(kDollCells[i].slot);
				// The two weapon hands only take catalog-holdable items; a held
				// non-holdable stays on the cursor and we signal the refusal.
				const bool handCell = kDollCells[i].slot == EquipSlot::LeftHand ||
									  kDollCells[i].slot == EquipSlot::RightHand;
				if (handCell && m_held && m_held->has_value() && m_categories &&
					!m_categories->Holdable(**m_held)) {
					if (onRejectHold) onRejectHold(**m_held);
				} else {
					ClickSlot(m_character->inventory.equipment[s]);
				}
				ctx.ConsumeMouse();
				return;
			}
		// Pack row: equip a held container, or select a pack empty-handed.
		for (int i = 0; i < kPackRowSlots; ++i)
			if (PackRowRect(px, i).Contains(mx, my)) {
				EquipOrSelectPack(i);
				ctx.ConsumeMouse();
				return;
			}
		auto& pack = m_character->inventory.SelectedContents();
		for (int i = 0; i < static_cast<int>(pack.size()); ++i)
			if (PackRect(px, i).Contains(mx, my)) {
				const bool dropping = m_held && m_held->has_value();
				if (dropping && !PackAccepts(**m_held)) {
					const Inventory& inv = m_character->inventory;
					if (onRejectDrop)
						onRejectDrop(**m_held,
									 inv.packs[static_cast<size_t>(inv.selectedPack)].typeId);
				} else {
					ClickSlot(pack[static_cast<size_t>(i)]);
				}
				ctx.ConsumeMouse();
				return;
			}
	}
	// A RIGHT-click on a non-empty backpack slot opens its use menu.
	const Input* input = ctx.CurrentInput();
	if (input && !ctx.IsMouseConsumed() &&
		input->WasMousePressed(MouseButton::Right)) {
		const auto& pack = m_character->inventory.SelectedContents();
		for (int i = 0; i < static_cast<int>(pack.size()); ++i)
			if (PackRect(px, i).Contains(mx, my) &&
				!pack[static_cast<size_t>(i)].Empty()) {
				if (onSlotMenu) onSlotMenu(i);
				break;
			}
	}
}

void CharacterSheet::DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								   const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();

	// --- equipment paper doll (left) ----------------------------------------
	for (int i = 0; i < kDollCellCount; ++i) {
		const size_t slot = static_cast<size_t>(kDollCells[i].slot);
		const gfx::Rect r = EquipRect(px, i);
		batch.DrawRect(r, kSlotBg);
		ui::DrawBorder(batch, r, theme.panelBorder);
		const ItemSlot& s = m_character->inventory.equipment[slot];
		if (s.Empty()) {
			if (m_slotIcons) {
				if (const gfx::Texture* o = m_slotIcons->For(kEquipIcon[slot])) {
					const float p = r.w * 0.12f;
					// The hand silhouette is a right hand; mirror for left-hand.
					const bool flip = kDollCells[i].slot == EquipSlot::LeftHand;
					const gfx::Rect uv = flip ? gfx::Rect{1, 0, -1, 1}
											  : gfx::Rect{0, 0, 1, 1};
					batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, uv,
									 *o, {1, 1, 1, 0.5f});
				}
			}
		} else if (m_icons) {
			if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
				const float p = r.w * 0.1f;
				batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
								 *icon, {1, 1, 1, 1});
			}
		}
	}

	// --- backpack (right) — carry load stands in for a "Backpack" header ----
	const float load = CarryLoad();
	const float maxLoad = m_character->MaxCarryLoad();
	const std::string loadText = loc::Format(
		"sheet.load", std::format("{:.1f}", load), std::format("{:.0f}", maxLoad));
	const Vec4 loadColor = load > maxLoad ? Vec4{0.85f, 0.25f, 0.2f, 1.0f} : theme.accent;
	font.Draw(batch, loadText, Ax(px, kPackX), Ay(px, kHeaderY), loadColor);

	const Inventory& inv = m_character->inventory;
	auto drawIcon = [&](const gfx::Rect& r, const std::string& typeId) {
		if (typeId.empty() || !m_icons) return;
		if (const gfx::Texture* icon = m_icons->For(typeId)) {
			const float p = r.w * 0.1f;
			batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
							 *icon, {1, 1, 1, 1});
		}
	};
	for (int i = 0; i < kPackRowSlots; ++i) {
		const gfx::Rect r = PackRowRect(px, i);
		const bool sel = i == inv.selectedPack;
		batch.DrawRect(r, sel ? Vec4{0.18f, 0.18f, 0.20f, 1.0f} : kSlotBg);
		ui::DrawBorder(batch, r, sel ? theme.accent : theme.panelBorder);
		drawIcon(r, inv.packs[static_cast<size_t>(i)].typeId);
	}

	// Divider rule between the pack row and its contents (1px tall chrome).
	const float gridW = static_cast<float>(kPackCols) * kPackW +
						static_cast<float>(kPackCols - 1) * kPackGapX;
	batch.DrawRect({Ax(px, kPackX), Ay(px, kPackSepY), gridW * px.w, 1.0f},
				   theme.panelBorder);

	const auto& pack = inv.SelectedContents();
	for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
		const gfx::Rect r = PackRect(px, i);
		batch.DrawRect(r, kSlotBg);
		ui::DrawBorder(batch, r, theme.panelBorder);
		drawIcon(r, pack[static_cast<size_t>(i)].typeId);
	}
}

} // namespace dungeon::game
