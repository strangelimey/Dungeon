// ============================================================================
// Game/CharacterSheet_Lists.cpp — Skills / Spells / Effects tabs + scroll.
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

gfx::Rect CharacterSheet::ScrollViewRect(const gfx::Rect& px) const {
	const float top = Ay(px, kScrollTop);
	const float bot = px.y + px.h - kScrollBottomPad * px.h;
	return {px.x, top, px.w, std::max(bot - top, 0.0f)};
}
gfx::Rect CharacterSheet::ScrollThumbRect(const gfx::Rect& px, const gfx::Rect& view,
										  float maxScroll) const {
	const float barW = kScrollBarW * px.w;
	const gfx::Rect track{view.x + view.w - barW - kScrollBarPadPx, view.y, barW,
						  view.h};
	const float thumbH =
		std::max(track.h * view.h / (view.h + maxScroll), kScrollThumbMinPx);
	const float t = maxScroll > 0.0f ? m_scroll / maxScroll : 0.0f;
	return {track.x, track.y + (track.h - thumbH) * t, track.w, thumbH};
}
void CharacterSheet::UpdateScroll(ui::UIContext& ctx, const gfx::Rect& px) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	const gfx::Rect view = ScrollViewRect(px);
	const float maxScroll = std::max(0.0f, m_scrollContentH - m_scrollViewH);
	if (m_scrollDragging && !input->IsMouseDown(MouseButton::Left))
		m_scrollDragging = false;
	if (maxScroll > 0.0f) {
		const gfx::Rect thumb = ScrollThumbRect(px, view, maxScroll);
		if (!ctx.IsMouseConsumed() && thumb.Contains(mx, my) &&
			input->WasMousePressed(MouseButton::Left)) {
			m_scrollDragging = true;
			m_scrollGrab = my - thumb.y;
			ctx.ConsumeMouse();
		}
		if (m_scrollDragging) {
			const float range = view.h - thumb.h;
			if (range > 0.0f)
				m_scroll = std::clamp(
					(my - m_scrollGrab - view.y) / range * maxScroll, 0.0f, maxScroll);
			ctx.ConsumeMouse();
		} else if (!ctx.IsMouseConsumed() && px.Contains(mx, my) &&
				   input->WheelDelta() != 0.0f) {
			m_scroll = std::clamp(m_scroll - input->WheelDelta() * kWheelStepPx, 0.0f,
								  maxScroll);
		}
	}
	m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
}
void CharacterSheet::DrawScrollbar(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								   const gfx::Rect& px, const gfx::Rect& view) {
	const float maxScroll = std::max(0.0f, m_scrollContentH - m_scrollViewH);
	if (maxScroll <= 0.0f) return;
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect thumb = ScrollThumbRect(px, view, maxScroll);
	batch.DrawRect({thumb.x, view.y, thumb.w, view.h}, theme.control);
	batch.DrawRect(thumb, m_scrollDragging ? theme.accent : theme.controlHot);
	ui::DrawBorder(batch, thumb, theme.panelBorder);
}
void CharacterSheet::DrawSkills(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	font.Draw(batch, m_skillsLabel, Ax(px, kLeft), Ay(px, kHeaderY), theme.accent);
	if (m_skillRows.empty()) {
		font.Draw(batch, m_noSkills, Ax(px, kLeft), Ay(px, kEmptyListY), theme.textDim);
		m_scrollContentH = 0.0f;
		return;
	}

	const gfx::Rect view = ScrollViewRect(px);
	const float rowH = kRowH * px.h;
	batch.SetScissor(&view);
	for (size_t i = 0; i < m_skillRows.size(); ++i) {
		const SkillRow& row = m_skillRows[i];
		const float y = view.y - m_scroll + static_cast<float>(i) * rowH;
		font.Draw(batch, row.label, Ax(px, kLabelX), y, theme.textDim);
		const float vw = font.MeasureWidth(row.level);
		font.Draw(batch, row.level, Ax(px, kValueRight) - vw, y, theme.text);
		const gfx::Rect bar{Ax(px, kSkillBarX), y, kSkillBarW * px.w, kBarH * px.h};
		DrawStatBar(batch, bar, row.frac,
					row.tint.w > 0.0f ? row.tint : theme.accent, theme);
	}
	batch.SetScissor(nullptr);
	m_scrollContentH = static_cast<float>(m_skillRows.size()) * rowH;
	m_scrollViewH = view.h;
	DrawScrollbar(ctx, batch, px, view);
}
void CharacterSheet::DrawEffects(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								 const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	font.Draw(batch, m_effectsLabel, Ax(px, kLeft), Ay(px, kHeaderY), theme.accent);
	if (m_effectRows.empty()) {
		font.Draw(batch, m_noEffects, Ax(px, kLeft), Ay(px, kEmptyListY),
				  theme.textDim);
		m_scrollContentH = 0.0f;
		return;
	}

	// Word wrap: string_view slices only — no per-frame allocation.
	auto drawWrapped = [&font, &batch](std::string_view text, float x, float y,
									   float maxW, const Vec4& color) -> float {
		while (!text.empty()) {
			size_t end = text.size();
			while (end > 0 && font.MeasureWidth(text.substr(0, end)) > maxW) {
				const size_t space = text.rfind(' ', end - 1);
				if (space == std::string_view::npos || space == 0) break;
				end = space;
			}
			font.Draw(batch, text.substr(0, end), x, y, color);
			y += font.LineAdvance();
			const size_t next = text.find_first_not_of(' ', end);
			text = next == std::string_view::npos ? std::string_view{}
												  : text.substr(next);
		}
		return y;
	};

	const gfx::Rect view = ScrollViewRect(px);
	batch.SetScissor(&view);
	float y = view.y - m_scroll;
	const float rowGap = kEffectRowGap * px.h;
	for (const EffectRow& row : m_effectRows) {
		// y is absolute (scroll view); width/height still fractions of the sheet.
		const gfx::Rect icon{Ax(px, kEffectIconX), y, kEffectIconW * px.w,
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
		font.Draw(batch, row.name, textX, y, theme.text);
		const float tw = font.MeasureWidth(row.time);
		font.Draw(batch, row.time, Ax(px, kTextRight) - tw, y, theme.accent);
		const float descBottom = drawWrapped(
			row.desc, textX, y + font.LineAdvance() + 4.0f, maxW, theme.textDim);
		y = std::max(descBottom, icon.y + icon.h) + rowGap;
	}
	batch.SetScissor(nullptr);
	m_scrollContentH = y + m_scroll - view.y;
	m_scrollViewH = view.h;
	DrawScrollbar(ctx, batch, px, view);
}
void CharacterSheet::DrawSpells(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	font.Draw(batch, m_spellsLabel, Ax(px, kLeft), Ay(px, kHeaderY), theme.accent);
	if (m_spellRows.empty()) {
		font.Draw(batch, m_noSpells, Ax(px, kLeft), Ay(px, kEmptyListY), theme.textDim);
		m_scrollContentH = 0.0f;
		return;
	}

	auto drawWrapped = [&font, &batch](std::string_view text, float x, float y,
									   float maxW, const Vec4& color) -> float {
		while (!text.empty()) {
			size_t end = text.size();
			while (end > 0 && font.MeasureWidth(text.substr(0, end)) > maxW) {
				const size_t space = text.rfind(' ', end - 1);
				if (space == std::string_view::npos || space == 0) break;
				end = space;
			}
			font.Draw(batch, text.substr(0, end), x, y, color);
			y += font.LineAdvance();
			const size_t next = text.find_first_not_of(' ', end);
			text = next == std::string_view::npos ? std::string_view{} : text.substr(next);
		}
		return y;
	};

	const float textX = Ax(px, kSpellTextX);
	const float maxW = (kTextRight - kSpellTextX) * px.w;
	const float ish = font.Height(); // rune-icon square ~ the text height
	const float runeGap = 0.004f * px.w;
	const gfx::Rect view = ScrollViewRect(px);
	batch.SetScissor(&view);
	float y = view.y - m_scroll;
	const float rowGap = kSpellRowGap * px.h;
	for (const SpellRow& row : m_spellRows) {
		float nameX = textX;
		for (SpellSymbol sym : row.symbols) {
			const gfx::Rect ir{nameX, y, ish, ish};
			const gfx::Texture* ic = m_icons ? m_icons->For(RuneItemId(sym)) : nullptr;
			if (ic)
				batch.DrawSprite(ir, {0, 0, 1, 1}, *ic, {1, 1, 1, 1});
			else {
				const Vec4 sc = ElementColor(sym);
				batch.DrawRect(ir, {sc.x, sc.y, sc.z, 0.6f});
			}
			nameX += ish + runeGap;
		}
		font.Draw(batch, row.name, nameX + runeGap, y,
				  {row.tint.x, row.tint.y, row.tint.z, 1.0f});
		y = drawWrapped(row.desc, textX, y + font.LineAdvance() + 2.0f, maxW,
						theme.textDim) +
			rowGap;
	}
	batch.SetScissor(nullptr);
	m_scrollContentH = y + m_scroll - view.y;
	m_scrollViewH = view.h;
	DrawScrollbar(ctx, batch, px, view);
}

} // namespace dungeon::game
