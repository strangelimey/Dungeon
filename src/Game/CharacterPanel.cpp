// ============================================================================
// Game/CharacterPanel.cpp — see CharacterPanel.h.
// ============================================================================
#include "Game/CharacterPanel.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"
#include "UI/Skin.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

CharacterPanel::CharacterPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster, size_t member,
							   const ui::Font* portraitFont,
							   const ResourceBarColors* barColors,
							   const HitSplatIcons* hitSplats,
							   const ItemIconBank* icons,
							   std::function<void()> onClick,
							   std::function<void()> onRight,
							   std::function<void()> onBars,
							   std::function<void()> onEffects)
	: m_roster(roster), m_member(member), m_portraitFont(portraitFont),
	  m_barColors(barColors), m_hitSplats(hitSplats), m_icons(icons),
	  m_onClick(std::move(onClick)), m_onRight(std::move(onRight)),
	  m_onBars(std::move(onBars)), m_onEffects(std::move(onEffects)) {
	bounds = rect;
}

// The portrait square: left side of the panel, inset by the shared pad. The
// one layout Draw and the effect-icon hit test both resolve against.
gfx::Rect CharacterPanel::PortraitRect() const {
	const gfx::Rect& px = Pixel();
	const float pad = px.h * 0.08f;
	return {px.x + pad, px.y + pad, px.h - 2 * pad, px.h - 2 * pad};
}

// The Nth status-effect icon: a small square in the NAME band, right-aligned
// against the panel edge and growing right-to-left as effects stack (index 0
// is the rightmost). Sized to the name row (the font's line advance) so the
// row reads as part of the name line.
gfx::Rect CharacterPanel::EffectIconRect(ui::UIContext& ctx,
										 size_t index) const {
	const gfx::Rect& px = Pixel();
	const float pad = px.h * 0.08f;
	const float s = ctx.GetFont().LineAdvance();
	const float gap = px.h * 0.02f; // fraction of the panel (not fixed pixels)
	return {px.x + px.w - pad - s - (s + gap) * static_cast<float>(index),
			px.y + pad, s, s};
}

// The stat-bar strip: right of the portrait, below the name (mirrors Draw's
// layout so the click target matches what's drawn).
gfx::Rect CharacterPanel::BarsRect(ui::UIContext& ctx) const {
	const gfx::Rect& px = Pixel();
	const float pad = px.h * 0.08f;
	const float left = px.x + pad + (px.h - 2 * pad) + pad; // past the portrait
	const float right = px.x + px.w - pad;
	const float barsTop = px.y + pad + ctx.GetFont().LineAdvance() + 2.0f;
	const float barsBottom = px.y + px.h - pad;
	return {left, barsTop, right - left, barsBottom - barsTop};
}

void CharacterPanel::Update(ui::UIContext& ctx) {
	m_character = RosterMember(m_roster, m_member);
	if (!m_character) return; // roster shorter than this slot — inert
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(mx, my);
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	// Which status-effect icon (the name-band row) the cursor rests on —
	// Draw shows that effect's name + time left under the panel.
	m_hotEffect = static_cast<size_t>(-1);
	if (m_hot) {
		for (size_t i = 0; i < m_character->effects.size(); ++i)
			if (EffectIconRect(ctx, i).Contains(mx, my)) {
				m_hotEffect = i;
				break;
			}
	}
	// A click on an effect icon opens the Effects tab (the icon's long form);
	// over the stat bars (either button) the Stats tab; elsewhere on the
	// panel the portrait actions keep (left = sheet/stow, right = backpack).
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot) {
			if (m_hotEffect != static_cast<size_t>(-1) && m_onEffects)
				m_onEffects();
			else if (BarsRect(ctx).Contains(mx, my) && m_onBars) m_onBars();
			else if (m_onClick) m_onClick();
		}
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot) {
			if (BarsRect(ctx).Contains(mx, my) && m_onBars) m_onBars();
			else if (m_onRight) m_onRight();
		}
		m_heldRight = false;
	}
}

void CharacterPanel::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	if (!m_character) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	// Skinned: the panel part is the slot face (frame baked in), hover/press
	// wash the theme's control colors over it and hover keeps its accent
	// border. The flat look stays as the debug mode, exactly as before.
	const ui::Skin* skin = ctx.GetSkin();
	if (skin && skin->panel.texture) {
		ui::DrawNineSlice(batch, px, skin->panel,
						  {1, 1, 1, theme.panel.w * backgroundOpacity});
		if (m_held || m_hot) {
			Vec4 wash = m_held ? theme.controlActive : theme.controlHot;
			wash.w = 0.3f;
			batch.DrawRect(px, wash);
		}
		if (m_hot) ui::DrawBorder(batch, px, theme.accent);
	} else {
		Vec4 background =
			m_held ? theme.controlActive : (m_hot ? theme.controlHot : theme.panel);
		background.w *= backgroundOpacity;
		batch.DrawRect(px, background);
		ui::DrawBorder(batch, px, m_hot ? theme.accent : theme.panelBorder);
	}

	// Internals scale with the slot height (the slot itself is normalized).
	const float pad = px.h * 0.08f;
	const gfx::Rect portrait = PortraitRect();
	DrawPortrait(batch, portrait, *m_character, *m_portraitFont, theme);

	// Hit feedback: a transient splat over the portrait while hitFlash > 0,
	// fading out as the timer winds down (the world ticks it). No number — the
	// icon alone conveys the hit. Slightly oversized so the spatter overhangs.
	if (m_character->hitFlash > 0.0f && m_hitSplats) {
		if (const gfx::Texture* splat = m_hitSplats->For(m_character->hitSeverity)) {
			const float fade = std::clamp(m_character->hitFlash / 0.7f, 0.0f, 1.0f);
			const float grow = portrait.w * 0.14f;
			const gfx::Rect r{portrait.x - grow * 0.5f, portrait.y - grow * 0.5f,
							  portrait.w + grow, portrait.h + grow};
			batch.DrawSprite(r, {0, 0, 1, 1}, *splat, {1, 1, 1, fade});
		}
	}

	// Active status effects: one small icon per effect in the NAME band,
	// right-aligned and growing right-to-left as effects stack — the kind's
	// icon art (the Protect rune tablet for a ward) under a school-tinted
	// border, with a depleting time sliver beneath. Hovering one names it
	// under the panel (Update tracks m_hotEffect). Spill past the name is a
	// later problem — for now every effect draws.
	const std::vector<fx::Inst>& effects = m_character->effects;
	for (size_t i = 0; i < effects.size(); ++i) {
		const gfx::Rect r = EffectIconRect(ctx, i);
		const fx::Inst& e = effects[i];
		const Vec4 tint = ElementColor(e.school);
		batch.DrawRect(r, kSlotBg);
		const gfx::Texture* icon =
			m_icons ? m_icons->For(e.kind->IconItem()) : nullptr;
		if (icon)
			batch.DrawSprite({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, {0, 0, 1, 1},
							 *icon, {1, 1, 1, 1});
		else
			batch.DrawRect({r.x + 1, r.y + 1, r.w - 2, r.h - 2},
						   {tint.x, tint.y, tint.z, 0.5f});
		// Remaining-time sliver draining along the icon's bottom edge.
		const float frac =
			e.duration > 0.0f ? std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f)
							  : 1.0f;
		batch.DrawRect({r.x + 1, r.y + r.h - 3, (r.w - 2) * frac, 2}, tint);
		ui::DrawBorder(batch, r, tint);
	}

	ui::Font& font = ctx.GetFont();
	const float left = portrait.x + portrait.w + pad;
	const float right = px.x + px.w - pad;
	font.Draw(batch, m_character->name, left, px.y + pad, theme.text);

	// Three resource bars filling the slot below the name.
	const float barsTop = px.y + pad + font.LineAdvance() + 2.0f;
	const float barsBottom = px.y + px.h - pad;
	const float barGap = 4.0f;
	const float barH = (barsBottom - barsTop - 2 * barGap) / 3.0f;
	const struct {
		float value, max;
		const Vec4& color;
	} bars[] = {
		{m_character->health, m_character->maxHealth, m_barColors->health},
		{m_character->stamina, m_character->maxStamina, m_barColors->stamina},
		{m_character->mana, m_character->maxMana, m_barColors->mana},
	};
	float y = barsTop;
	for (const auto& bar : bars) {
		DrawStatBar(batch, {left, y, right - left, barH},
					bar.value / std::max(bar.max, 1.0f), bar.color, theme);
		y += barH + barGap;
	}

	// Hovered effect icon: name + time left on a small plaque just under the
	// panel (the strip icons are too small to label in place).
	if (m_hotEffect < effects.size()) {
		const fx::Inst& e = effects[m_hotEffect];
		const std::string label =
			loc::Format("hud.effect_time", loc::Tr(e.NameKey()),
						static_cast<int>(e.timeLeft + 0.5f));
		const gfx::Rect tip{px.x, px.y + px.h + 2.0f,
							font.MeasureWidth(label) + 12.0f,
							font.LineAdvance() + 6.0f};
		ui::DrawPanelFace(ctx, batch, tip);
		font.Draw(batch, label, tip.x + 6.0f, tip.y + 3.0f, theme.text);
	}
}

} // namespace dungeon::game
