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
// The most symbols a built sequence holds — recipes are 1-2 symbols today,
// six slots leave room for deeper tiers without outgrowing the magic box.
constexpr size_t kMaxSequence = 6;
} // namespace

SpellbookPanel::SpellbookPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster,
							   const ItemIconBank* icons)
	: m_roster(roster), m_icons(icons),
	  m_placeholder(loc::Tr("hud.magic_none")), m_castLabel(loc::Tr("magic.cast")),
	  m_clearLabel(loc::Tr("magic.clear")) {
	bounds = rect;
}

void SpellbookPanel::SelectMember(size_t member) {
	m_member = static_cast<int>(member);
	m_sequence.clear();
}

void SpellbookPanel::Close() {
	m_member = -1;
	m_sequence.clear();
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

gfx::Rect SpellbookPanel::MemberButtonRect(const gfx::Rect& px, size_t i) const {
	const float pad = kPadX * px.w, gap = kMemberGapX * px.w;
	const float cell = (px.w - 2 * pad - 3 * gap) / 4.0f;
	return {px.x + pad + (cell + gap) * static_cast<float>(i),
			px.y + kMemberY * px.h, cell, kMemberH * px.h};
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
	return {px.x + pad, px.y + px.h - pad - h, w, h};
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

std::vector<SpellbookPanel::RuneSlot>
SpellbookPanel::RuneSlots(const Character& c) const {
	std::vector<RuneSlot> slots;
	// The four school runes ALWAYS hold the top row — an unknown one keeps
	// its place as an empty frame, so the row reads as the fixed school set.
	for (SpellSymbol s : kSchoolRow) slots.push_back({s, c.Knows(s)});
	// Everything else appears below only once memorized, in enum order.
	for (u32 i = 0; i < kSymbolCount; ++i) {
		const auto s = static_cast<SpellSymbol>(i);
		if (!IsSchoolSymbol(s) && c.Knows(s)) slots.push_back({s, true});
	}
	return slots;
}

const Spell* SpellbookPanel::Match() const {
	if (!spells || m_sequence.empty()) return nullptr;
	for (const auto& def : spells())
		if (std::ranges::equal(def->Sequence(), m_sequence)) return def.get();
	return nullptr;
}

namespace {
// Whether a symbol button responds given the sequence so far. School (element)
// runes are mutually exclusive — one picks the spell's school, then all four
// go dark; a spell also STARTS with its school, so until one is down every
// other symbol waits. Any symbol already spelled in is spent (no repeats).
bool SymbolAvailable(SpellSymbol s, const std::vector<SpellSymbol>& sequence) {
	if (std::ranges::find(sequence, s) != sequence.end()) return false;
	const bool haveSchool =
		!sequence.empty() && IsSchoolSymbol(sequence.front());
	return IsSchoolSymbol(s) ? !haveSchool : haveSchool;
}
} // namespace

void SpellbookPanel::DrawRune(gfx::SpriteBatch& batch, const gfx::Rect& r,
							  SpellSymbol s, bool hot, bool disabled) const {
	batch.DrawRect(r, hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg);
	const gfx::Texture* icon = m_icons ? m_icons->For(RuneItemId(s)) : nullptr;
	if (icon) {
		const float pad = r.w * 0.08f;
		batch.DrawSprite({r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad},
						 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	} else {
		// Fallback: an element-tinted fill (ElementColor is premultiplied
		// additive — rebuild it opaque for flat UI ink).
		const Vec4 e = ElementColor(s);
		batch.DrawRect({r.x + 3, r.y + 3, r.w - 6, r.h - 6},
					   {e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
	}
	const Vec4 e = ElementColor(s);
	if (disabled) {
		// Already spelled into the sequence: washed out under a dark overlay,
		// border flattened — reads as "spent" and stops responding.
		batch.DrawRect(r, {0.0f, 0.0f, 0.0f, 0.62f});
		ui::DrawBorder(batch, r, {e.x * 0.25f, e.y * 0.25f, e.z * 0.25f, 1.0f});
		return;
	}
	ui::DrawBorder(batch, r, hot ? Vec4{e.x, e.y, e.z, 1.0f}
								 : Vec4{e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
}

void SpellbookPanel::Update(ui::UIContext& ctx) {
	m_hotSymbol = -1;
	m_hotSeq = -1;
	m_hotMember = -1;
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

	// The member selector row — a disabled button (absent/down member) is
	// inert: no hover, no click.
	for (size_t i = 0; i < 4; ++i) {
		if (!MemberButtonRect(px, i).Contains(mx, my)) continue;
		if (MemberEligible(i)) {
			m_hotMember = static_cast<int>(i);
			if (pressed && static_cast<int>(i) != m_member) {
				SelectMember(i);
				if (onClick) onClick();
			}
		}
		break;
	}
	if (m_member < 0) {
		ctx.ConsumeMouse(); // the selector row owns clicks over the box
		return;
	}
	const Character* c = RosterMember(m_roster, static_cast<size_t>(m_member));
	if (!c) return; // unreachable after the eligibility check; belt-and-braces
	// Self-heal: a roster reset may have taken symbols back; the sequence must
	// never show (or cast) anything the member no longer knows.
	std::erase_if(m_sequence,
				  [c](SpellSymbol s) { return !c->Knows(s); });

	const std::vector<RuneSlot> slots = RuneSlots(*c);
	for (size_t i = 0; i < slots.size(); ++i) {
		if (!SymbolRect(px, i).Contains(mx, my)) continue;
		// Unknown school frames and unavailable symbols (spent, or blocked by
		// the school rule) are inert — no hover, no click.
		if (!slots[i].known || !SymbolAvailable(slots[i].symbol, m_sequence))
			break;
		m_hotSymbol = static_cast<int>(i);
		if (pressed && m_sequence.size() < kMaxSequence) {
			m_sequence.push_back(slots[i].symbol);
			if (onClick) onClick();
		}
	}
	for (size_t i = 0; i < m_sequence.size(); ++i) {
		if (!SequenceRect(px, i).Contains(mx, my)) continue;
		m_hotSeq = static_cast<int>(i);
		if (pressed) {
			// Remove this symbol AND everything spelled after it — the tail
			// was built on top of it, so it goes too.
			m_sequence.resize(i);
			if (onClick) onClick();
			break;
		}
	}
	if (CastRect(px).Contains(mx, my)) {
		m_hotCast = true;
		if (pressed && !m_sequence.empty()) {
			if (onCast) onCast(static_cast<size_t>(m_member), m_sequence);
			m_sequence.clear(); // the slate empties either way (a fizzle is spent)
		}
	}
	if (ClearRect(px).Contains(mx, my)) {
		m_hotClear = true;
		if (pressed && !m_sequence.empty()) {
			m_sequence.clear();
			if (onClick) onClick();
		}
	}
	ctx.ConsumeMouse(); // the open book owns clicks over its box
}

void SpellbookPanel::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	const gfx::Rect px = Pixel();
	const Character* c =
		m_member < 0 ? nullptr : RosterMember(m_roster, static_cast<size_t>(m_member));

	// The member selector row: one button per party slot in the member's
	// identity color — pressed = the open book, washed out = absent/down.
	// Skinned, the identity color TINTS the wood face (lerped toward white
	// first so a dark identity blue doesn't crush the grain to black),
	// stepped by state; the flat path stays as the skinless fallback.
	const ui::Skin* skin = ctx.GetSkin();
	for (size_t i = 0; i < 4; ++i) {
		const gfx::Rect r = MemberButtonRect(px, i);
		const Character* m =
			m_roster && i < m_roster->size() ? &(*m_roster)[i] : nullptr;
		const bool eligible = MemberEligible(i);
		const bool selected = static_cast<int>(i) == m_member;
		const bool hot = static_cast<int>(i) == m_hotMember;
		const Vec4 col = m ? m->portraitColor : theme.control;
		if (skin && skin->button.texture) {
			// The FRAME stays natural wood/iron (untinted); the FACE is a flat
			// identity fill inside the frame ring — pure color, no grain
			// (Michael's call after the tinted-wood cuts read muddy).
			ui::DrawNineSlice(batch, r, skin->button,
							  eligible ? Vec4{1, 1, 1, 1}
									   : Vec4{0.55f, 0.55f, 0.55f, 1.0f});
			const float ring = skin->button.corner * skin->button.scale;
			const float in = std::max(2.0f, ring - 2.0f);
			const gfx::Rect face{r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in};
			if (eligible) {
				const float f = selected ? 1.0f : (hot ? 0.95f : 0.75f);
				batch.DrawRect(face, {col.x * f, col.y * f, col.z * f, 1.0f});
			} else {
				batch.DrawRect(face, theme.control);
			}
			if (selected) ui::DrawBorder(batch, r, theme.accent);
		} else if (!eligible) {
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
		} else {
			const float f = selected ? 0.85f : (hot ? 0.5f : 0.3f);
			batch.DrawRect(r, {col.x * f, col.y * f, col.z * f, 1.0f});
			ui::DrawBorder(batch, r,
						   selected ? theme.accent
									: Vec4{col.x, col.y, col.z, 1.0f});
		}
	}
	// No selection: the placeholder line where the grid would start. (No name
	// line — the pressed button and portrait row already say whose book.)
	if (!c) {
		const gfx::Rect b0 = MemberButtonRect(px, 0);
		font.Draw(batch, m_placeholder, px.x + kPadX * px.w,
				  b0.y + b0.h + 0.025f * px.h, theme.textDim);
		return;
	}

	// The rune grid: the school row on top (unknown schools keep their place
	// as empty frames), learned runes below. A symbol the sequence can't take
	// right now (spent, or blocked by the one-school rule) draws disabled
	// until a sequence edit frees it.
	const std::vector<RuneSlot> slots = RuneSlots(*c);
	for (size_t i = 0; i < slots.size(); ++i) {
		const gfx::Rect r = SymbolRect(px, i);
		if (!slots[i].known) { // reserved school slot, not yet memorized
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
			continue;
		}
		DrawRune(batch, r, slots[i].symbol, static_cast<int>(i) == m_hotSymbol,
				 !SymbolAvailable(slots[i].symbol, m_sequence));
	}

	// The sequence spelled out so far — six slots at the bottom, just above
	// Cast / Clear, filled left to right.
	for (size_t i = 0; i < kMaxSequence; ++i) {
		const gfx::Rect r = SequenceRect(px, i);
		if (i < m_sequence.size()) {
			DrawRune(batch, r, m_sequence[i], static_cast<int>(i) == m_hotSeq);
		} else {
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
		}
	}

	// The spell those symbols resolve to — on the line above the sequence row,
	// but ONLY once this member has LEARNED it (first successful cast). An
	// unlearned recipe stays anonymous so building a sequence is genuine
	// EXPERIMENTATION: the book won't confirm a discovery before the cast does.
	const gfx::Rect seq0 = SequenceRect(px, 0);
	if (const Spell* def = Match(); def && c->HasLearnedSpell(def->Id()))
		font.Draw(batch, "= " + loc::Tr(def->NameKey()), px.x + kPadX * px.w,
				  seq0.y - 0.025f * px.h - font.Height(), theme.accent);

	// Cast / Clear. With icon faces (round buttons with their own chrome +
	// alpha) each draws centered at the rect's height — the WHOLE rect stays
	// the hit target, so the small circles keep the generous click area.
	// Without icons, the localized text buttons return.
	const bool armed = !m_sequence.empty();
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
