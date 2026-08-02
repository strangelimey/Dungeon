// ============================================================================
// Game/CharacterSheet_Lists.cpp — Skills / Spells / Effects tabs.
//
// The three tabs share SheetList (CharacterSheet.h): a heading plus one row
// widget per item inside a ui::ScrollArea, which owns the scrolling, the
// clipping, the culling and the scrollbar. This file supplies what differs —
// how many rows there are, how tall each is, and how to draw one.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"
#include "Game/Spell/Spell.h"

#include "Core/Loc.h"

#include <algorithm>
#include <format>

namespace dungeon::game {
using namespace sheet;

namespace {

// Word-wraps `text` to `maxW`, handing each line and its index to `sink`, and
// returns the line count. string_view slices only — no per-frame allocation.
// Measuring a row and drawing it walk the same function, so a description can
// never be measured one height and drawn another.
template <typename Sink>
int WrapLines(const ui::Font& font, std::string_view text, float maxW, Sink&& sink) {
	int lines = 0;
	while (!text.empty()) {
		size_t end = text.size();
		while (end > 0 && font.MeasureWidth(text.substr(0, end)) > maxW) {
			const size_t space = text.rfind(' ', end - 1);
			if (space == std::string_view::npos || space == 0) break;
			end = space;
		}
		sink(text.substr(0, end), lines);
		++lines;
		const size_t next = text.find_first_not_of(' ', end);
		text = next == std::string_view::npos ? std::string_view{} : text.substr(next);
	}
	return lines;
}

int CountLines(const ui::Font& font, std::string_view text, float maxW) {
	return WrapLines(font, text, maxW, [](std::string_view, int) {});
}

} // namespace

// --- SheetList -------------------------------------------------------------

SheetList::SheetList(const gfx::Rect& rect, std::string heading,
					 std::string emptyText, Counter count, Measure measure,
					 SheetRow::DrawFn drawRow)
	: m_heading(std::move(heading)), m_empty(std::move(emptyText)),
	  m_count(std::move(count)), m_measure(std::move(measure)) {
	bounds = rect;
	debugName = "SheetList";
	m_scroll = Add<ui::ScrollArea>(gfx::Rect{});
	m_scroll->padding = 0.0f; // the rows carry the sheet's own margins
	m_scroll->debugName = "SheetScroll";
	m_rows = m_scroll->Add<ui::Repeater>(
		gfx::Rect{0, 0, 1, 1},
		[draw = std::move(drawRow)](size_t i) -> std::unique_ptr<ui::Widget> {
			return std::make_unique<SheetRow>(i, draw);
		},
		[this] { return m_count ? m_count() : 0; },
		[this](size_t i) {
			// Fractions of the REPEATER, which LayoutSelf has already sized to
			// the full stacked height.
			if (m_contentH <= 0.0f || i >= m_rowTop.size())
				return gfx::Rect{0, 0, 1, 0};
			return gfx::Rect{0.0f, m_rowTop[i] / m_contentH, 1.0f,
							 m_rowH[i] / m_contentH};
		});
	m_rows->debugName = "SheetRows";
}

void SheetList::ScrollToTop() {
	if (m_scroll) m_scroll->ScrollToTop();
}

float SheetList::ViewHeight() const {
	return std::max((bandBottom - bandTop) * Pixel().h, 0.0f);
}

// Measure every row and stack them, before the repeater's placer (which runs
// later in this same layout pass) asks for the offsets.
void SheetList::LayoutSelf(ui::UIContext& ctx) {
	const gfx::Rect& px = Pixel();
	m_scroll->bounds = {0.0f, bandTop, 1.0f, bandBottom - bandTop};
	// The scrollbar gutter, in the same terms the sheet's own layout used.
	m_scroll->gutter = kScrollBarW * px.w + kScrollBarPadPx;

	const size_t rows = m_count ? m_count() : 0;
	m_rowTop.resize(rows);
	m_rowH.resize(rows);
	float y = 0.0f;
	for (size_t i = 0; i < rows; ++i) {
		m_rowTop[i] = y;
		m_rowH[i] = m_measure ? m_measure(i, ctx.GetFont(), px.w) : 0.0f;
		y += m_rowH[i];
	}
	m_contentH = y;

	// The repeater carries the stacked height: the scroll area reads overflow
	// off its own children's bounds, and the rows are one level deeper. A
	// content box taller than the view is what makes the area scroll.
	const float view = ViewHeight();
	m_rows->bounds = {0.0f, 0.0f, 1.0f,
					  view > 0.0f ? std::max(m_contentH / view, 1.0f) : 1.0f};
}

void SheetList::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	ui::Font& font = ctx.GetFont();
	const ui::Theme& theme = ctx.GetTheme();
	font.Draw(batch, m_heading, Ax(px, kLeft), Ay(px, headingY), theme.accent);
	if ((m_count ? m_count() : 0) == 0)
		font.Draw(batch, m_empty, Ax(px, kLeft), Ay(px, kEmptyListY), theme.textDim);
}

// --- the bakes -------------------------------------------------------------

void CharacterSheet::BakeSkills() {
	m_skillRows.clear();
	if (!m_character) return;
	const Character& character = *m_character;
	// Skills-tab rows (docs/skills.md): the school skills first (symbol order,
	// bar tinted by school), then every other trained skill in the map's
	// alphabetical order (weapon classes — accent bar). Only trained skills
	// (xp > 0) list; none at all keeps the "No skills yet." line.
	auto addRow = [&](std::string_view id, float xp, const Vec4& tint) {
		const int level = Character::LevelForXp(xp);
		const float base = static_cast<float>(level * level);
		const float next = static_cast<float>((level + 1) * (level + 1));
		m_skillRows.push_back({loc::Tr("skill." + std::string(id)),
							   std::to_string(level),
							   std::clamp((xp - base) / (next - base), 0.0f, 1.0f),
							   tint});
	};
	for (u32 s = 0; s < kSymbolCount; ++s) {
		const SpellSymbol sym = static_cast<SpellSymbol>(s);
		if (!IsSchoolSymbol(sym)) continue;
		if (const float xp = character.SkillXpOf(SymbolId(sym)); xp > 0.0f) {
			const Vec4 c = ElementColor(sym);
			addRow(SymbolId(sym), xp, {c.x, c.y, c.z, 1.0f});
		}
	}
	for (const auto& [id, xp] : character.skillXp) {
		SpellSymbol sym;
		if (ParseSymbol(id, sym)) continue; // schools already listed above
		if (xp > 0.0f) addRow(id, xp, {0, 0, 0, 0});
	}
}

void CharacterSheet::BakeEffects() {
	m_effectRows.clear();
	if (!m_character) return;
	// Effects-tab rows: one per active effect, list order (= HUD icon order).
	// Baked here because the sheet freezes the world — nothing ticks while open.
	for (const fx::Inst& e : m_character->effects) {
		const Vec4 c = ElementColor(e.school);
		m_effectRows.push_back(
			{e.kind,
			 {c.x, c.y, c.z, 1.0f},
			 e.duration > 0.0f ? std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f)
							   : 1.0f,
			 loc::Tr(e.NameKey()),
			 loc::Format(std::string(e.NameKey()) + ".desc",
						 static_cast<int>(e.magnitude + 0.5f)),
			 loc::Format("sheet.effect_time",
						 static_cast<int>(e.timeLeft + 0.5f))});
	}
}

void CharacterSheet::BakeSpells() {
	m_spellRows.clear();
	if (!m_character || !spells) return;
	const Character& character = *m_character;
	// Spells-tab rows: learned spells, school-first then rune count then id.
	std::vector<const Spell*> defs;
	for (const auto& def : spells())
		if (character.HasLearnedSpell(def->Id())) defs.push_back(def.get());
	std::ranges::sort(defs, [](const Spell* a, const Spell* b) {
		const int sa = static_cast<int>(a->School()), sb = static_cast<int>(b->School());
		if (sa != sb) return sa < sb;
		const auto na = a->Sequence().size(), nb = b->Sequence().size();
		if (na != nb) return na < nb;
		return a->Id() < b->Id();
	});
	for (const Spell* def : defs) {
		const Vec4 c = ElementColor(def->School());
		m_spellRows.push_back(
			{{def->Sequence().begin(), def->Sequence().end()},
			 loc::Tr(def->NameKey()),
			 loc::Format(def->DescKey(), static_cast<int>(def->Power() + 0.5f)),
			 {c.x, c.y, c.z, 1.0f}});
	}
}

// --- rows: measure + draw --------------------------------------------------
//
// A row owns its Y (the list stacks it); the X fractions still resolve against
// the SHEET, which is what keeps the columns lined up with the rest of the page.

float CharacterSheet::MeasureSkillRow(size_t, ui::Font&, float) const {
	return kRowH * Pixel().h;
}

void CharacterSheet::DrawSkillRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_skillRows.size()) return;
	const SkillRow& row = m_skillRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();
	font.Draw(batch, row.label, Ax(px, kLabelX), r.y, theme.textDim);
	const float vw = font.MeasureWidth(row.level);
	font.Draw(batch, row.level, Ax(px, kValueRight) - vw, r.y, theme.text);
	DrawStatBar(batch, {Ax(px, kSkillBarX), r.y, kSkillBarW * px.w, kBarH * px.h},
				row.frac, row.tint.w > 0.0f ? row.tint : theme.accent, theme);
}

float CharacterSheet::MeasureSpellRow(size_t i, ui::Font& font,
									  float widthPx) const {
	if (i >= m_spellRows.size()) return 0.0f;
	const float maxW = (kTextRight - kSpellTextX) * widthPx;
	const int lines = CountLines(font, m_spellRows[i].desc, maxW);
	return font.LineAdvance() + 2.0f + static_cast<float>(lines) * font.LineAdvance() +
		   kSpellRowGap * Pixel().h;
}

void CharacterSheet::DrawSpellRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_spellRows.size()) return;
	const SpellRow& row = m_spellRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();
	const float textX = Ax(px, kSpellTextX);
	const float maxW = (kTextRight - kSpellTextX) * px.w;
	const float ish = font.Height(); // rune-icon square ~ the text height
	const float runeGap = 0.004f * px.w;

	float nameX = textX;
	for (SpellSymbol sym : row.symbols) {
		const gfx::Rect ir{nameX, r.y, ish, ish};
		const gfx::Texture* ic = m_icons ? m_icons->For(RuneItemId(sym)) : nullptr;
		if (ic)
			batch.DrawSprite(ir, {0, 0, 1, 1}, *ic, {1, 1, 1, 1});
		else {
			const Vec4 sc = ElementColor(sym);
			batch.DrawRect(ir, {sc.x, sc.y, sc.z, 0.6f});
		}
		nameX += ish + runeGap;
	}
	font.Draw(batch, row.name, nameX + runeGap, r.y,
			  {row.tint.x, row.tint.y, row.tint.z, 1.0f});
	const float descTop = r.y + font.LineAdvance() + 2.0f;
	WrapLines(font, row.desc, maxW, [&](std::string_view line, int n) {
		font.Draw(batch, line, textX,
				  descTop + static_cast<float>(n) * font.LineAdvance(), theme.textDim);
	});
}

float CharacterSheet::MeasureEffectRow(size_t i, ui::Font& font,
									   float widthPx) const {
	if (i >= m_effectRows.size()) return 0.0f;
	const float maxW = (kTextRight - kEffectTextX) * widthPx;
	const int lines = CountLines(font, m_effectRows[i].desc, maxW);
	const float textH =
		font.LineAdvance() + 4.0f + static_cast<float>(lines) * font.LineAdvance();
	return std::max(textH, kEffectIconH * Pixel().h) + kEffectRowGap * Pixel().h;
}

void CharacterSheet::DrawEffectRow(size_t i, ui::UIContext& ctx,
								   gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_effectRows.size()) return;
	const EffectRow& row = m_effectRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	const gfx::Rect icon{Ax(px, kEffectIconX), r.y, kEffectIconW * px.w,
						 kEffectIconH * px.h};
	batch.DrawRect(icon, kSlotBg);
	const gfx::Texture* iconTex =
		m_icons && row.kind ? m_icons->For(row.kind->IconItem()) : nullptr;
	if (iconTex)
		batch.DrawSprite({icon.x + 2, icon.y + 2, icon.w - 4, icon.h - 4},
						 {0, 0, 1, 1}, *iconTex, {1, 1, 1, 1});
	else
		batch.DrawRect({icon.x + 2, icon.y + 2, icon.w - 4, icon.h - 4},
					   {row.tint.x, row.tint.y, row.tint.z, 0.5f});
	batch.DrawRect({icon.x + 2, icon.y + icon.h - 5, (icon.w - 4) * row.frac, 3},
				   row.tint);
	ui::DrawBorder(batch, icon, row.tint);

	const float textX = Ax(px, kEffectTextX);
	const float maxW = (kTextRight - kEffectTextX) * px.w;
	font.Draw(batch, row.name, textX, r.y, theme.text);
	const float tw = font.MeasureWidth(row.time);
	font.Draw(batch, row.time, Ax(px, kTextRight) - tw, r.y, theme.accent);
	const float descTop = r.y + font.LineAdvance() + 4.0f;
	WrapLines(font, row.desc, maxW, [&](std::string_view line, int n) {
		font.Draw(batch, line, textX,
				  descTop + static_cast<float>(n) * font.LineAdvance(), theme.textDim);
	});
}

} // namespace dungeon::game
