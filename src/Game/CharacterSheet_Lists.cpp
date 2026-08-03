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
			// Fractions of the REPEATER'S OWN pixel height (m_placeH), NOT of
			// the stacked content height. Those differ whenever the list is
			// shorter than its band: the repeater is held open to the band so
			// the scroll area sees no overflow, and dividing by the smaller
			// content height would inflate every row to fill it — a short list
			// came out with rows half again as tall as they measured.
			if (m_placeH <= 0.0f || i >= m_rowTop.size())
				return gfx::Rect{0, 0, 1, 0};
			return gfx::Rect{0.0f, m_rowTop[i] / m_placeH, 1.0f,
							 m_rowH[i] / m_placeH};
		});
	m_rows->debugName = "SheetRows";
}

void SheetList::ScrollToTop() {
	if (m_scroll) m_scroll->ScrollToTop();
}

float SheetList::ViewHeight() const {
	return std::max((bandBottom - m_bandTop) * Pixel().h, 0.0f);
}

// Measure every row and stack them, before the repeater's placer (which runs
// later in this same layout pass) asks for the offsets.
void SheetList::LayoutSelf(ui::UIContext& ctx) {
	const gfx::Rect& px = Pixel();
	// The scrolling band starts a MEASURED line below the tab's title, the same
	// rule the Stats tab uses for its own heading (DrawStats). An authored band
	// top would have to be re-tuned by hand every time the title size moved —
	// and at kTabTitleRem the old fraction put the title straight through the
	// first row.
	const ui::Font& title = ctx.FontAt(ui::FontRole::Body, Rem(kTabTitleRem));
	m_bandTop = headingY + (title.LineAdvance() + Rem(0.4f)) / std::max(px.h, 1.0f);
	m_scroll->bounds = {0.0f, m_bandTop, 1.0f,
						std::max(bandBottom - m_bandTop, 0.0f)};
	// The scrollbar gutter, in the same terms the sheet's own layout used.
	m_scroll->gutter = (kScrollBarW * px.w) / Rem() + kScrollBarPadRem;

	const size_t rows = m_count ? m_count() : 0;
	m_rowTop.resize(rows);
	m_rowH.resize(rows);
	float y = 0.0f;
	for (size_t i = 0; i < rows; ++i) {
		m_rowTop[i] = y;
		m_rowH[i] = m_measure ? m_measure(i, ctx, TextFont(), px.w) : 0.0f;
		y += m_rowH[i];
	}
	m_contentH = y;

	// The repeater carries the stacked height: the scroll area reads overflow
	// off its own children's bounds, and the rows are one level deeper. A
	// content box taller than the view is what makes the area scroll — and it
	// never shrinks BELOW the view, or a short list would report overflow it
	// does not have. m_placeH is that final height, which the row placer
	// divides by so rows keep the size they were measured at either way.
	const float view = ViewHeight();
	m_placeH = std::max(m_contentH, view);
	m_rows->bounds = {0.0f, 0.0f, 1.0f, view > 0.0f ? m_placeH / view : 1.0f};
}

void SheetList::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	const ui::Theme& theme = ctx.GetTheme();
	// Title at kTabTitleRem, matching the Stats tab's "Attributes".
	ctx.FontAt(ui::FontRole::Body, Rem(kTabTitleRem))
		.Draw(batch, m_heading, Ax(px, kLeft), Ay(px, headingY), theme.accent);
	// The empty-list line sits at the top of the band the rows would have
	// filled, so it follows the title down instead of needing its own fraction.
	if ((m_count ? m_count() : 0) == 0)
		TextFont().Draw(batch, m_empty, Ax(px, kLeft), Ay(px, m_bandTop),
						theme.textDim);
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

float CharacterSheet::MeasureSkillRow(size_t, ui::UIContext& ctx, const ui::Font&,
									  float) const {
	// One line plus a small gap, measured — not a fixed pitch. A skill row is a
	// single line of text, so anything more is dead space in a list that grows.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kSkillRem));
	return font.LineAdvance() + kSkillRowGap * Pixel().h;
}

void CharacterSheet::DrawSkillRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_skillRows.size()) return;
	const SkillRow& row = m_skillRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kSkillRem));
	const gfx::Rect& px = Pixel();
	font.Draw(batch, row.label, Ax(px, kLabelX), r.y, theme.textDim);
	const float vw = font.MeasureWidth(row.level);
	font.Draw(batch, row.level, Ax(px, kValueRight) - vw, r.y, theme.text);
	// The bar stands as tall as the text beside it, so it tightens with the row
	// instead of holding it open at the Stats tab's height.
	DrawStatBar(batch, {Ax(px, kSkillBarX), r.y, kSkillBarW * px.w, font.Height()},
				row.frac, row.tint.w > 0.0f ? row.tint : theme.accent, theme);
}

float CharacterSheet::MeasureSpellRow(size_t i, ui::UIContext& ctx,
									  const ui::Font&, float widthPx) const {
	if (i >= m_spellRows.size()) return 0.0f;
	// The NAME line is interface (the list's own face); the description is the
	// world talking, so it is set in Script. Measured in the same two faces
	// DrawSpellRow draws them in — measuring the wrap in one face and drawing it
	// in another gives a row height that does not match its contents.
	const ui::Font& name = ctx.FontAt(ui::FontRole::Body, Rem(kNameRem));
	const ui::Font& desc = ctx.FontAt(ui::FontRole::Script, Rem(kDescRem));
	const float maxW = (kTextRight - kSpellTextX) * widthPx;
	const int lines = CountLines(desc, m_spellRows[i].desc, maxW);
	return name.Height() + Rem(kNameDescGapRem) +
		   static_cast<float>(lines) * desc.LineAdvance() +
		   kSpellRowGap * Pixel().h;
}

void CharacterSheet::DrawSpellRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_spellRows.size()) return;
	const SpellRow& row = m_spellRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	// The name line (and with it the rune squares, which are sized off the text)
	// runs at kNameRem; MeasureSpellRow uses the same font for that line.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kNameRem));
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
	// The description in Script — in-world text, as against the interface face
	// the name and the rest of the sheet use. MeasureSpellRow splits it the same
	// way, so the row is as tall as what lands in it.
	const ui::Font& descFont = ctx.FontAt(ui::FontRole::Script, Rem(kDescRem));
	const float descTop = r.y + font.Height() + Rem(kNameDescGapRem);
	WrapLines(descFont, row.desc, maxW, [&](std::string_view line, int n) {
		descFont.Draw(batch, line, textX,
					  descTop + static_cast<float>(n) * descFont.LineAdvance(),
					  theme.textDim);
	});
}

float CharacterSheet::EffectIconSize(const ui::Font& nameFont) const {
	// Exactly the name line: its height plus the gap that separates it from the
	// description, which is where the description begins.
	return nameFont.Height() + Rem(kNameDescGapRem);
}

float CharacterSheet::EffectTextInset(const ui::Font& nameFont) const {
	return kEffectIconX * Pixel().w + EffectIconSize(nameFont) +
		   Rem(kEffectIconGapRem);
}

float CharacterSheet::MeasureEffectRow(size_t i, ui::UIContext& ctx,
									   const ui::Font&, float widthPx) const {
	if (i >= m_effectRows.size()) return 0.0f;
	// Same split as a spell row: the name/timer line is interface, the
	// description is the world, and each is measured in the face it draws in.
	const ui::Font& name = ctx.FontAt(ui::FontRole::Body, Rem(kNameRem));
	const ui::Font& desc = ctx.FontAt(ui::FontRole::Script, Rem(kDescRem));
	const float maxW = kTextRight * widthPx - EffectTextInset(name);
	const int lines = CountLines(desc, m_effectRows[i].desc, maxW);
	// No max() against the icon any more: it spans the name line by
	// construction, so it can never be taller than name + description.
	return name.Height() + Rem(kNameDescGapRem) +
		   static_cast<float>(lines) * desc.LineAdvance() +
		   kEffectRowGap * Pixel().h;
}

void CharacterSheet::DrawEffectRow(size_t i, ui::UIContext& ctx,
								   gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_effectRows.size()) return;
	const EffectRow& row = m_effectRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	// Name AND duration at kNameRem, matching a spell row's name line.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kNameRem));
	const gfx::Rect& px = Pixel();

	// Square, spanning the name line: top on the name's top, bottom where the
	// description starts.
	const float iconSize = EffectIconSize(font);
	const gfx::Rect icon{Ax(px, kEffectIconX), r.y, iconSize, iconSize};
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

	const float textX = px.x + EffectTextInset(font);
	const float maxW = Ax(px, kTextRight) - textX;
	font.Draw(batch, row.name, textX, r.y, theme.text);
	const float tw = font.MeasureWidth(row.time);
	font.Draw(batch, row.time, Ax(px, kTextRight) - tw, r.y, theme.accent);
	// The effect's description in Script, matching a spell's (kDescRem).
	const ui::Font& descFont = ctx.FontAt(ui::FontRole::Script, Rem(kDescRem));
	const float descTop = r.y + font.Height() + Rem(kNameDescGapRem);
	WrapLines(descFont, row.desc, maxW, [&](std::string_view line, int n) {
		descFont.Draw(batch, line, textX,
					  descTop + static_cast<float>(n) * descFont.LineAdvance(),
					  theme.textDim);
	});
}

} // namespace dungeon::game
