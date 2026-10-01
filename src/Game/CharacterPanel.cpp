// ============================================================================
// Game/CharacterPanel.cpp — see CharacterPanel.h.
// ============================================================================
#include "Game/CharacterPanel.h"

#include "Core/Loc.h"
#include "Game/PartyHudDraw.h"
#include "UI/Skin.h"
#include "UI/Units.h"

#include <algorithm>
#include <cmath>
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
	// Drawn every frame the icon is hot, so it formats inline (a loc::Line)
	// rather than into a std::string.
	const loc::Line label =
		loc::FormatLine("hud.effect_time", loc::View(effect->NameKey()),
						static_cast<int>(effect->timeLeft + 0.5f));
	// Under the icon, kept wholly on screen (the rightmost member's tip used to
	// run off the window's edge).
	const gfx::Rect tip = ui::PlaceTooltip(r, font.MeasureWidth(label) + Rem(0.7f),
										   font.LineAdvance() + Rem(0.35f),
										   {0, 0, ctx.Width(), ctx.Height()},
										   ui::TipSide::Below, Rem(0.35f), 2.0f,
										   ui::TipAlign::Start);
	ui::DrawPanelFace(ctx, batch, tip);
	font.Draw(batch, label, tip.x + Rem(0.35f), tip.y + Rem(0.18f),
			  ctx.GetTheme().text);
}

// --- NameTag ---------------------------------------------------------------

NameTag::NameTag(const std::vector<Character>* roster, size_t member)
	: m_roster(roster), m_member(member) {
	debugName = "NameTag";
}

bool NameTag::IsLeader() const {
	const int leader = m_link && m_link->leader ? m_link->leader() : 0;
	return leader == static_cast<int>(m_member);
}

void NameTag::UpdateSelf(ui::UIContext& ctx) {
	m_hot = false;
	const Character* c = RosterMember(m_roster, m_member);
	const Input* input = ctx.CurrentInput();
	if (!c || !input || !m_link) return;
	// A click on the name picks the leader - a standing member only, so a
	// downed one's name is just a name (the world would refuse it anyway).
	const bool over =
		!ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (over && c->IsAlive()) {
		m_hot = true;
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (m_held && input->WasMouseReleased(MouseButton::Left)) {
			m_held = false;
			if (!IsLeader() && m_link->pick) m_link->pick(m_member);
		}
	}
	if (!input->IsMouseDown(MouseButton::Left)) m_held = false;
}

void NameTag::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* c = RosterMember(m_roster, m_member);
	if (!c) return;
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const float w = font.MeasureWidth(c->name);
	const bool leader = IsLeader();
	// THE LEADER: the accent, over a soft glow of it hugging the word.
	if (leader) {
		const gfx::Rect word{px.x, px.y + font.Height() * 0.18f, w, font.Height() * 0.7f};
		ui::DrawGlow(batch, word, theme.accent, Em(0.45f), 0.22f);
	}
	const Vec4 ink = leader ? theme.accent : theme.text;
	font.Draw(batch, c->name, px.x, px.y, ink);
	// A name that could take the lead, hovered: underlined, like a link.
	if (m_hot && !leader)
		batch.DrawRect({px.x, std::round(px.y + font.Height() * 0.98f), w, 1.0f},
					   {ink.x, ink.y, ink.z, 0.8f});
}

void NameTag::DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_hot || !m_link) return;
	const std::string& line = IsLeader() ? m_link->leaderTip : m_link->pickTip;
	if (line.empty()) return;
	const ui::Font& font = TextFont();
	const gfx::Rect tip = ui::PlaceTooltip(Pixel(), font.MeasureWidth(line) + Rem(0.7f),
										   font.LineAdvance() + Rem(0.35f),
										   {0, 0, ctx.Width(), ctx.Height()},
										   ui::TipSide::Below, Rem(0.35f), 2.0f,
										   ui::TipAlign::Start);
	ui::DrawPanelFace(ctx, batch, tip);
	font.Draw(batch, line, tip.x + Rem(0.35f), tip.y + Rem(0.18f), ctx.GetTheme().text);
}

// --- StatsArea -------------------------------------------------------------

StatsArea::StatsArea(const std::vector<Character>* roster, size_t member,
					 const ResourceBarStyle* barStyle,
					 std::function<void()> onBars)
	: m_roster(roster), m_member(member), m_barStyle(barStyle),
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
	// The rects below are the GLASS, and each frame reaches past its tube
	// (FrameReach). The frames must stay inside the member's slot (Michael,
	// 2026-09-30: the chrome overlapped the character container), AND clear of
	// each other (Michael, ui-updates: the silver caps reach nearly a
	// tube-height each way and interleaved into one another) - so what stacks
	// is WHOLE frames, each tube plus its reach above and below, a small gap
	// apart.
	const bool framed = m_barStyle->framed && m_barStyle->frame;
	float barGap = Rem(framed ? kFramedGapRem : 0.25f);
	float barH = (px.h - 2 * barGap) / 3.0f;
	float x = px.x, w = px.w, top = px.y;
	if (framed) {
		const BarFrameReach unit = FrameReach(1.0f); // reaches per px of tube
		const float span = 1.0f + unit.top + unit.bottom; // one frame, per px of tube
		barH = (px.h - 2 * barGap) / (3.0f * span);
		const BarFrameReach reach = FrameReach(barH);
		x += reach.left;
		w = std::max(w - reach.left - reach.right, 0.0f);
		top += reach.top;
		// From one tube's top to the next is one whole frame plus the gap.
		barGap += reach.top + reach.bottom;
	}
	const struct {
		float value, max;
		ResourceBar which;
	} bars[] = {
		{c->health, c->maxHealth, ResourceBar::Health},
		{c->stamina, c->maxStamina, ResourceBar::Stamina},
		{c->mana, c->maxMana, ResourceBar::Mana},
	};
	auto tube = [&](size_t i) {
		return gfx::Rect{x, top + static_cast<float>(i) * (barH + barGap), w, barH};
	};
	for (size_t i = 0; i < std::size(bars); ++i)
		DrawResourceBarFill(batch, tube(i), bars[i].which,
							bars[i].value / std::max(bars[i].max, 1.0f), m_member,
							*m_barStyle, ctx.GetTheme());
	for (size_t i = 0; i < std::size(bars); ++i)
		DrawResourceBarFrame(batch, tube(i), *m_barStyle);
}

// --- CharacterPanel --------------------------------------------------------

CharacterPanel::CharacterPanel(const gfx::Rect& rect,
							   const std::vector<Character>* roster, size_t member,
							   const ResourceBarStyle* barStyle,
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
	m_name = Add<NameTag>(roster, member);
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
	m_stats = Add<StatsArea>(roster, member, barStyle, std::move(onBars));
}

// Portrait square at the left, the effect strip along the name row, the bars
// filling what is left beneath. All three are fractions of THIS slot, worked
// out from its live pixel rect because they are aspect- and font-locked.
void CharacterPanel::LayoutSelf(ui::UIContext& ctx) {
	const gfx::Rect& px = Pixel();
	const bool present = RosterMember(m_roster, m_member) != nullptr;
	m_portrait->visible = present;
	m_name->visible = present;
	m_effects->visible = present;
	m_stats->visible = present;
	if (!present || px.w <= 0.0f || px.h <= 0.0f) return;

	const float padY = kPad;               // fraction of the slot's height
	const float padX = kPad * px.h / px.w; // the same inset, in width fractions
	const float ppY = kPortraitPad;            // the portrait's tighter inset
	const float ppX = kPortraitPad * px.h / px.w;
	const float sideY = 1.0f - 2 * ppY;        // portrait square, height fractions
	const float sideX = sideY * px.h / px.w;
	m_portrait->bounds = {ppX, ppY, sideX, sideY};

	// THE NAME is kNameScale of the panel's text (Michael: "make the names
	// bigger"), so its scale is set here from the panel's own - a fontScale is
	// absolute, not a multiple, and the panel's comes from its floating panel.
	// It is measured in that same face, asked exactly as Widget::Layout will.
	const ui::FontRole role = ResolvedRole();
	const float panelScale = TextFont().Height() / ctx.FontFor(role).Height();
	const float nameScale = panelScale * kNameScale;
	m_name->fontScale = nameScale;
	const ui::Font& nameFont = ctx.FontAt(role, ctx.DesignHeight() * nameScale);
	// The name row is one line advance of the NAME tall; the effect icons keep
	// the body's line and sit centred on it.
	const float rowH = nameFont.LineAdvance() / px.h;
	const float iconH = TextFont().LineAdvance() / px.h;
	const float left = ColumnLeft(px.w, px.h); // past the portrait
	const float right = 1.0f - padX;
	// The name leads the row, as wide as the word (it is a click target: the
	// party leader's picker); the effect strip has the rest, right-aligned.
	const float nameW = std::min(
		nameFont.MeasureWidth(RosterMember(m_roster, m_member)->name) / px.w, right - left);
	m_name->bounds = {left, padY, nameW, rowH};
	const float stripLeft = std::min(right, left + nameW + Rem(0.4f) / px.w);
	m_effects->bounds = {stripLeft, padY + std::max(0.0f, rowH - iconH) * 0.5f,
						 right - stripLeft, iconH};

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

	// Skinned: a stone panel face, hover/press wash the theme's control colors
	// over it and hover keeps its accent border. The flat look stays as the
	// debug mode, exactly as before.
	const ui::Skin* skin = ctx.GetSkin();
	if (skin && skin->panel.texture) {
		ui::DrawFace(batch, px, *skin, ui::Face::Panel,
					 {1, 1, 1, theme.panel.w * backgroundOpacity});
		if (m_pressed || m_hot) {
			Vec4 wash = m_pressed ? theme.controlActive : theme.controlHot;
			wash.w = 0.2f;
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
	// (The name is the NameTag child's: it is the leader picker.)
}

} // namespace dungeon::game
