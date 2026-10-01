// ============================================================================
// Game/SpellbookPanel.cpp — see SpellbookPanel.h.
// ============================================================================
#include "Game/SpellbookPanel.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"
#include "Game/Spell/Spell.h"
#include "UI/Skin.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Horizontal / vertical pads as fractions of the panel.
constexpr float kPadX = 0.045f;
constexpr float kPadY = 0.035f;
constexpr float kMemberY = 0.035f;
constexpr float kMemberH = 0.085f;
constexpr float kMemberGapX = 0.036f; // extra air for skinned button frames
constexpr float kGridY = 0.145f;
constexpr float kGridGap = 0.027f;
constexpr float kSeqGap = 0.018f;
constexpr float kCastH = 0.15f;
constexpr float kCastGap = 0.036f; // between Cast and Clear
constexpr float kSeqAboveCast = 0.03f;
} // namespace

SpellbookPanel::SpellbookPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster,
							   const ItemIconBank* icons)
	: m_roster(roster), m_icons(icons),
	  m_placeholder(loc::Tr("hud.magic_none")), m_castLabel(loc::Tr("magic.cast")),
	  m_clearLabel(loc::Tr("magic.clear")) {
	bounds = rect;
	debugName = "SpellbookPanel";
	// The selector row is a child; the rune grid and the sequence/Cast/Clear
	// below it stay this panel's own (docs/ui-hierarchy.md says why).
	Add<MemberRow>(gfx::Rect{kPadX, kMemberY, 1.0f - 2.0f * kPadX, kMemberH},
				   roster, &m_member,
				   [this](size_t i) { return MemberEligible(i); },
				   [this](size_t i) {
					   SelectMember(i);
					   if (onClick) onClick();
				   });
}

void SpellbookPanel::SelectMember(size_t member) {
	m_member = static_cast<int>(member);
	m_seqLen = 0;
}

void SpellbookPanel::Close() {
	m_member = -1;
	m_seqLen = 0;
}

bool SpellbookPanel::MemberEligible(size_t i) const {
	// A button responds only for a member who exists, is standing, and has
	// something to spell with — no memorized symbols, no book.
	return m_roster && i < m_roster->size() && (*m_roster)[i].IsAlive() &&
		   (*m_roster)[i].knownSymbols != 0;
}

// Layout: every region is a fraction of this panel's pixel rect (parent =
// the spellbook). Top → bottom: member selector, rune grid; sequence + Cast/
// Clear anchor to the bottom.

// --- MemberButton / MemberRow ----------------------------------------------

MemberButton::MemberButton(size_t member, const std::vector<Character>* roster,
						   const int* selected,
						   std::function<bool(size_t)> eligible,
						   std::function<void(size_t)> onSelect)
	: m_member(member), m_roster(roster), m_selected(selected),
	  m_eligible(std::move(eligible)), m_onSelect(std::move(onSelect)) {
	debugName = "MemberButton";
}

void MemberButton::UpdateSelf(ui::UIContext& ctx) {
	m_hot = false;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	if (!Pixel().Contains(input->MouseX(), input->MouseY())) return;
	// A disabled button (absent / down / no symbols) is inert: it still swallows
	// the click so the box beneath it doesn't act, but shows no hover.
	ctx.ConsumeMouse();
	if (!m_eligible || !m_eligible(m_member)) return;
	m_hot = true;
	if (input->WasMousePressed(MouseButton::Left) &&
		static_cast<int>(m_member) != *m_selected && m_onSelect)
		m_onSelect(m_member);
}

// A face in the member's identity color — pressed = the open book, washed out
// = absent/down. Skinned, the FRAME stays natural wood/iron and the FACE is a
// flat identity fill inside the frame ring (pure color, no grain — Michael's
// call after the tinted-wood cuts read muddy); the flat path is the fallback.
void MemberButton::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& r = Pixel();
	const Character* m =
		m_roster && m_member < m_roster->size() ? &(*m_roster)[m_member] : nullptr;
	const bool eligible = m_eligible && m_eligible(m_member);
	const bool selected = static_cast<int>(m_member) == *m_selected;
	const Vec4 col = m ? m->portraitColor : theme.control;
	const ui::Skin* skin = ctx.GetSkin();
	if (skin && skin->button.texture) {
		ui::DrawNineSlice(batch, r, skin->button,
						  eligible ? Vec4{1, 1, 1, 1}
								   : Vec4{0.55f, 0.55f, 0.55f, 1.0f});
		const float ring = skin->button.corner * skin->button.scale;
		const float in = std::max(2.0f, ring - 2.0f);
		const gfx::Rect face{r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in};
		if (eligible) {
			const float f = selected ? 1.0f : (m_hot ? 0.95f : 0.75f);
			batch.DrawRect(face, {col.x * f, col.y * f, col.z * f, 1.0f});
		} else {
			batch.DrawRect(face, theme.control);
		}
		if (selected) ui::DrawBorder(batch, r, theme.accent);
	} else if (!eligible) {
		batch.DrawRect(r, theme.control);
		ui::DrawBorder(batch, r, theme.panelBorder);
	} else {
		const float f = selected ? 0.85f : (m_hot ? 0.5f : 0.3f);
		batch.DrawRect(r, {col.x * f, col.y * f, col.z * f, 1.0f});
		ui::DrawBorder(batch, r,
					   selected ? theme.accent : Vec4{col.x, col.y, col.z, 1.0f});
	}
}

MemberRow::MemberRow(const gfx::Rect& rect, const std::vector<Character>* roster,
					 const int* selected, std::function<bool(size_t)> eligible,
					 std::function<void(size_t)> onSelect) {
	bounds = rect;
	debugName = "MemberRow";
	// Four even columns with the authored gap, as fractions of the row.
	const float gap = kMemberGapX / (1.0f - 2.0f * kPadX);
	const float cell = (1.0f - 3.0f * gap) / 4.0f;
	for (size_t i = 0; i < 4; ++i)
		Add<MemberButton>(i, roster, selected, eligible, onSelect)->bounds = {
			(cell + gap) * static_cast<float>(i), 0.0f, cell, 1.0f};
}

gfx::Rect SpellbookPanel::SymbolRect(const gfx::Rect& px, size_t i) const {
	const float pad = kPadX * px.w, gap = kGridGap * px.w;
	const float cell = (px.w - 2 * pad - 3 * gap) / 4.0f;
	return {px.x + pad + (cell + gap) * static_cast<float>(i % 4),
			px.y + kGridY * px.h + (cell + gap) * static_cast<float>(i / 4), cell,
			cell};
}

gfx::Rect SpellbookPanel::SequenceRect(const gfx::Rect& px, size_t i) const {
	// Sequence sits just above Cast / Clear.
	const float pad = kPadX * px.w, gap = kSeqGap * px.w;
	const float cell =
		(px.w - 2 * pad - gap * static_cast<float>(kMaxSequence - 1)) /
		static_cast<float>(kMaxSequence);
	const float y = CastRect(px).y - kSeqAboveCast * px.h - cell;
	return {px.x + pad + (cell + gap) * static_cast<float>(i), y, cell, cell};
}

gfx::Rect SpellbookPanel::CastRect(const gfx::Rect& px) const {
	const float pad = kPadX * px.w;
	const float gap = kCastGap * px.w;
	const float w = (px.w - 2 * pad - gap) / 2.0f;
	const float h = kCastH * px.h;
	// Flush with the panel's bottom: the dock around it (ControlBar.h HudDock)
	// already pads that edge, and a second pad here left the row floating
	// (Michael, 2026-09-30). The sequence row hangs off this one, so it follows.
	return {px.x + pad, px.y + px.h - h, w, h};
}

gfx::Rect SpellbookPanel::ClearRect(const gfx::Rect& px) const {
	const gfx::Rect cast = CastRect(px);
	const float gap = kCastGap * px.w;
	return {cast.x + cast.w + gap, cast.y, cast.w, cast.h};
}

namespace {
// The top row's fixed school order — the docs/magic system.md schools table
// (Earth/Air/Fire/Water), not the enum's serialization order.
constexpr SpellSymbol kSchoolRow[] = {SpellSymbol::Earth, SpellSymbol::Air,
									  SpellSymbol::Fire, SpellSymbol::Water};
} // namespace

SpellbookPanel::RuneSlotList SpellbookPanel::RuneSlots(const Character& c) const {
	RuneSlotList slots;
	const auto add = [&slots](SpellSymbol s, bool known) {
		if (slots.count < slots.slot.size()) slots.slot[slots.count++] = {s, known};
	};
	// The four school runes ALWAYS hold the top row — an unknown one keeps
	// its place as an empty frame, so the row reads as the fixed school set.
	for (SpellSymbol s : kSchoolRow) add(s, c.Knows(s));
	// Everything else appears below only once memorized, in enum order.
	for (u32 i = 0; i < kSymbolCount; ++i) {
		const auto s = static_cast<SpellSymbol>(i);
		if (!IsSchoolSymbol(s) && c.Knows(s)) add(s, true);
	}
	return slots;
}

const Spell* SpellbookPanel::Match() const {
	if (!spells || m_seqLen == 0) return nullptr;
	for (const auto& def : spells())
		if (std::ranges::equal(def->Sequence(), Sequence())) return def.get();
	return nullptr;
}

namespace {
// Whether a symbol button responds given the sequence so far. School (element)
// runes are mutually exclusive — one picks the spell's school, then all four
// go dark; a spell also STARTS with its school, so until one is down every
// other symbol waits. Any symbol already spelled in is spent (no repeats).
bool SymbolAvailable(SpellSymbol s, std::span<const SpellSymbol> sequence) {
	if (std::ranges::find(sequence, s) != sequence.end()) return false;
	const bool haveSchool =
		!sequence.empty() && IsSchoolSymbol(sequence.front());
	return IsSchoolSymbol(s) ? !haveSchool : haveSchool;
}
} // namespace

void SpellbookPanel::UpdateSelf(ui::UIContext& ctx) {
	m_hotSymbol = -1;
	m_hotSeq = -1;
	m_hotCast = false;
	m_hotClear = false;
	// The selection must stay ELIGIBLE: a member who went down (or a roster
	// that shrank) deselects — their button draws disabled, never pressed.
	if (m_member >= 0 && !MemberEligible(static_cast<size_t>(m_member)))
		Close();

	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const gfx::Rect px = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!px.Contains(mx, my)) return;
	const bool pressed = input->WasMousePressed(MouseButton::Left);

	// The selector row is a child (MemberRow) and has already had the mouse.
	if (m_member < 0) {
		ctx.ConsumeMouse(); // the selector row owns clicks over the box
		return;
	}
	const Character* c = RosterMember(m_roster, static_cast<size_t>(m_member));
	if (!c) return; // unreachable after the eligibility check; belt-and-braces
	// Self-heal: a roster reset may have taken symbols back; the sequence must
	// never show (or cast) anything the member no longer knows.
	size_t kept = 0;
	for (size_t i = 0; i < m_seqLen; ++i)
		if (c->Knows(m_sequence[i])) m_sequence[kept++] = m_sequence[i];
	m_seqLen = kept;

	const RuneSlotList list = RuneSlots(*c);
	const std::span<const RuneSlot> slots = list.View();
	for (size_t i = 0; i < slots.size(); ++i) {
		if (!SymbolRect(px, i).Contains(mx, my)) continue;
		// Unknown school frames and unavailable symbols (spent, or blocked by
		// the school rule) are inert — no hover, no click.
		if (!slots[i].known || !SymbolAvailable(slots[i].symbol, Sequence()))
			break;
		m_hotSymbol = static_cast<int>(i);
		if (pressed && m_seqLen < kMaxSequence) {
			m_sequence[m_seqLen++] = slots[i].symbol;
			if (onClick) onClick();
		}
	}
	for (size_t i = 0; i < m_seqLen; ++i) {
		if (!SequenceRect(px, i).Contains(mx, my)) continue;
		m_hotSeq = static_cast<int>(i);
		if (pressed) {
			// Remove this symbol AND everything spelled after it — the tail
			// was built on top of it, so it goes too.
			m_seqLen = i;
			if (onClick) onClick();
			break;
		}
	}
	if (CastRect(px).Contains(mx, my)) {
		m_hotCast = true;
		if (pressed && m_seqLen > 0) {
			if (onCast) onCast(static_cast<size_t>(m_member), Sequence());
			m_seqLen = 0; // the slate empties either way (a fizzle is spent)
		}
	}
	if (ClearRect(px).Contains(mx, my)) {
		m_hotClear = true;
		if (pressed && m_seqLen > 0) {
			m_seqLen = 0;
			if (onClick) onClick();
		}
	}
	// The open book owns the pointer AND the wheel over its box.
	ctx.ConsumeMouse();
	ctx.ConsumeWheel();
}

void SpellbookPanel::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const gfx::Rect px = Pixel();
	const Character* c =
		m_member < 0 ? nullptr : RosterMember(m_roster, static_cast<size_t>(m_member));

	// No selection: the placeholder line where the grid would start. (No name
	// line — the pressed button and portrait row already say whose book.)
	if (!c) {
		font.Draw(batch, m_placeholder, px.x + kPadX * px.w,
				  px.y + (kMemberY + kMemberH + 0.025f) * px.h, theme.textDim);
		return;
	}

	// The rune grid: the school row on top (unknown schools keep their place
	// as empty frames), learned runes below. A symbol the sequence can't take
	// right now (spent, or blocked by the one-school rule) draws disabled
	// until a sequence edit frees it.
	const RuneSlotList list = RuneSlots(*c);
	const std::span<const RuneSlot> slots = list.View();
	for (size_t i = 0; i < slots.size(); ++i) {
		const gfx::Rect r = SymbolRect(px, i);
		if (!slots[i].known) { // reserved school slot, not yet memorized
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
			continue;
		}
		// Disabled = already spelled into the sequence (or blocked by the
		// school rule): it stops responding until a sequence edit frees it.
		DrawRuneFace(batch, r, slots[i].symbol, m_icons,
					 static_cast<int>(i) == m_hotSymbol,
					 !SymbolAvailable(slots[i].symbol, Sequence()));
	}

	// The sequence spelled out so far — six slots at the bottom, just above
	// Cast / Clear, filled left to right.
	for (size_t i = 0; i < kMaxSequence; ++i) {
		const gfx::Rect r = SequenceRect(px, i);
		if (i < m_seqLen) {
			DrawRuneFace(batch, r, m_sequence[i], m_icons,
						 static_cast<int>(i) == m_hotSeq);
		} else {
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
		}
	}

	// The spell those symbols resolve to — on the line above the sequence row,
	// but ONLY once this member has LEARNED it (first successful cast). An
	// unlearned recipe stays anonymous so building a sequence is genuine
	// EXPERIMENTATION: the book won't confirm a discovery before the cast does.
	// Drawn in two pieces from views of the table's own text: the label shows
	// every frame the sequence matches, and `"= " + loc::Tr(...)` made two
	// strings a frame.
	const gfx::Rect seq0 = SequenceRect(px, 0);
	if (const Spell* def = Match(); def && c->HasLearnedSpell(def->Id())) {
		constexpr std::string_view kPrefix = "= ";
		const float x = px.x + kPadX * px.w;
		const float y = seq0.y - 0.025f * px.h - font.Height();
		font.Draw(batch, kPrefix, x, y, theme.accent);
		font.Draw(batch, loc::View(def->NameKey()), x + font.MeasureWidth(kPrefix), y,
				  theme.accent);
	}

	// Cast / Clear. With icon faces (round buttons with their own chrome +
	// alpha) each draws centered at the rect's height — the WHOLE rect stays
	// the hit target, so the small circles keep the generous click area.
	// Without icons, the localized text buttons return.
	const bool armed = m_seqLen > 0;
	auto iconButton = [&](const gfx::Rect& r, const gfx::Texture* icon,
						  const std::string& label, bool hot) {
		if (!icon) {
			ui::DrawButtonFace(batch, font, r, label, theme, hot, false, armed);
			return;
		}
		const float d = std::min(r.h, r.w);
		const gfx::Rect ir{r.x + (r.w - d) * 0.5f, r.y + (r.h - d) * 0.5f, d, d};
		const float f = armed && hot ? 1.0f : 0.78f; // brighten on hover
		batch.DrawSprite(ir, {0, 0, 1, 1}, *icon, {f, f, f, armed ? 1.0f : 0.35f});
	};
	iconButton(CastRect(px), castIcon, m_castLabel, m_hotCast);
	iconButton(ClearRect(px), clearIcon, m_clearLabel, m_hotClear);
}

} // namespace dungeon::game
