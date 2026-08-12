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

// The one warning colour this tab uses: an armor penalty, or a strength it
// cannot carry.
constexpr Vec4 kDefBad{0.85f, 0.25f, 0.20f, 1.0f};
// The comparison colours: better than what is worn, and worse than it.
constexpr Vec4 kTipGood{0.45f, 0.80f, 0.40f, 1.0f};
constexpr Vec4 kTipBad{0.85f, 0.30f, 0.25f, 1.0f};
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

	// What the pointer is over, for the armor tooltip. Tracked every frame
	// rather than on a click — a tooltip that needed clicking would not be one.
	// Nothing is CONSUMED here: hovering must not steal the click that a slot
	// is about to want.
	m_hoverDoll = m_hoverPack = -1;
	for (int i = 0; i < kDollCellCount; ++i)
		if (EquipRect(px, i).Contains(mx, my)) { m_hoverDoll = i; break; }
	if (m_hoverDoll < 0) {
		const auto& contents = m_character->inventory.SelectedContents();
		for (int i = 0; i < static_cast<int>(contents.size()); ++i)
			if (PackRect(px, i).Contains(mx, my)) { m_hoverPack = i; break; }
	}

	// Item slots are only live (and only hit-tested) in Inventory mode.
	if (clicked && !ctx.IsMouseConsumed()) {
		for (int i = 0; i < kDollCellCount; ++i)
			if (EquipRect(px, i).Contains(mx, my)) {
				const size_t s = static_cast<size_t>(kDollCells[i].slot);
				// EVERY doll slot is now checked, not just the hands. A hand
				// takes anything `holdable`; the rest are type-specific, so a
				// sword cannot be worn as a hat (Inventory.h). A refused item
				// stays on the cursor and says so — the same path the hands
				// already used for a non-holdable.
				if (m_held && m_held->has_value() && m_categories &&
					!m_categories->FitsSlot(**m_held, kDollCells[i].slot)) {
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
	const ui::Font& font = TextFont();

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

// ============================================================================
// The armor tooltip.
//
// Hovering a WORN piece explains the defense it gives — the breakdown that was
// briefly a column of the sheet. Hovering one in the PACK sets that breakdown
// beside the worn one, so a piece can be judged without putting it on, taking
// it off again, and trying to remember two sets of numbers.
//
// COLOUR IS THE COMPARISON: on the hovered column, better than what is worn is
// green, worse is red, the same is ordinary text. Which direction is "better"
// is per ROW and not global — more soak is good, more armor penalty is not.
// ============================================================================
void CharacterSheet::DrawArmorTip(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								  const gfx::Rect& px) const {
	if (!m_character || !defenseFor) return;
	if (m_hoverDoll < 0 && m_hoverPack < 0) return;

	// What is being hovered, and is it armor at all? A tooltip about a rune or
	// an empty slot would be noise.
	std::string hoveredId;
	gfx::Rect anchor{};
	bool comparing = false;
	if (m_hoverDoll >= 0) {
		const size_t slot = static_cast<size_t>(kDollCells[m_hoverDoll].slot);
		hoveredId = m_character->inventory.equipment[slot].typeId;
		anchor = EquipRect(px, m_hoverDoll);
	} else {
		const auto& contents = m_character->inventory.SelectedContents();
		if (m_hoverPack >= static_cast<int>(contents.size())) return;
		hoveredId = contents[static_cast<size_t>(m_hoverPack)].typeId;
		anchor = PackRect(px, m_hoverPack);
		comparing = true;
	}
	if (hoveredId.empty()) return;
	if (!m_categories || m_categories->WornAt(hoveredId) == WearSlot::None) return;

	// The piece currently in the SAME slot the hovered one would go to — that
	// is what it is really being compared against, and its icon heads the left
	// column.
	std::string wornId;
	if (m_categories) {
		const WearSlot wear = m_categories->WornAt(hoveredId);
		for (int i = 0; i < kEquipCount; ++i)
			if (WearSlotFits(wear, static_cast<EquipSlot>(i))) {
				wornId = m_character->inventory.equipment[static_cast<size_t>(i)].typeId;
				break;
			}
	}

	const DefenseReadout now = defenseFor(*m_character);
	const DefenseReadout with =
		comparing && defenseWith ? defenseWith(*m_character, hoveredId) : now;
	// Hovering the piece already worn compares it with itself, which is just
	// the single-column form.
	if (comparing && !defenseWith) return;

	const ui::Font& font = TextFont();
	const ui::Theme& theme = ctx.GetTheme();
	const float rem = Rem();
	const float pad = kTipPadRem * rem, row = kTipRowRem * rem;

	struct Row {
		std::string label, left, right;
		float lv = 0.0f, rv = 0.0f;
		bool higherBetter = true;
		bool compare = true; // false = a fact, not a score
	};
	// Rounds toward zero BEFORE formatting, or a term of -0.4 prints "-0" —
	// the same trap the sheet column had, reintroduced here because this is a
	// second formatter and it did not inherit the fix.
	const auto pts = [](float v) {
		const int n = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
		return std::format("{}{}", n >= 0 ? "+" : "", n);
	};
	std::vector<Row> rows;
	const auto nameOf = [&](const DefenseReadout& d) {
		return d.armorClass == ArmorClass::None ? loc::Tr("sheet.def.unarmored")
												: (d.armorName.empty()
													   ? std::string(ArmorClassId(d.armorClass))
													   : d.armorName);
	};
	rows.push_back({loc::Tr("sheet.def.armor"), nameOf(now), nameOf(with), 0, 0,
					true, false});
	rows.push_back({loc::Tr("sheet.def.soak"), std::format("{:.1f}", now.soak),
					std::format("{:.1f}", with.soak), now.soak, with.soak, true});
	rows.push_back({loc::Tr("sheet.def.roll"), std::format("{:.0f}", now.total),
					std::format("{:.0f}", with.total), now.total, with.total, true});
	rows.push_back({loc::Tr("sheet.def.base"), pts(now.base), pts(with.base),
					now.base, with.base, true});
	rows.push_back({loc::Tr("sheet.def.dex"), pts(now.stat), pts(with.stat),
					now.stat, with.stat, true});
	rows.push_back({loc::Tr("sheet.def.stance"), pts(now.stance), pts(with.stance),
					now.stance, with.stance, true});
	// The armor term is a COST: less of it is better, so its polarity flips.
	rows.push_back({loc::Tr("sheet.def.armorpen"), pts(-now.armorPenalty),
					pts(-with.armorPenalty), -now.armorPenalty, -with.armorPenalty,
					true});
	if (now.strengthNeeded > 0 || with.strengthNeeded > 0) {
		// Unarmored asks for no strength at all, and "16 / 0" reads as a
		// requirement of zero rather than as no requirement.
		const auto strOf = [](const DefenseReadout& d) {
			return d.strengthNeeded > 0
					   ? std::format("{} / {}", d.strength, d.strengthNeeded)
					   : std::string("-");
		};
		rows.push_back({loc::Tr("sheet.def.str"), strOf(now), strOf(with),
						static_cast<float>(-now.strengthNeeded),
						static_cast<float>(-with.strengthNeeded), true});
	}

	// Size from the content, then place. WIDTH: label + one or two values.
	const float labelW = kTipLabelRem * rem, valueW = kTipValueRem * rem;
	const float w = pad * 2.0f + labelW +
					(comparing ? valueW * 2.0f + kTipGapRem * rem : valueW);
	// The heading row carries ICONS rather than the words "worn" and "this" —
	// the pieces name themselves, and a picture of the thing under the pointer
	// is a faster answer to "which column is which" than a caption.
	const float iconSize = kTipIconRem * rem;
	const float headH = iconSize + rem * 0.25f;
	const float h = pad * 2.0f + headH + row * static_cast<float>(rows.size());

	// NEVER OVER THE ITEM. Below it by preference, above when that would run
	// off the screen — the thing under the pointer is what the tooltip is
	// about, and covering it would answer a question by hiding it. (The same
	// rule the dev console's tooltips use.)
	const float screenW = ctx.Width(), screenH = ctx.Height();
	float tx = anchor.x;
	if (tx + w > screenW - pad) tx = screenW - pad - w;
	if (tx < pad) tx = pad;
	float ty = anchor.y + anchor.h + rem * 0.3f;
	if (ty + h > screenH - pad) ty = anchor.y - h - rem * 0.3f;
	if (ty < 0.0f) ty = anchor.y + anchor.h + rem * 0.3f; // neither fits: below
	const gfx::Rect tip{tx, ty, w, h};

	// Near-opaque: it sits over a busy grid, and a translucent panel would
	// leave the icons behind it legible through the numbers in front.
	batch.DrawRect(tip, {0.10f, 0.10f, 0.13f, 0.97f});
	ui::DrawBorder(batch, tip, theme.panelBorder);

	float y = tip.y + pad;
	const float lx = tip.x + pad;
	const float v1 = lx + labelW;
	const float v2 = v1 + valueW + kTipGapRem * rem;

	// Heading: the label column keeps its title, the value columns show the
	// PIECES. An empty slot has no icon to show, which reads correctly as
	// "nothing there" without needing to say so.
	font.Draw(batch, loc::Tr("sheet.defense"), lx,
			  y + (headH - font.Height()) * 0.5f, theme.accent);
	const auto icon = [&](float x, const std::string& id) {
		if (id.empty() || !m_icons) return;
		if (const gfx::Texture* t = m_icons->For(id))
			batch.DrawSprite({x, y, iconSize, iconSize}, {0, 0, 1, 1}, *t,
							 {1, 1, 1, 1});
	};
	if (comparing) {
		icon(v1, wornId);
		icon(v2, hoveredId);
	} else {
		icon(v1, hoveredId);
	}
	y += headH;

	for (const Row& r : rows) {
		font.Draw(batch, r.label, lx, y, theme.textDim);
		font.Draw(batch, r.left, v1, y, theme.text);
		if (comparing) {
			// Better green, worse red, identical ordinary — and a row that is
			// a FACT rather than a score (the piece's name) never colours.
			// Compared at the precision SHOWN, not the precision stored: the
			// stance term is derived by subtraction, so two identical stances
			// differ in the last float bit and coloured green for nothing.
			Vec4 c = theme.text;
			if (r.compare && std::fabs(r.rv - r.lv) >= 0.5f)
				c = (r.rv > r.lv) == r.higherBetter ? kTipGood : kTipBad;
			font.Draw(batch, r.right, v2, y, c);
		}
		y += row;
	}
}

} // namespace dungeon::game
