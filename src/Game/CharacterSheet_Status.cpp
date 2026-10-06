// ============================================================================
// Game/CharacterSheet_Status.cpp - the status bar along the sheet's foot.
//
// One line naming whatever the pointer is over, on every tab: an item's name
// and weight (a bag's with what is in it), an attribute's or a resource bar's
// name and what it does, a skill's, a spell's or an effect's name and a line
// about it. Blank when the pointer is over nothing that has anything to say
// (docs/ui-updates-plan.md P1).
//
// It is set every frame the sheet is up, and the sheet's frames are GUARDED
// (Game::SteadyStateFrame), so everything here is inline: loc::Line halves,
// keys assembled on the stack, numbers formatted into stack buffers.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"

#include "Core/Loc.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {
using namespace sheet;

namespace {

// The attribute and bar ids the Stats tab lists, in its draw order - the hint
// keys are attr.<id>.hint and bar.<id>.hint.
constexpr std::string_view kAttrIds[] = {"strength", "dexterity", "vitality",
										 "willpower", "intelligence"};
constexpr std::string_view kBarIds[] = {"health", "stamina", "mana", "food", "water"};

// "<prefix><id>.hint", looked up and COPIED into a Line: a missing key comes
// back as the key itself, which here is a stack buffer (loc::View's caveat).
loc::Line HintFor(std::string_view prefix, std::string_view id) {
	constexpr std::string_view kHint = ".hint";
	char key[96];
	const size_t room = sizeof(key) - kHint.size();
	const size_t p = std::min(prefix.size(), room);
	const size_t n = std::min(id.size(), room - p);
	std::copy_n(prefix.data(), p, key);
	std::copy_n(id.data(), n, key + p);
	std::copy(kHint.begin(), kHint.end(), key + p + n);
	return loc::Line(loc::View(std::string_view(key, p + n + kHint.size())));
}

// `text` cut down to `maxW` with a trailing "...", written into `buf` (or
// returned as-is when it already fits). Searches whole UTF-8 code points, so a
// cut never lands inside a multi-byte character.
std::string_view FitWidth(const ui::Font& font, std::string_view text, float maxW,
						  std::span<char> buf) {
	if (font.MeasureWidth(text) <= maxW) return text;
	constexpr std::string_view kDots = "...";
	const float avail = maxW - font.MeasureWidth(kDots);
	if (avail <= 0.0f || buf.size() <= kDots.size()) return {};
	const auto boundary = [&](size_t i) {
		while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
		return i;
	};
	size_t lo = 0, hi = std::min(text.size(), buf.size() - kDots.size());
	while (hi - lo > 1) {
		const size_t mid = boundary((lo + hi) / 2);
		if (mid <= lo) break;
		if (font.MeasureWidth(text.substr(0, mid)) <= avail) lo = mid;
		else hi = mid;
	}
	while (lo > 0 && text[lo - 1] == ' ') --lo; // no "word ..." gap
	std::copy_n(text.data(), lo, buf.data());
	std::copy(kDots.begin(), kDots.end(), buf.data() + lo);
	return {buf.data(), lo + kDots.size()};
}

} // namespace

gfx::Rect CharacterSheet::Body() const {
	const gfx::Rect& px = Pixel();
	if (m_card) {
		// The card shows only the TAB part of a body - kHeaderY down - under its
		// name band, so the body it resolves against is that part stretched back
		// to a whole one, its top (where the portrait would be) above the card.
		const float top = px.y + Em(kCardNameEm);
		const float h = std::max(px.y + px.h - top, 0.0f) / (1.0f - kHeaderY);
		return {px.x, top - kHeaderY * h, px.w, h};
	}
	return {px.x, px.y, px.w, px.h * kBodyFrac};
}

gfx::Rect CharacterSheet::StatusRect() const {
	const gfx::Rect& px = Pixel();
	const float bodyH = px.h * kBodyFrac;
	return {px.x, px.y + bodyH, px.w, px.h - bodyH};
}

void CharacterSheet::SetStatus(std::string_view name, std::string_view text,
							   const Vec4& nameColor) {
	m_statusName.Assign(name);
	m_statusText.Assign(text);
	m_statusColor = nameColor;
}

void CharacterSheet::SetItemStatus(const std::string& itemId,
								   std::span<const ItemSlot> contents) {
	if (itemId.empty()) return;
	float kg = m_weights ? m_weights->For(itemId) : 0.0f;
	if (m_weights)
		for (const ItemSlot& s : contents)
			if (!s.Empty()) kg += m_weights->For(s.typeId);
	// Tenths, formatted as integers - the carry-load line's reason (MSVC's
	// float precision path allocates in debug).
	char buf[16];
	const long tenths = std::lround(std::max(kg, 0.0f) * 10.0f);
	const auto end = std::format_to_n(buf, sizeof(buf), "{}.{}", tenths / 10, tenths % 10).out;
	const std::string_view weight(buf, static_cast<size_t>(end - buf));
	m_statusName = loc::ViewKey("item.", itemId);
	m_statusText = loc::FormatLine("sheet.status.weight", weight);
	m_statusColor = m_accent;
}

void CharacterSheet::UpdateStatus(ui::UIContext& ctx, bool pointerFree) {
	m_statusName = {};
	m_statusText = {};
	m_tipRune = -1;
	m_accent = ctx.GetTheme().accent;
	if (!m_character) return;
	const Input* input = ctx.CurrentInput();
	const float mx = input ? input->MouseX() : 0.0f;
	const float my = input ? input->MouseY() : 0.0f;
	const gfx::Rect px = Body();

	switch (m_mode) {
	case Mode::Inventory: {
		const Inventory& inv = m_character->inventory;
		if (m_hoverDoll >= 0) {
			const size_t slot = static_cast<size_t>(kDollCells[m_hoverDoll].slot);
			SetItemStatus(inv.equipment[slot].typeId, {});
		} else if (m_hoverPack >= 0) {
			const auto& contents = inv.SelectedContents();
			if (m_hoverPack < static_cast<int>(contents.size()))
				SetItemStatus(contents[static_cast<size_t>(m_hoverPack)].typeId, {});
		} else if (m_hoverPackRow >= 0) {
			const Pack& pack = inv.packs[static_cast<size_t>(m_hoverPackRow)];
			SetItemStatus(pack.typeId, pack.contents);
		}
		// Over nothing with a name (no slot, or an empty one), the item riding the
		// cursor is what there is to name.
		if (m_statusName.empty() && m_held && m_held->has_value())
			SetItemStatus(**m_held, {});
		break;
	}
	case Mode::Stats: {
		if (!pointerFree) break;
		const StatRows rows = StatRowsFor(ctx, px);
		const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kStatRem));
		for (size_t i = 0; i < std::size(kAttrIds); ++i) {
			const float y = rows.top + static_cast<float>(i) * rows.step;
			const gfx::Rect attr{Ax(px, kLabelX), y, (kValueRight - kLabelX) * px.w,
								 font.LineAdvance()};
			const gfx::Rect bar{Ax(px, kBarLabelX), y, (kBarX + kBarW - kBarLabelX) * px.w,
								std::max(kStatBarH * px.h, font.LineAdvance())};
			if (attr.Contains(mx, my)) {
				SetStatus(m_attrLabels[i], HintFor("attr.", kAttrIds[i]), m_accent);
				break;
			}
			if (bar.Contains(mx, my)) {
				const std::string* labels[] = {&m_healthLabel, &m_staminaLabel,
											   &m_manaLabel, &m_foodLabel, &m_waterLabel};
				SetStatus(*labels[i], HintFor("bar.", kBarIds[i]), m_accent);
				break;
			}
		}
		break;
	}
	case Mode::Skills: {
		const int i = m_lists[0] ? m_lists[0]->HoveredRow() : -1;
		if (i < 0 || static_cast<size_t>(i) >= m_skillRows.size()) break;
		const SkillRow& row = m_skillRows[static_cast<size_t>(i)];
		if (row.header) break; // a group heading has nothing to explain
		// Named in its BAR's colour, so a skill reads in one colour (C477).
		SetStatus(row.label, HintFor("skill.", row.id), SkillBarColor(row.id));
		break;
	}
	case Mode::Spells: {
		const int i = m_lists[1] ? m_lists[1]->HoveredRow() : -1;
		if (i < 0 || static_cast<size_t>(i) >= m_spellRows.size()) break;
		const SpellRow& row = m_spellRows[static_cast<size_t>(i)];
		// Over one rune of the recipe, the bar names THE RUNE - its Futhark name
		// and its meaning - and the overlay pass draws its tip (ui-bars-updates
		// B3); anywhere else on the row, the spell.
		const gfx::Rect& rowRect = m_lists[1]->HoveredRowRect();
		for (size_t k = 0; k < row.symbols.size(); ++k) {
			const gfx::Rect cell = SpellRuneRect(ctx, rowRect, k);
			if (!cell.Contains(mx, my)) continue;
			const SpellSymbol s = row.symbols[k];
			m_statusName = loc::Line(loc::View(RuneNameKey(s)));
			m_statusText = loc::ViewKey("symbol.", SymbolId(s));
			m_statusColor = ElementColor(s);
			m_statusColor.w = 1.0f;
			m_tipRune = static_cast<int>(s);
			m_tipRuneRect = cell;
			break;
		}
		if (m_tipRune < 0) SetStatus(row.name, row.desc, row.tint);
		break;
	}
	case Mode::Effects: {
		const int i = m_lists[2] ? m_lists[2]->HoveredRow() : -1;
		if (i < 0 || static_cast<size_t>(i) >= m_effectRows.size()) break;
		const EffectRow& row = m_effectRows[static_cast<size_t>(i)];
		SetStatus(row.name, row.desc, row.tint);
		break;
	}
	}
}

void CharacterSheet::DrawStatus(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect band = StatusRect();
	const gfx::Rect& px = Pixel();
	const float left = Ax(px, kLeft);
	const float right = Ax(px, 1.0f - kLeft);
	// The rule that divides the bar from the tabs: always drawn, so the band
	// reads as part of the page even while it is empty.
	batch.DrawRect({left, band.y, right - left, 1.0f}, theme.panelBorder);
	if (m_statusName.empty()) return;

	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kStatusTextRem));
	const float y = band.y + (band.h - font.Height()) * 0.5f;
	char nameBuf[loc::Line::kCapacity + 4];
	const std::string_view name = FitWidth(font, m_statusName.View(), right - left, nameBuf);
	font.Draw(batch, name, left, y, m_statusColor);
	if (m_statusText.empty()) return;
	const float textX = left + font.MeasureWidth(name) + Em(kStatusGapRem);
	if (textX >= right) return;
	char textBuf[loc::Line::kCapacity + 4];
	font.Draw(batch, FitWidth(font, m_statusText.View(), right - textX, textBuf), textX, y,
			  theme.text);
}

} // namespace dungeon::game
