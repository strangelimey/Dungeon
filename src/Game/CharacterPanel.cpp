// ============================================================================
// Game/CharacterPanel.cpp — see CharacterPanel.h.
// ============================================================================
#include "Game/CharacterPanel.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"
#include "UI/Skin.h"
#include "UI/Units.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

// The bust's fallback initial, in rem of the HUD. It used to come from GameUI's
// 64px title Font, handed down as a raw pointer; both that 64 and the HUD's 17
// were authored against the same 900px design window and scaled by the same
// factor, so 64/17 rem reproduces the old size EXACTLY at every resolution —
// and now tracks the HUD by itself, with nothing passed in. (Roles carry a
// face, not a size, so text deliberately larger than the body has to say how
// much larger; see UIContext::FontAt.)
constexpr float kBustRem = 64.0f / 17.0f;

// --- PortraitBox -----------------------------------------------------------

PortraitBox::PortraitBox(const std::vector<Character>* roster, size_t member,
						 const HitSplatIcons* hitSplats,
						 std::function<void()> onClick,
						 std::function<void()> onRight)
	: m_roster(roster), m_member(member), m_hitSplats(hitSplats),
	  m_onClick(std::move(onClick)), m_onRight(std::move(onRight)) {
	debugName = "Portrait";
}

void PortraitBox::UpdateSelf(ui::UIContext& ctx) {
	if (!RosterMember(m_roster, m_member)) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	const bool hot = !ctx.IsMouseConsumed() && Pixel().Contains(mx, my);
	if (hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (hot && m_onClick) m_onClick();
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (hot && m_onRight) m_onRight();
		m_heldRight = false;
	}
}

void PortraitBox::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* character = RosterMember(m_roster, m_member);
	if (!character) return;
	const gfx::Rect& px = Pixel();
	DrawPortrait(batch, px, *character, ctx.FontAt(ui::FontRole::Display, Rem(kBustRem)),
				 ctx.GetTheme());

	// Hit feedback: a transient splat over the portrait while hitFlash > 0,
	// fading out as the timer winds down (the world ticks it). No number — the
	// icon alone conveys the hit. Slightly oversized so the spatter overhangs.
	if (character->hitFlash > 0.0f && m_hitSplats) {
		if (const gfx::Texture* splat = m_hitSplats->For(character->hitSeverity)) {
			const float fade = std::clamp(character->hitFlash / 0.7f, 0.0f, 1.0f);
			const float grow = px.w * 0.14f;
			const gfx::Rect r{px.x - grow * 0.5f, px.y - grow * 0.5f, px.w + grow,
							  px.h + grow};
			batch.DrawSprite(r, {0, 0, 1, 1}, *splat, {1, 1, 1, fade});
		}
	}
}

// --- EffectIcon ------------------------------------------------------------

EffectIcon::EffectIcon(const std::vector<Character>* roster, size_t member,
					   size_t index, const ItemIconBank* icons,
					   std::function<void()> onClick)
	: m_roster(roster), m_member(member), m_index(index), m_icons(icons),
	  m_onClick(std::move(onClick)) {
	debugName = "EffectIcon";
}

const fx::Inst* EffectIcon::Effect() const {
	const Character* character = RosterMember(m_roster, m_member);
	if (!character || m_index >= character->effects.size()) return nullptr;
	return &character->effects[m_index];
}

void EffectIcon::UpdateSelf(ui::UIContext& ctx) {
	m_hot = false;
	if (!Effect()) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && m_onClick) m_onClick();
		m_held = false;
	}
}

void EffectIcon::DrawSelf(ui::UIContext&, gfx::SpriteBatch& batch) {
	const fx::Inst* effect = Effect();
	if (!effect) return;
	const gfx::Rect& r = Pixel();
	const Vec4 tint = ElementColor(effect->school);
	batch.DrawRect(r, kSlotBg);
	const gfx::Texture* icon =
		m_icons ? m_icons->For(effect->kind->IconItem()) : nullptr;
	if (icon)
		batch.DrawSprite({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, {0, 0, 1, 1}, *icon,
						 {1, 1, 1, 1});
	else
		batch.DrawRect({r.x + 1, r.y + 1, r.w - 2, r.h - 2},
					   {tint.x, tint.y, tint.z, 0.5f});
	// Remaining-time sliver draining along the icon's bottom edge.
	const float frac =
		effect->duration > 0.0f
			? std::clamp(effect->timeLeft / effect->duration, 0.0f, 1.0f)
			: 1.0f;
	batch.DrawRect({r.x + 1, r.y + r.h - 3, (r.w - 2) * frac, 2}, tint);
	ui::DrawBorder(batch, r, tint);
}

// Name + time left on a small plaque under the slot — the strip icons are far
// too small to label in place. Overlay-drawn so it floats above whatever the
// panel sits on rather than being painted over by it.
void EffectIcon::DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const fx::Inst* effect = Effect();
	if (!m_hot || !effect) return;
	const ui::Font& font = TextFont();
	const gfx::Rect& r = Pixel();
	const std::string label =
		loc::Format("hud.effect_time", loc::Tr(effect->NameKey()),
					static_cast<int>(effect->timeLeft + 0.5f));
	const gfx::Rect tip{r.x, r.y + r.h + Rem(0.35f),
						font.MeasureWidth(label) + Rem(0.7f),
						font.LineAdvance() + Rem(0.35f)};
	ui::DrawPanelFace(ctx, batch, tip);
	font.Draw(batch, label, tip.x + Rem(0.35f), tip.y + Rem(0.18f),
			  ctx.GetTheme().text);
}

// --- StatsArea -------------------------------------------------------------

StatsArea::StatsArea(const std::vector<Character>* roster, size_t member,
					 const ResourceBarColors* barColors,
					 std::function<void()> onBars)
	: m_roster(roster), m_member(member), m_barColors(barColors),
	  m_onBars(std::move(onBars)) {
	debugName = "StatsArea";
}

void StatsArea::UpdateSelf(ui::UIContext& ctx) {
	if (!RosterMember(m_roster, m_member)) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const bool hot =
		!ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	// Either button opens the Stats tab.
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (hot && m_onBars) m_onBars();
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (hot && m_onBars) m_onBars();
		m_heldRight = false;
	}
}

void StatsArea::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* c = RosterMember(m_roster, m_member);
	if (!c) return;
	const gfx::Rect& px = Pixel();
	const float barGap = Rem(0.25f);
	const float barH = (px.h - 2 * barGap) / 3.0f;
	const struct {
		float value, max;
		const Vec4& color;
	} bars[] = {
		{c->health, c->maxHealth, m_barColors->health},
		{c->stamina, c->maxStamina, m_barColors->stamina},
		{c->mana, c->maxMana, m_barColors->mana},
	};
	float y = px.y;
	for (const auto& bar : bars) {
		DrawStatBar(batch, {px.x, y, px.w, barH}, bar.value / std::max(bar.max, 1.0f),
					bar.color, ctx.GetTheme());
		y += barH + barGap;
	}
}

// --- CharacterPanel --------------------------------------------------------

CharacterPanel::CharacterPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster, size_t member,
							   const ResourceBarColors* barColors,
							   const HitSplatIcons* hitSplats,
							   const ItemIconBank* icons,
							   std::function<void()> onClick,
							   std::function<void()> onRight,
							   std::function<void()> onBars,
							   std::function<void()> onEffects)
	: m_roster(roster), m_member(member) {
	bounds = rect;
	debugName = "CharacterPanel";
	m_portrait = Add<PortraitBox>(roster, member, hitSplats, std::move(onClick),
								  std::move(onRight));
	// One icon per live effect, right-aligned in the name band and growing
	// right-to-left as effects stack (index 0 is the rightmost). LayoutSelf
	// gives the repeater its box; the placer splits that box into cells.
	m_effects = Add<ui::Repeater>(
		gfx::Rect{},
		[roster, member, icons, onEffects](size_t i) -> std::unique_ptr<ui::Widget> {
			return std::make_unique<EffectIcon>(roster, member, i, icons, onEffects);
		},
		[roster, member] {
			const Character* c = RosterMember(roster, member);
			return c ? c->effects.size() : size_t{0};
		},
		[this](size_t i) {
			// Square cells the height of the strip, laid out right to left.
			const gfx::Rect& box = m_effects->Pixel();
			if (box.w <= 0.0f) return gfx::Rect{0, 0, 0, 0};
			const float side = box.h / box.w; // a square, in box-width fractions
			const float gap = side * 0.18f;
			return gfx::Rect{1.0f - side - (side + gap) * static_cast<float>(i), 0.0f,
							 side, 1.0f};
		});
	m_effects->debugName = "EffectsArea";
	m_stats = Add<StatsArea>(roster, member, barColors, std::move(onBars));
}

// Portrait square at the left, the effect strip along the name row, the bars
// filling what is left beneath. All three are fractions of THIS slot, worked
// out from its live pixel rect because they are aspect- and font-locked.
void CharacterPanel::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	const bool present = RosterMember(m_roster, m_member) != nullptr;
	m_portrait->visible = present;
	m_effects->visible = present;
	m_stats->visible = present;
	if (!present || px.w <= 0.0f || px.h <= 0.0f) return;

	const float padY = kPad;               // fraction of the slot's height
	const float padX = kPad * px.h / px.w; // the same inset, in width fractions
	const float sideY = 1.0f - 2 * padY;   // portrait square, height fractions
	const float sideX = sideY * px.h / px.w;
	m_portrait->bounds = {padX, padY, sideX, sideY};

	// The name row is one line advance tall, so the effect icons sit exactly
	// on the name band.
	const float rowH = TextFont().LineAdvance() / px.h;
	const float left = padX + sideX + padX; // past the portrait
	const float right = 1.0f - padX;
	m_effects->bounds = {left, padY, right - left, rowH};

	const float barsTop = padY + rowH + Rem(0.12f) / px.h;
	m_stats->bounds = {left, barsTop, right - left, 1.0f - padY - barsTop};
}

// Latched BEFORE the children claim the mouse: the slot highlights as one
// piece, so asking afterwards would read "not hovered" whenever the pointer is
// over the portrait or the bars.
void CharacterPanel::UpdateBeforeChildren(ui::UIContext& ctx) {
	m_hot = false;
	if (!RosterMember(m_roster, m_member)) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot && input->WasMousePressed(MouseButton::Left)) m_pressed = true;
	if (!input->IsMouseDown(MouseButton::Left)) m_pressed = false;
}

void CharacterPanel::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* character = RosterMember(m_roster, m_member);
	if (!character) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	// Skinned: the panel part is the slot face (frame baked in), hover/press
	// wash the theme's control colors over it and hover keeps its accent
	// border. The flat look stays as the debug mode, exactly as before.
	const ui::Skin* skin = ctx.GetSkin();
	if (skin && skin->panel.texture) {
		ui::DrawNineSlice(batch, px, skin->panel,
						  {1, 1, 1, theme.panel.w * backgroundOpacity});
		if (m_pressed || m_hot) {
			Vec4 wash = m_pressed ? theme.controlActive : theme.controlHot;
			wash.w = 0.3f;
			batch.DrawRect(px, wash);
		}
		if (m_hot) ui::DrawBorder(batch, px, theme.accent);
	} else {
		Vec4 background =
			m_pressed ? theme.controlActive : (m_hot ? theme.controlHot : theme.panel);
		background.w *= backgroundOpacity;
		batch.DrawRect(px, background);
		ui::DrawBorder(batch, px, m_hot ? theme.accent : theme.panelBorder);
	}

	// The name shares the row the effect strip sits on: name left, strip right.
	const float pad = px.h * kPad;
	const float left = px.x + pad + (px.h - 2 * pad) + pad; // past the portrait
	TextFont().Draw(batch, character->name, left, px.y + pad, theme.text);
}

} // namespace dungeon::game
