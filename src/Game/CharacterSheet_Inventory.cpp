// ============================================================================
// Game/CharacterSheet_Inventory.cpp — paper doll, packs, contents grid.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"

#include "Core/Loc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

namespace dungeon::game {

// The one warning colour this tab uses: an armor penalty, or a strength it
// cannot carry.
constexpr Vec4 kDefBad{0.85f, 0.25f, 0.20f, 1.0f};
// The comparison colours: better than what is worn, and worse than it.
constexpr Vec4 kTipGood{0.45f, 0.80f, 0.40f, 1.0f};
constexpr Vec4 kTipBad{0.85f, 0.30f, 0.25f, 1.0f};
using namespace sheet;

namespace {
// What the armor tooltip names when nothing is worn in the compared slot.
const std::string kNoItem;

// One armor-tooltip value, formatted in place (the tooltip draws every frame
// it is up). Callers use whole-number arithmetic only: MSVC's float-precision
// path ("{:.1f}") allocates in the debug build.
struct Cell {
	char text[48] = {};
	size_t len = 0;
	std::string_view View() const { return {text, len}; }
	void Set(std::string_view s) {
		len = std::min(s.size(), sizeof(text));
		std::copy_n(s.data(), len, text);
	}
	template <class... Args>
	void Format(std::format_string<Args...> fmt, Args&&... args) {
		len = static_cast<size_t>(
			std::format_to_n(text, sizeof(text), fmt, std::forward<Args>(args)...).out -
			text);
	}
};
} // namespace

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
	// Place (any occupant returns to the cursor) or pick up: one exchange,
	// which allocates nothing (see HeldItem).
	if (m_held->has_value() || !slot.Empty()) m_held->SwapWith(slot);
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
		// Fresh capacity from the catalog (this pack type's content slots).
		int cap = m_categories->Capacity(**m_held);
		if (cap <= 0) cap = kBackpackStart;
		// Equip into an empty slot, or swap the (empty) pack onto the cursor.
		m_held->SwapIdWith(slot.typeId);
		// Every slot is already empty (HasItems above), so only the COUNT
		// changes - and a PackSlots resize allocates nothing either way, a
		// bigger bag included (its strings exist from construction).
		slot.contents.resize(static_cast<size_t>(cap));
		inv.selectedPack = i;                 // view the newly equipped pack
		++m_packEquips;
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

// What the pointer is over, for the armor tooltip and the status bar. Tracked
// every frame rather than on a click - a tooltip that needed clicking would not
// be one. Cleared whenever the pointer is not ours (a popup or the close box in
// front of the slot), so neither readout goes stale. Nothing is CONSUMED here:
// hovering must not steal the click that a slot is about to want.
void CharacterSheet::TrackInventoryHover(ui::UIContext& ctx, const gfx::Rect& px,
										 bool pointerFree) {
	m_hoverDoll = m_hoverPack = m_hoverPackRow = -1;
	const Input* input = ctx.CurrentInput();
	if (!pointerFree || !input || !m_character || m_mode != Mode::Inventory) return;
	const float mx = input->MouseX(), my = input->MouseY();
	for (int i = 0; i < kDollCellCount; ++i)
		if (EquipRect(px, i).Contains(mx, my)) { m_hoverDoll = i; return; }
	const auto& contents = m_character->inventory.SelectedContents();
	for (int i = 0; i < static_cast<int>(contents.size()); ++i)
		if (PackRect(px, i).Contains(mx, my)) { m_hoverPack = i; return; }
	for (int i = 0; i < kPackRowSlots; ++i)
		if (PackRowRect(px, i).Contains(mx, my)) { m_hoverPackRow = i; return; }
}

void CharacterSheet::UpdateInventory(ui::UIContext& ctx, const gfx::Rect& px,
									 float mx, float my, bool clicked) {
	if (!m_character) return;

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
	// RIGHT = the item's details, MIDDLE = its use menu, on whatever non-empty
	// thing the pointer is over (TrackInventoryHover has already said which).
	// Nothing happens over an empty slot - no item to describe or use - except
	// a middle-click on an empty hand (below).
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const bool right = input->WasMousePressed(MouseButton::Right);
	const bool middle = input->WasMousePressed(MouseButton::Middle);
	if (!right && !middle) return;
	const Inventory& inv = m_character->inventory;
	ItemPlace place;
	bool hit = false;
	if (m_hoverDoll >= 0) {
		const EquipSlot slot = kDollCells[m_hoverDoll].slot;
		place = {ItemPlace::Kind::Doll, static_cast<int>(slot)};
		// An EMPTY hand still has a use menu (punch, kick, its quick-cast
		// spells), exactly as the HUD's hand box does.
		const bool hand = slot == EquipSlot::LeftHand || slot == EquipSlot::RightHand;
		hit = !inv.equipment[static_cast<size_t>(slot)].Empty() || (middle && hand);
	} else if (m_hoverPack >= 0) {
		place = {ItemPlace::Kind::Pack, m_hoverPack};
		hit = m_hoverPack < static_cast<int>(inv.SelectedContents().size()) &&
			  !inv.SelectedContents()[static_cast<size_t>(m_hoverPack)].Empty();
	} else if (m_hoverPackRow >= 0) {
		place = {ItemPlace::Kind::Bag, m_hoverPackRow};
		hit = !inv.packs[static_cast<size_t>(m_hoverPackRow)].Empty();
	}
	if (!hit) return;
	if (right && onItemDetails) onItemDetails(place);
	else if (middle && onItemUse) onItemUse(place);
}

void CharacterSheet::DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								   const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();

	// --- equipment paper doll (left) ----------------------------------------
	for (int i = 0; i < kDollCellCount; ++i) {
		const size_t slot = static_cast<size_t>(kDollCells[i].slot);
		const gfx::Rect r = EquipRect(px, i);
		ui::DrawSlotFace(ctx, batch, r, kSlotBg);
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
		} else {
			DrawItemIcon(batch, r, s.typeId, m_icons); // a rune glows (PartyHudDraw)
		}
	}

	// --- backpack (right) — carry load stands in for a "Backpack" header ----
	const float load = CarryLoad();
	const float maxLoad = m_character->MaxCarryLoad();
	// Formatted on the stack: this draws every frame the tab is up. In whole
	// numbers, because MSVC's float path for a precision ("{:.1f}") allocates
	// in the debug build where the integer one does not. Loads are never
	// negative, so tenths split cleanly.
	char loadBuf[16], maxBuf[16];
	const long tenths = std::lround(std::max(load, 0.0f) * 10.0f);
	const auto loadEnd =
		std::format_to_n(loadBuf, sizeof(loadBuf), "{}.{}", tenths / 10, tenths % 10).out;
	const auto maxEnd =
		std::format_to_n(maxBuf, sizeof(maxBuf), "{}", std::lround(maxLoad)).out;
	const std::string_view loadStr(loadBuf, static_cast<size_t>(loadEnd - loadBuf));
	const std::string_view maxStr(maxBuf, static_cast<size_t>(maxEnd - maxBuf));
	const loc::Line loadText = loc::FormatLine("sheet.load", loadStr, maxStr);
	const Vec4 loadColor = load > maxLoad ? Vec4{0.85f, 0.25f, 0.2f, 1.0f} : theme.accent;
	font.Draw(batch, loadText, Ax(px, kPackX), Ay(px, kHeaderY), loadColor);

	const Inventory& inv = m_character->inventory;
	auto drawIcon = [&](const gfx::Rect& r, const std::string& typeId) {
		DrawItemIcon(batch, r, typeId, m_icons); // a rune glows (PartyHudDraw)
	};
	for (int i = 0; i < kPackRowSlots; ++i) {
		const gfx::Rect r = PackRowRect(px, i);
		const bool sel = i == inv.selectedPack;
		ui::DrawSlotFace(ctx, batch, r, sel ? Vec4{0.18f, 0.18f, 0.20f, 1.0f} : kSlotBg,
						 sel ? 0.08f : 0.0f);
		if (sel) ui::DrawBorder(batch, r, theme.accent);
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
		ui::DrawSlotFace(ctx, batch, r, kSlotBg);
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

	// This draws EVERY FRAME the pointer rests on a piece, so nothing below
	// builds a string: ids are referenced where they live, labels are loc
	// views, and the numbers are formatted into fixed cells on the stack.
	//
	// What is being hovered, and is it armor at all? A tooltip about a rune or
	// an empty slot would be noise.
	const std::string* hovered = nullptr;
	gfx::Rect anchor{};
	bool comparing = false;
	if (m_hoverDoll >= 0) {
		const size_t slot = static_cast<size_t>(kDollCells[m_hoverDoll].slot);
		hovered = &m_character->inventory.equipment[slot].typeId;
		anchor = EquipRect(px, m_hoverDoll);
	} else {
		const auto& contents = m_character->inventory.SelectedContents();
		if (m_hoverPack >= static_cast<int>(contents.size())) return;
		hovered = &contents[static_cast<size_t>(m_hoverPack)].typeId;
		anchor = PackRect(px, m_hoverPack);
		comparing = true;
	}
	const std::string& hoveredId = *hovered;
	if (hoveredId.empty()) return;
	if (!m_categories || m_categories->WornAt(hoveredId) == WearSlot::None) return;

	// The piece currently in the SAME slot the hovered one would go to — that
	// is what it is really being compared against, and its icon heads the left
	// column.
	const std::string* worn = &kNoItem;
	if (m_categories) {
		const WearSlot wear = m_categories->WornAt(hoveredId);
		for (int i = 0; i < kEquipCount; ++i)
			if (WearSlotFits(wear, static_cast<EquipSlot>(i))) {
				worn = &m_character->inventory.equipment[static_cast<size_t>(i)].typeId;
				break;
			}
	}
	const std::string& wornId = *worn;

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
		std::string_view label;
		Cell left, right;
		float lv = 0.0f, rv = 0.0f;
		bool higherBetter = true;
		bool compare = true; // false = a fact, not a score
	};
	// Rounds toward zero BEFORE formatting, or a term of -0.4 prints "-0" —
	// the same trap the sheet column had, reintroduced here because this is a
	// second formatter and it did not inherit the fix.
	const auto pts = [](Cell& c, float v) {
		const int n = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
		c.Format("{}{}", n >= 0 ? "+" : "", n);
	};
	const auto tenths = [](Cell& c, float v) {
		const long t = std::lround(v * 10.0f);
		c.Format("{}{}.{}", t < 0 ? "-" : "", std::labs(t) / 10, std::labs(t) % 10);
	};
	const auto whole = [](Cell& c, float v) { c.Format("{}", std::lround(v)); };
	const auto nameOf = [](Cell& c, const DefenseReadout& d) {
		c.Set(d.armorClass == ArmorClass::None
				  ? loc::View("sheet.def.unarmored")
				  : (d.armorName.empty() ? std::string_view(ArmorClassId(d.armorClass))
										 : d.armorName));
	};
	std::array<Row, 8> rows{};
	size_t rowCount = 0;
	const auto add = [&](const char* labelKey, float lv, float rv, bool compare = true)
		-> Row& {
		Row& r = rows[rowCount++];
		r.label = loc::View(labelKey);
		r.lv = lv;
		r.rv = rv;
		r.compare = compare;
		return r;
	};
	{
		Row& r = add("sheet.def.armor", 0, 0, false);
		nameOf(r.left, now);
		nameOf(r.right, with);
	}
	{
		Row& r = add("sheet.def.soak", now.soak, with.soak);
		tenths(r.left, now.soak);
		tenths(r.right, with.soak);
	}
	{
		Row& r = add("sheet.def.roll", now.total, with.total);
		whole(r.left, now.total);
		whole(r.right, with.total);
	}
	{
		Row& r = add("sheet.def.base", now.base, with.base);
		pts(r.left, now.base);
		pts(r.right, with.base);
	}
	{
		Row& r = add("sheet.def.dex", now.stat, with.stat);
		pts(r.left, now.stat);
		pts(r.right, with.stat);
	}
	{
		Row& r = add("sheet.def.stance", now.stance, with.stance);
		pts(r.left, now.stance);
		pts(r.right, with.stance);
	}
	{
		// The armor term is a COST: less of it is better, so its polarity flips.
		Row& r = add("sheet.def.armorpen", -now.armorPenalty, -with.armorPenalty);
		pts(r.left, -now.armorPenalty);
		pts(r.right, -with.armorPenalty);
	}
	if (now.strengthNeeded > 0 || with.strengthNeeded > 0) {
		// Unarmored asks for no strength at all, and "16 / 0" reads as a
		// requirement of zero rather than as no requirement.
		const auto strOf = [](Cell& c, const DefenseReadout& d) {
			if (d.strengthNeeded > 0) c.Format("{} / {}", d.strength, d.strengthNeeded);
			else c.Set("-");
		};
		Row& r = add("sheet.def.str", static_cast<float>(-now.strengthNeeded),
					 static_cast<float>(-with.strengthNeeded));
		strOf(r.left, now);
		strOf(r.right, with);
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
	const float h = pad * 2.0f + headH + row * static_cast<float>(rowCount);

	// NEVER OVER THE ITEM. Below it by preference, above when that would run
	// off the screen — the thing under the pointer is what the tooltip is
	// about, and covering it would answer a question by hiding it. (The same
	// rule every tooltip uses - ui::PlaceTooltip, which also keeps it on screen.)
	const gfx::Rect tip =
		ui::PlaceTooltip(anchor, w, h, {0, 0, ctx.Width(), ctx.Height()}, ui::TipSide::Below,
						 rem * 0.3f, pad, ui::TipAlign::Start);

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
	font.Draw(batch, loc::View("sheet.defense"), lx,
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

	for (size_t k = 0; k < rowCount; ++k) {
		const Row& r = rows[k];
		font.Draw(batch, r.label, lx, y, theme.textDim);
		font.Draw(batch, r.left.View(), v1, y, theme.text);
		if (comparing) {
			// Better green, worse red, identical ordinary — and a row that is
			// a FACT rather than a score (the piece's name) never colours.
			// Compared at the precision SHOWN, not the precision stored: the
			// stance term is derived by subtraction, so two identical stances
			// differ in the last float bit and coloured green for nothing.
			Vec4 c = theme.text;
			if (r.compare && std::fabs(r.rv - r.lv) >= 0.5f)
				c = (r.rv > r.lv) == r.higherBetter ? kTipGood : kTipBad;
			font.Draw(batch, r.right.View(), v2, y, c);
		}
		y += row;
	}
}

} // namespace dungeon::game
