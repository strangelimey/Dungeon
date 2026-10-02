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
#include "Game/Combat.h"
#include "Game/PartyHudDraw.h"
#include "Game/PartyHudTypes.h"
#include "Game/Spell/Spell.h"

#include "Core/Loc.h"
#include "UI/TextWrap.h" // ui::WrapLines (shared with the editor map's tooltip)

#include <algorithm>
#include <format>

namespace dungeon::game {
using namespace sheet;

namespace {

using ui::WrapLines;

int CountLines(const ui::Font& font, std::string_view text, float maxW) {
	return WrapLines(font, text, maxW, [](std::string_view, int) {});
}

// A skill bar's colour is its skill's FAMILY (Michael, ui-bars-updates: chosen
// over a grey-to-green grade by fraction, which read as "sickly"): magic by
// school, weapons steel, defence bronze, each reserve the pool it feeds.
constexpr Vec4 kWeaponSteel{0.70f, 0.76f, 0.84f, 1.0f};
constexpr Vec4 kDefenceBronze{0.85f, 0.62f, 0.30f, 1.0f};
// The reserves wear the pool each one feeds (bar.hlsl's bright stops).
constexpr Vec4 kHealthRed{0.95f, 0.18f, 0.14f, 1.0f};
constexpr Vec4 kStaminaGreen{0.35f, 0.95f, 0.45f, 1.0f};
constexpr Vec4 kManaBlue{0.30f, 0.60f, 1.00f, 1.0f};

Vec4 SkillBarColor(std::string_view id) {
	if (SpellSymbol sym; ParseSymbol(id, sym)) {
		const Vec4 c = ElementColor(sym);
		return {c.x, c.y, c.z, 1.0f};
	}
	if (id == resource::SkillId(resource::Kind::Health)) return kHealthRed;
	if (id == resource::SkillId(resource::Kind::Stamina)) return kStaminaGreen;
	if (id == resource::SkillId(resource::Kind::Mana)) return kManaBlue;
	if (id == kAvoidSkill) return kDefenceBronze;
	for (int c = 0; c < static_cast<int>(ArmorClass::Count); ++c)
		if (const char* skill = ArmorSkillId(static_cast<ArmorClass>(c)); *skill && id == skill)
			return kDefenceBronze;
	// Everything else is a way of hitting things: unarmed, throwing, and every
	// weapon class a catalog names.
	return kWeaponSteel;
}

// A skill bar's glass, as a share of the text height beside it (Michael, A1:
// the rows open up so the bar stays about as tall as its text).
constexpr float kSkillGlass = 0.8f;

// The height a skill row's LINE takes: the text's line advance, or - framed -
// enough for the whole frame round a kSkillGlass glass, whichever is taller.
// Measure and Draw both ask, so the text, the bar and the row pitch agree.
float SkillBand(const ui::Font& font, const ResourceBarStyle& style) {
	float band = font.LineAdvance();
	if (style.framed && style.frame) {
		const BarFrameReach unit = FrameReach(1.0f);
		band = std::max(band, kSkillGlass * font.Height() *
								  (1.0f + unit.top + unit.bottom) / kFramedRowShare);
	}
	return band;
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
		[this, draw = std::move(drawRow)](size_t i) -> std::unique_ptr<ui::Widget> {
			return std::make_unique<SheetRow>(i, draw, &m_hoverRow, &m_hoverRect);
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

void SheetList::Warm(size_t n) {
	m_rowTop.reserve(n);
	m_rowH.reserve(n);
	if (m_rows) m_rows->Warm(n);
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
	const ui::Font& title = ctx.FontAt(ui::FontRole::Body, Em(kTabTitleRem));
	m_bandTop = headingY + (title.LineAdvance() + Em(0.4f)) / std::max(px.h, 1.0f);
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
	ctx.FontAt(ui::FontRole::Body, Em(kTabTitleRem))
		.Draw(batch, m_heading, Ax(px, kLeft), Ay(px, headingY), theme.accent);
	// The empty-list line sits at the top of the band the rows would have
	// filled, so it follows the title down instead of needing its own fraction.
	if ((m_count ? m_count() : 0) == 0)
		TextFont().Draw(batch, m_empty, Ax(px, kLeft), Ay(px, m_bandTop),
						theme.textDim);
}

// --- the bakes -------------------------------------------------------------

// Each bake rewinds its RowPool and assign()s into the rows it hands back, so a
// re-bake allocates nothing (CharacterSheet.h, RowPool): labels are loc views,
// numbers and formatted lines go through stack buffers / loc::FormatLine.

void CharacterSheet::BakeSkills() {
	m_skillRows.Reset();
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
		SkillRow& row = m_skillRows.Next();
		row.id.assign(id);
		row.label.assign(loc::ViewKey("skill.", id).View());
		char buf[16];
		const auto end = std::format_to_n(buf, sizeof(buf), "{}", level).out;
		row.level.assign(buf, end);
		row.frac = std::clamp((xp - base) / (next - base), 0.0f, 1.0f);
		row.tint = tint;
		row.header = false;
	};
	// A heading, added only when its group turns out to have rows — so a member
	// who has trained nothing gets the "No skills yet." line rather than two
	// labels over empty space.
	auto addHeader = [&](const char* key) {
		SkillRow& row = m_skillRows.Next();
		row.id.clear();
		row.label.assign(loc::View(key));
		row.level.clear();
		row.frac = 0.0f;
		row.tint = {0, 0, 0, 0};
		row.header = true;
	};
	// The RESOURCE practices are told apart from the rest by id, not by any flag
	// on the row — resource::SkillId is the one place that mapping lives, and
	// asking it means a fourth pool would land in the right group for free.
	const auto isPractice = [](std::string_view id) {
		for (u8 k = 0; k < static_cast<u8>(resource::Kind::Count); ++k)
			if (id == resource::SkillId(static_cast<resource::Kind>(k))) return true;
		return false;
	};

	// --- what you chose to practise: schools first (symbol order, tinted by
	// school), then everything else in the map's alphabetical order.
	const size_t trainedStart = m_skillRows.size();
	addHeader("sheet.skills.training");
	const size_t afterHeader = m_skillRows.size();
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
		if (xp > 0.0f && !isPractice(id)) addRow(id, xp, {0, 0, 0, 0});
	}
	if (m_skillRows.size() == afterHeader) m_skillRows.Truncate(trainedStart);

	// --- and what your body did. Same shape, its own heading.
	const size_t bodyStart = m_skillRows.size();
	addHeader("sheet.skills.reserves");
	const size_t afterBody = m_skillRows.size();
	for (const auto& [id, xp] : character.skillXp)
		if (xp > 0.0f && isPractice(id)) addRow(id, xp, {0, 0, 0, 0});
	if (m_skillRows.size() == afterBody) m_skillRows.Truncate(bodyStart);
}

void CharacterSheet::BakeEffects() {
	m_effectRows.Reset();
	if (!m_character) return;
	// Effects-tab rows: one per active effect, list order (= HUD icon order).
	for (const fx::Inst& e : m_character->effects) {
		const Vec4 c = ElementColor(e.school);
		EffectRow& row = m_effectRows.Next();
		row.kind = e.kind;
		row.tint = {c.x, c.y, c.z, 1.0f};
		row.frac = e.duration > 0.0f ? std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f)
									 : 1.0f;
		row.name.assign(loc::View(e.NameKey()));
		// The description's key is <nameKey>.desc, assembled on the stack.
		constexpr std::string_view kDesc = ".desc";
		char key[96];
		const std::string_view nameKey = e.NameKey();
		const size_t n = std::min(nameKey.size(), sizeof(key) - kDesc.size());
		std::copy_n(nameKey.data(), n, key);
		std::copy(kDesc.begin(), kDesc.end(), key + n);
		row.desc.assign(loc::FormatLine(std::string_view(key, n + kDesc.size()),
										static_cast<int>(e.magnitude + 0.5f))
							.View());
		row.time.assign(
			loc::FormatLine("sheet.effect_time", static_cast<int>(e.timeLeft + 0.5f))
				.View());
	}
}

void CharacterSheet::BakeSpells() {
	m_spellRows.Reset();
	if (!m_character || !spells) return;
	const Character& character = *m_character;
	// Spells-tab rows: learned spells, school-first then rune count then id.
	m_spellOrder.clear();
	for (const auto& def : spells())
		if (character.HasLearnedSpell(def->Id())) m_spellOrder.push_back(def.get());
	std::ranges::sort(m_spellOrder, [](const Spell* a, const Spell* b) {
		const int sa = static_cast<int>(a->School()), sb = static_cast<int>(b->School());
		if (sa != sb) return sa < sb;
		const auto na = a->Sequence().size(), nb = b->Sequence().size();
		if (na != nb) return na < nb;
		return a->Id() < b->Id();
	});
	for (const Spell* def : m_spellOrder) {
		const Vec4 c = ElementColor(def->School());
		SpellRow& row = m_spellRows.Next();
		row.symbols.assign(def->Sequence().begin(), def->Sequence().end());
		row.name.assign(loc::View(def->NameKey()));
		row.desc.assign(
			loc::FormatLine(def->DescKey(), static_cast<int>(def->Power() + 0.5f)).View());
		row.tint = {c.x, c.y, c.z, 1.0f};
	}
}

// --- rows: measure + draw --------------------------------------------------
//
// A row owns its Y (the list stacks it); the X fractions still resolve against
// the SHEET, which is what keeps the columns lined up with the rest of the page.

// Air above a group heading, so it reads as a divider rather than as another
// row. Measured and drawn from the SAME constant — the height and the offset
// have to agree or the gap lands under the heading instead of over it.
namespace {
constexpr float kSkillGroupGapRem = 0.8f;
} // namespace

float CharacterSheet::MeasureSkillRow(size_t i, ui::UIContext& ctx,
									  const ui::Font&, float) const {
	// One line plus a small gap, measured — not a fixed pitch. A skill row is a
	// single line of text, so anything more is dead space in a list that grows.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kSkillRem));
	// A heading has no bar, so it keeps the plain line.
	const bool header = i < m_skillRows.size() && m_skillRows[i].header;
	float h = (header ? font.LineAdvance() : SkillBand(font, *m_barStyle)) +
			  kSkillRowGap * Body().h;
	// Not the FIRST heading, which would push the whole list off the tab's top
	// edge for no gain — there is nothing above it to be separated from.
	if (i > 0 && header) h += Em(kSkillGroupGapRem);
	return h;
}

void CharacterSheet::DrawSkillRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_skillRows.size()) return;
	const SkillRow& row = m_skillRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kSkillRem));
	const gfx::Rect px = Body();
	// A group heading: the label alone, in the accent the Stats tab uses for its
	// column headings, and hard against the left margin rather than indented
	// with the skills under it.
	if (row.header) {
		// Pushed down by the leading Measure reserved above it (see there).
		const float top = r.y + (i > 0 ? Em(kSkillGroupGapRem) : 0.0f);
		font.Draw(batch, row.label, Ax(px, kLeft), top, theme.accent);
		return;
	}
	// The row's line is SkillBand tall (the frame needs more than the text); the
	// text and the bar share its centre line.
	const float band = SkillBand(font, *m_barStyle);
	const float textY = r.y + (band - font.Height()) * 0.5f;
	font.Draw(batch, row.label, Ax(px, kLabelX), textY, theme.textDim);
	const float vw = font.MeasureWidth(row.level);
	font.Draw(batch, row.level, Ax(px, kValueRight) - vw, textY, theme.text);
	// Framed, the glass is kSkillGlass of the text height and the whole frame
	// fits the band; flat, the bar stands as tall as the text, as it always did.
	const bool framed = m_barStyle->framed && m_barStyle->frame;
	const float barH = framed ? kSkillGlass * font.Height() : font.Height();
	const gfx::Rect tube = FitFramedTube(
		{Ax(px, kSkillBarX), r.y + (band - barH) * 0.5f, kSkillBarW * px.w, barH}, band,
		*m_barStyle);
	const float seed = static_cast<float>(m_member) * 1.37f + static_cast<float>(i) * 0.53f;
	DrawProgressBar(batch, tube, row.frac, SkillBarColor(row.id), seed, *m_barStyle, theme);
}

float CharacterSheet::MeasureSpellRow(size_t i, ui::UIContext& ctx,
									  const ui::Font&, float widthPx) const {
	if (i >= m_spellRows.size()) return 0.0f;
	// The NAME line is interface (the list's own face); the description is the
	// world talking, so it is set in Script. Measured in the same two faces
	// DrawSpellRow draws them in — measuring the wrap in one face and drawing it
	// in another gives a row height that does not match its contents.
	const ui::Font& name = ctx.FontAt(ui::FontRole::Body, Em(kNameRem));
	const ui::Font& desc = ctx.FontAt(ui::FontRole::Script, Em(kDescRem));
	const float maxW = (kTextRight - kSpellTextX) * widthPx;
	const int lines = CountLines(desc, m_spellRows[i].desc, maxW);
	return name.Height() + Em(kNameDescGapRem) +
		   static_cast<float>(lines) * desc.LineAdvance() +
		   kSpellRowGap * Body().h;
}

gfx::Rect CharacterSheet::SpellRuneRect(ui::UIContext& ctx, const gfx::Rect& r,
										 size_t k) const {
	// Squares the height of the name line (kNameRem), left of the name, a
	// small gap apart - as DrawSpellRow lays them.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kNameRem));
	const gfx::Rect px = Body();
	const float ish = font.Height();
	const float runeGap = 0.004f * px.w;
	return {Ax(px, kSpellTextX) + static_cast<float>(k) * (ish + runeGap), r.y, ish, ish};
}

void CharacterSheet::DrawSpellRow(size_t i, ui::UIContext& ctx,
								  gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_spellRows.size()) return;
	const SpellRow& row = m_spellRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	// The name line (and with it the rune squares, which are sized off the text)
	// runs at kNameRem; MeasureSpellRow uses the same font for that line.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kNameRem));
	const gfx::Rect px = Body();
	const float textX = Ax(px, kSpellTextX);
	const float maxW = (kTextRight - kSpellTextX) * px.w;
	const float ish = font.Height(); // rune-icon square ~ the text height
	const float runeGap = 0.004f * px.w;

	float nameX = textX;
	for (size_t k = 0; k < row.symbols.size(); ++k) {
		const SpellSymbol sym = row.symbols[k];
		const gfx::Rect ir = SpellRuneRect(ctx, r, k);
		// The rune glows, as it does in every socket (DrawItemIcon).
		if (!DrawItemIcon(batch, ir, RuneItemId(sym), m_icons, 0.0f)) {
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
	const ui::Font& descFont = ctx.FontAt(ui::FontRole::Script, Em(kDescRem));
	const float descTop = r.y + font.Height() + Em(kNameDescGapRem);
	WrapLines(descFont, row.desc, maxW, [&](std::string_view line, int n) {
		descFont.Draw(batch, line, textX,
					  descTop + static_cast<float>(n) * descFont.LineAdvance(),
					  theme.textDim);
	});
}

float CharacterSheet::EffectIconSize(const ui::Font& nameFont) const {
	// Exactly the name line: its height plus the gap that separates it from the
	// description, which is where the description begins.
	return nameFont.Height() + Em(kNameDescGapRem);
}

float CharacterSheet::EffectTextInset(const ui::Font& nameFont) const {
	return kEffectIconX * Body().w + EffectIconSize(nameFont) +
		   Em(kEffectIconGapRem);
}

float CharacterSheet::MeasureEffectRow(size_t i, ui::UIContext& ctx,
									   const ui::Font&, float widthPx) const {
	if (i >= m_effectRows.size()) return 0.0f;
	// Same split as a spell row: the name/timer line is interface, the
	// description is the world, and each is measured in the face it draws in.
	const ui::Font& name = ctx.FontAt(ui::FontRole::Body, Em(kNameRem));
	const ui::Font& desc = ctx.FontAt(ui::FontRole::Script, Em(kDescRem));
	const float maxW = kTextRight * widthPx - EffectTextInset(name);
	const int lines = CountLines(desc, m_effectRows[i].desc, maxW);
	// No max() against the icon any more: it spans the name line by
	// construction, so it can never be taller than name + description.
	return name.Height() + Em(kNameDescGapRem) +
		   static_cast<float>(lines) * desc.LineAdvance() +
		   kEffectRowGap * Body().h;
}

void CharacterSheet::DrawEffectRow(size_t i, ui::UIContext& ctx,
								   gfx::SpriteBatch& batch, const gfx::Rect& r) {
	if (i >= m_effectRows.size()) return;
	const EffectRow& row = m_effectRows[i];
	const ui::Theme& theme = ctx.GetTheme();
	// Name AND duration at kNameRem, matching a spell row's name line.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Em(kNameRem));
	const gfx::Rect px = Body();

	// Square, spanning the name line: top on the name's top, bottom where the
	// description starts.
	const float iconSize = EffectIconSize(font);
	const gfx::Rect icon{Ax(px, kEffectIconX), r.y, iconSize, iconSize};
	const gfx::Rect well = ui::DrawSlotFace(ctx, batch, icon, kSlotBg);
	const gfx::Rect pic{well.x + 2, well.y + 2, well.w - 4, well.h - 4};
	const gfx::Texture* iconTex =
		m_icons && row.kind ? m_icons->For(row.kind->IconItem()) : nullptr;
	if (iconTex)
		batch.DrawSprite(pic, {0, 0, 1, 1}, *iconTex, {1, 1, 1, 1});
	else
		batch.DrawRect(pic,
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
	const ui::Font& descFont = ctx.FontAt(ui::FontRole::Script, Em(kDescRem));
	const float descTop = r.y + font.Height() + Em(kNameDescGapRem);
	WrapLines(descFont, row.desc, maxW, [&](std::string_view line, int n) {
		descFont.Draw(batch, line, textX,
					  descTop + static_cast<float>(n) * descFont.LineAdvance(),
					  theme.textDim);
	});
}

} // namespace dungeon::game
