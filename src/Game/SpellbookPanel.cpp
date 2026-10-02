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
// The member row and rune grid sit HIGH and Cast / Clear low (Michael,
// ui-updates). A learned spell's name used to print between the grid and the
// sequence row (with the old 0.035 / 0.145 / 0.15 it landed on the grid's
// second row); it sits beside the sequence now, and the grid takes that room.
constexpr float kMemberY = 0.012f;
constexpr float kMemberH = 0.085f;
constexpr float kMemberGapX = 0.036f; // extra air for skinned button frames
constexpr float kGridY = 0.11f;
constexpr float kGridGap = 0.027f;
constexpr float kGridCell = 0.86f; // rune cells' share of the width-derived size
constexpr float kSeqGap = 0.018f;
constexpr float kCastH = 0.125f;
constexpr float kCastGap = 0.036f; // between Cast and Clear
constexpr float kSeqAboveCast = 0.03f;
constexpr float kGridAboveSeq = 0.06f; // the least gap between rune grid and sequence
// The tallest the vertical fractions above are taken of, as a share of the
// panel's width (SpellbookPanel::RefH): about the default dock's shape before
// the tray took a strip off it, the shape every number here was tuned at.
constexpr float kRefAspect = 0.9f;

// The glow's pulse for rune `slot`: one breath every ~3.4 s of REAL time (the
// bars' clock, so rest's 60x never hurries it), each slot ~75 degrees behind
// the last so the grid shimmers instead of throbbing in unison.
float RunePhase(const gfx::SpriteBatch& batch, size_t slot) {
	constexpr float kTwoPi = 6.2831853f;
	return batch.Time() * (kTwoPi / 3.4f) - static_cast<float>(slot) * 1.3f;
}
} // namespace

SpellbookPanel::SpellbookPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster,
							   const ItemIconBank* icons)
	: m_roster(roster), m_icons(icons),
	  m_placeholder(loc::Tr("hud.magic_none")), m_castLabel(loc::Tr("magic.cast")),
	  m_clearLabel(loc::Tr("magic.clear")) {
	bounds = rect;
	debugName = "SpellbookPanel";
	// The selector row and Cast / Clear are children; the rune grid and the
	// sequence row stay this panel's own (docs/ui-hierarchy.md says why).
	m_memberRow = Add<MemberRow>(gfx::Rect{kPadX, kMemberY, 1.0f - 2.0f * kPadX, kMemberH},
				   roster, &m_member,
				   [this](size_t i) { return MemberEligible(i); },
				   [this](size_t i) {
					   SelectMember(i);
					   if (onClick) onClick();
				   });
	// The action runs at the bottom of the button's push (ui::Button), so it
	// re-reads the state then rather than trusting what was true at the click.
	m_castButton = Add<ui::Button>(gfx::Rect{}, m_castLabel, [this] {
		if (m_member >= 0 && m_seqLen > 0 && onCast)
			onCast(static_cast<size_t>(m_member), Sequence());
		m_seqLen = 0; // the slate empties either way (a fizzle is spent)
	});
	m_clearButton = Add<ui::Button>(gfx::Rect{}, m_clearLabel, [this] {
		if (m_seqLen == 0) return;
		m_seqLen = 0;
		if (onClick) onClick();
	});
}

void SpellbookPanel::SetActionIcons(const gfx::Texture* cast, const gfx::Texture* clear) {
	m_castButton->faceIcon = cast;
	m_castButton->tooltip = cast ? m_castLabel : std::string();
	m_clearButton->faceIcon = clear;
	m_clearButton->tooltip = clear ? m_clearLabel : std::string();
}

float SpellbookPanel::RefH(const gfx::Rect& px) {
	return std::min(px.h, px.w * kRefAspect);
}

void SpellbookPanel::LayoutSelf(ui::UIContext&) {
	// Cast / Clear fill their row (CastRect / ClearRect) and only act on a
	// spelled sequence; with no book open they are not there at all.
	const gfx::Rect px = Pixel();
	const bool open = m_member >= 0 && px.w > 0.0f && px.h > 0.0f;
	// The member row, at its tuned height however tall the panel is, spanning
	// the panel so each button can sit exactly over the school rune in its
	// column (Michael: "line them up ... so one is above the other").
	if (px.w > 0.0f && px.h > 0.0f) {
		const float k = RefH(px) / px.h;
		m_memberRow->bounds = {0.0f, kMemberY * k, 1.0f, kMemberH * k};
		size_t i = 0;
		for (const auto& button : m_memberRow->Children()) {
			const gfx::Rect cell = ColumnRect(px, i++);
			button->bounds = {(cell.x - px.x) / px.w, 0.0f, cell.w / px.w, 1.0f};
		}
	}
	const auto place = [&](ui::Button* b, const gfx::Rect& r) {
		b->visible = open;
		if (!open) return;
		b->bounds = {(r.x - px.x) / px.w, (r.y - px.y) / px.h, r.w / px.w, r.h / px.h};
		b->enabled = m_seqLen > 0;
	};
	place(m_castButton, CastRect(px));
	place(m_clearButton, ClearRect(px));
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
		// A stone button, sunk while selected, with the member's colour laid
		// flat inside its bevel.
		const ui::Face kind = selected ? ui::Face::ButtonDown : ui::Face::Button;
		// The member's colour glows off the stone round an eligible button,
		// brightest on the open book (ui-updates: Tilo's and Maren's sank).
		if (eligible)
			ui::DrawGlow(batch, r, col, Rem(0.3f), selected ? 0.7f : (m_hot ? 0.45f : 0.3f));
		ui::DrawFace(batch, r, *skin, kind,
					 eligible ? Vec4{1, 1, 1, 1} : Vec4{0.55f, 0.55f, 0.55f, 1.0f});
		const float in = ui::FaceInset(*skin, kind);
		const gfx::Rect face{r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in};
		if (eligible) {
			const float f = selected ? 1.0f : (m_hot ? 0.95f : 0.8f);
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

gfx::Rect SpellbookPanel::ColumnRect(const gfx::Rect& px, size_t col) const {
	// The cells are kGridCell of what the width would allow, the spare going
	// into the gaps so the grid keeps its span: the panel's height is fixed, and
	// full-width cells left no room for the spell name under the grid.
	const float pad = kPadX * px.w;
	const float full = (px.w - 2 * pad - 3 * kGridGap * px.w) / 4.0f;
	const float cell = full * kGridCell;
	const float gap = (px.w - 2 * pad - 4 * cell) / 3.0f;
	return {px.x + pad + (cell + gap) * static_cast<float>(col), px.y + kGridY * RefH(px),
			cell, cell};
}

gfx::Rect SpellbookPanel::SymbolRect(const gfx::Rect& px, const RuneSlot& slot,
									 size_t rows) const {
	const gfx::Rect column = ColumnRect(px, slot.col);
	// Rows keep the authored gap. The grid must end above the sequence row:
	// three tiers of runes do not fit at the tuned size in a default-height
	// dock, so the cells shrink until they do (a third at the least, where a
	// dock squeezed that far has bigger problems than its runes).
	const float rowGap = kGridGap * px.w;
	const float n = static_cast<float>(std::max<size_t>(rows, 1));
	const float bottom = SequenceRect(px, 0).y - kGridAboveSeq * RefH(px);
	const float fit = (bottom - column.y - (n - 1.0f) * rowGap) / n;
	const float cell = std::clamp(fit, column.w / 3.0f, column.w);
	return {column.x + (column.w - cell) * 0.5f,
			column.y + (cell + rowGap) * static_cast<float>(slot.row), cell, cell};
}

gfx::Rect SpellbookPanel::SequenceRect(const gfx::Rect& px, size_t i) const {
	// Sequence sits just above Cast / Clear. Its cells keep the size they had
	// when the row held six (the grammar now stops at three): a bigger row
	// would only take height from the rune grid above it.
	constexpr size_t kAcross = 6;
	const float pad = kPadX * px.w, gap = kSeqGap * px.w;
	const float cell =
		(px.w - 2 * pad - gap * static_cast<float>(kAcross - 1)) /
		static_cast<float>(kAcross);
	const float y = CastRect(px).y - kSeqAboveCast * RefH(px) - cell;
	return {px.x + pad + (cell + gap) * static_cast<float>(i), y, cell, cell};
}

gfx::Rect SpellbookPanel::CastRect(const gfx::Rect& px) const {
	const float pad = kPadX * px.w;
	const float gap = kCastGap * px.w;
	const float w = (px.w - 2 * pad - gap) / 2.0f;
	const float h = kCastH * RefH(px);
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
	u8 col = 0;
	const auto add = [&](SpellSymbol s, bool known) {
		if (slots.count >= slots.slot.size()) return;
		slots.slot[slots.count++] = {s, known, static_cast<u8>(slots.rows), col++};
	};
	// The four school runes ALWAYS hold the top row — an unknown one keeps
	// its place as an empty frame, so the row reads as the fixed school set.
	for (SpellSymbol s : kSchoolRow) add(s, c.Knows(s));
	slots.rows = 1;
	// The forms, then the modifiers, each tier its own row below, holding only
	// what is memorized, in enum order. A tier with nothing known takes no row.
	for (const SymbolTier tier : {SymbolTier::Form, SymbolTier::Modifier}) {
		col = 0;
		for (u32 i = 0; i < kSymbolCount; ++i) {
			const auto s = static_cast<SpellSymbol>(i);
			if (TierOf(s) == tier && c.Knows(s)) add(s, true);
		}
		if (col > 0) ++slots.rows;
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
// Whether a symbol button responds given the sequence so far: the recipe
// grammar (Spells.h SymbolMayFollow) - a school first, then a form, then a
// modifier, so each row of the grid lights in its turn and goes dark once
// spent.
bool SymbolAvailable(SpellSymbol s, std::span<const SpellSymbol> sequence) {
	return SymbolMayFollow(s, sequence);
}
} // namespace

void SpellbookPanel::UpdateSelf(ui::UIContext& ctx) {
	m_hotSymbol = -1;
	m_hotSeq = -1;
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
	// never show (or cast) anything the member no longer knows. It stops at the
	// first forgotten rune: what followed it was spelled on top of it, and
	// closing the gap would break the school-form-modifier order.
	for (size_t i = 0; i < m_seqLen; ++i)
		if (!c->Knows(m_sequence[i])) {
			m_seqLen = i;
			break;
		}

	const RuneSlotList list = RuneSlots(*c);
	const std::span<const RuneSlot> slots = list.View();
	for (size_t i = 0; i < slots.size(); ++i) {
		if (!SymbolRect(px, slots[i], list.rows).Contains(mx, my)) continue;
		// Unknown school frames and unavailable symbols (spent, or out of
		// turn in the recipe order) are inert - no hover, no click.
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
	// (Cast / Clear are child buttons and have had the mouse already.)
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
				  px.y + (kMemberY + kMemberH + 0.025f) * RefH(px), theme.textDim);
		return;
	}

	// The rune grid: the school row on top (unknown schools keep their place
	// as empty frames), then a row of known forms and a row of known modifiers.
	// A symbol the sequence can't take right now (spent, or out of turn in the
	// recipe order) draws disabled until a sequence edit frees it.
	const RuneSlotList list = RuneSlots(*c);
	const std::span<const RuneSlot> slots = list.View();
	for (size_t i = 0; i < slots.size(); ++i) {
		const gfx::Rect r = SymbolRect(px, slots[i], list.rows);
		if (!slots[i].known) { // reserved school slot, not yet memorized
			ui::DrawSlotFace(ctx, batch, r, theme.control);
			continue;
		}
		// Disabled = already spelled into the sequence (or out of turn): it
		// stops responding until a sequence edit frees it.
		// Each rune GLOWS in its school's colour in its socket, pulsing a
		// little out of step with its neighbours (ui-updates).
		ui::DrawSlotFace(ctx, batch, r, theme.control);
		DrawRuneGlow(batch, r, slots[i].symbol, m_icons, static_cast<int>(i) == m_hotSymbol,
					 !SymbolAvailable(slots[i].symbol, Sequence()), RunePhase(batch, i));
	}

	// The sequence spelled out so far - kMaxSequence slots at the bottom, just
	// above Cast / Clear, filled left to right. (Their glow phases follow on
	// from the grid's, so no socket pulses in step with another.)
	for (size_t i = 0; i < kMaxSequence; ++i) {
		const gfx::Rect r = SequenceRect(px, i);
		if (i < m_seqLen) {
			ui::DrawSlotFace(ctx, batch, r, theme.control);
			DrawRuneGlow(batch, r, m_sequence[i], m_icons, static_cast<int>(i) == m_hotSeq,
						 false, RunePhase(batch, i + kSymbolCount));
		} else {
			ui::DrawSlotFace(ctx, batch, r, theme.control);
		}
	}

	// The spell those symbols resolve to - BESIDE the sequence row, in the room
	// its three cells leave on the right (it sat on a line of its own above the
	// row while that held six, and the line cost the rune grid its third row) -
	// but ONLY once this member has LEARNED it (first successful cast). An
	// unlearned recipe stays anonymous so building a sequence is genuine
	// EXPERIMENTATION: the book won't confirm a discovery before the cast does.
	// Drawn from a view of the table's own text, fitted with a trim mark (it
	// shows every frame the sequence matches, so it must build no string). No
	// "= " lead (Michael, ui-updates).
	if (const Spell* def = Match(); def && c->HasLearnedSpell(def->Id())) {
		const gfx::Rect last = SequenceRect(px, kMaxSequence - 1);
		const float x = last.x + last.w + Rem(0.5f);
		const float room = px.x + px.w - kPadX * px.w - x;
		const float y = last.y + (last.h - font.Height()) * 0.5f;
		ui::DrawFittedText(batch, font, loc::View(def->NameKey()), x, y, room, theme.accent);
	}
	// (Cast / Clear are child ui::Buttons and draw themselves.)
}

} // namespace dungeon::game
