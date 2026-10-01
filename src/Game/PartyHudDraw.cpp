// ============================================================================
// Game/PartyHudDraw.cpp — see PartyHudDraw.h.
// ============================================================================
#include "Game/PartyHudDraw.h"

#include "Game/PartyHudTypes.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace dungeon::game {

void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme) {
	batch.DrawRect(rect, theme.control);
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	if (t > 0.0f) batch.DrawRect({rect.x, rect.y, rect.w * t, rect.h}, color);
	ui::DrawBorder(batch, rect, theme.panelBorder);
}

// --- the framed resource bars ------------------------------------------------

namespace {

// bar_frame.png's geometry, as printed by tools/CutBarFrame.py - fractions of
// the frame image. Re-cut the frame, copy the new numbers here.
constexpr float kFrameAspect = 1024.0f / 175.0f; // image width / height
constexpr float kTubeLeft = 0.0858f;   // the glass, inset from each edge
constexpr float kTubeRight = 0.0841f;
constexpr float kTubeTop = 0.3016f;
constexpr float kTubeBottom = 0.2444f;
constexpr float kCapLeft = 0.1908f;    // where the end caps stop and the
constexpr float kCapRight = 0.1887f;   // plain (stretchable) rim begins

// The heartbeat's numbers, in beats per minute. A first cut, Michael's to tune.
constexpr float kBpmRest = 60.0f;
constexpr float kBpmNoticed = 120.0f;
constexpr float kBpmNearDeath = 35.0f;   // at 0 health, still standing
constexpr float kNearDeath = 0.30f;      // health fraction where the slowing starts
constexpr float kBpmEaseSeconds = 1.5f;  // time constant of a rate change

} // namespace

BarFrameReach FrameReach(float tubeH) {
	const float frameH = tubeH / (1.0f - kTubeTop - kTubeBottom);
	const float frameW = frameH * kFrameAspect; // the image's own width at this height
	return {kTubeLeft * frameW, kTubeRight * frameW, kTubeTop * frameH,
			kTubeBottom * frameH};
}

void DrawResourceBar(gfx::SpriteBatch& batch, const gfx::Rect& tube, ResourceBar which,
					 float fraction, size_t member, const ResourceBarStyle& style,
					 const ui::Theme& theme) {
	DrawResourceBarFill(batch, tube, which, fraction, member, style, theme);
	DrawResourceBarFrame(batch, tube, style);
}

void DrawResourceBarFill(gfx::SpriteBatch& batch, const gfx::Rect& tube, ResourceBar which,
						 float fraction, size_t member, const ResourceBarStyle& style,
						 const ui::Theme& theme) {
	if (style.demo) {
		// `hudbars demo`: an 8 s triangle wave, each bar a little behind the last.
		const float s = std::fmod(style.clock / 8.0f + static_cast<float>(which) * 0.07f, 1.0f);
		fraction = 1.0f - std::abs(s * 2.0f - 1.0f);
	}
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	const Vec4* flat = &style.health;
	gfx::BarFill fill;
	switch (which) {
	case ResourceBar::Health:
		flat = &style.health;
		fill.kind = gfx::BarKind::Health;
		break;
	case ResourceBar::Stamina:
		flat = &style.stamina;
		fill.kind = gfx::BarKind::Stamina;
		break;
	case ResourceBar::Mana:
		flat = &style.mana;
		fill.kind = gfx::BarKind::Mana;
		break;
	case ResourceBar::Food: flat = &style.food; break;
	case ResourceBar::Water: flat = &style.water; break;
	}
	if (!style.framed || !style.frame) {
		DrawStatBar(batch, tube, t, *flat, theme);
		return;
	}

	fill.fraction = t;
	fill.tint = *flat;
	fill.beat = style.PulseOf(member).phase;
	// Every bar its own offset, so neighbours never slosh in lockstep.
	fill.seed = static_cast<float>(member) * 1.37f + static_cast<float>(which) * 0.71f;
	batch.DrawBarFill(tube, fill);
}

void DrawResourceBarFrame(gfx::SpriteBatch& batch, const gfx::Rect& tube,
						  const ResourceBarStyle& style) {
	if (!style.framed || !style.frame || tube.h <= 0.0f) return;
	// The frame, 3-SLICED: both end caps at the image's own aspect, the plain rim
	// between them stretched to whatever width the bar is.
	const BarFrameReach reach = FrameReach(tube.h);
	const float frameH = tube.h + reach.top + reach.bottom;
	const float frameW = frameH * kFrameAspect;
	const gfx::Rect frame{tube.x - reach.left, tube.y - reach.top,
						  tube.w + reach.left + reach.right, frameH};
	float capL = kCapLeft * frameW, capR = kCapRight * frameW;
	if (capL + capR > frame.w) { // a bar shorter than its two caps: squeeze them
		const float s = frame.w / (capL + capR);
		capL *= s;
		capR *= s;
	}
	const gfx::Texture& tex = *style.frame;
	const Vec4 white{1, 1, 1, 1};
	batch.DrawSprite({frame.x, frame.y, capL, frameH}, {0, 0, kCapLeft, 1}, tex, white);
	batch.DrawSprite({frame.x + capL, frame.y, frame.w - capL - capR, frameH},
					 {kCapLeft, 0, 1.0f - kCapLeft - kCapRight, 1}, tex, white);
	batch.DrawSprite({frame.x + frame.w - capR, frame.y, capR, frameH},
					 {1.0f - kCapRight, 0, kCapRight, 1}, tex, white);
}

float HeartRateTarget(const Character& member, bool noticed) {
	if (!member.IsAlive()) return 0.0f; // down: no beat
	const float h = member.maxHealth > 0.0f ? member.health / member.maxHealth : 1.0f;
	// Near death WINS over noticed: below the threshold the heart labours,
	// fight or no fight, sliding from the resting rate down to the floor.
	if (h < kNearDeath)
		return kBpmNearDeath + (kBpmRest - kBpmNearDeath) * (h / kNearDeath);
	return noticed ? kBpmNoticed : kBpmRest;
}

void TickBarPulse(BarPulse& pulse, float targetBpm, float dt) {
	if (dt <= 0.0f) return;
	pulse.bpm += (targetBpm - pulse.bpm) * (1.0f - std::exp(-dt / kBpmEaseSeconds));
	pulse.phase += dt * pulse.bpm / 60.0f;
	pulse.phase -= std::floor(pulse.phase);
}

Vec4 MutedIdentity(const Vec4& color, bool down) {
	// Halfway to its own grey, then darkened to sit at the stone's depth.
	const float grey = color.x * 0.3f + color.y * 0.59f + color.z * 0.11f;
	const float sat = 0.55f, dark = down ? 0.35f : 0.62f;
	const auto mix = [&](float c) { return (grey + (c - grey) * sat) * dark; };
	return {mix(color.x), mix(color.y), mix(color.z), 1.0f};
}

namespace {
// The portrait frame's groove and the breath of stone between it and the
// picture, as shares of the portrait's width.
float PortraitGroove(const gfx::Rect& r) { return std::max(3.0f, r.w * 0.033f); }
float PortraitInset(const gfx::Rect& r) { return PortraitGroove(r) + std::max(2.0f, r.w * 0.035f); }
} // namespace

void DrawIdentityBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect,
						const Character& character) {
	// A groove carved round the picture, its floor the member's colour muted
	// into the stone (Michael: the bright glowing frame was far too loud).
	ui::DrawCarvedGroove(batch, rect, PortraitGroove(rect),
						 MutedIdentity(character.portraitColor, !character.IsAlive()));
}

void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme) {
	// The picture sits a breath of stone in from its groove (Michael: space it
	// out inside the border), the frame round the slot's own edge.
	const float in = PortraitInset(rect);
	const gfx::Rect pic{rect.x + in, rect.y + in, rect.w - 2 * in, rect.h - 2 * in};
	DrawIdentityBorder(batch, rect, character);
	if (character.portrait) {
		batch.DrawSprite(pic, {0, 0, 1, 1}, *character.portrait, {1, 1, 1, 1});
		return;
	}
	batch.DrawRect(pic, character.portraitColor);
	const std::string_view initial = std::string_view(character.name).substr(0, 1);
	const float initialW = font.MeasureWidth(initial);
	font.Draw(batch, initial, rect.x + (rect.w - initialW) * 0.5f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
}

Vec4 RuneGlowColor(SpellSymbol s) {
	// The icons' own glyph colours, as tools/BuildRuneGlow.py prints them.
	switch (s) {
	case SpellSymbol::Fire: return {0.95f, 0.45f, 0.18f, 1.0f};
	case SpellSymbol::Earth: return {0.45f, 0.80f, 0.32f, 1.0f};
	case SpellSymbol::Air: return {0.80f, 0.92f, 1.00f, 1.0f};
	case SpellSymbol::Water: return {0.30f, 0.55f, 0.95f, 1.0f};
	default: return {1.0f, 1.0f, 1.0f, 1.0f}; // a form rune: no school, white
	}
}

void DrawRuneGlow(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled, float phase) {
	const size_t i = static_cast<size_t>(s);
	const gfx::Texture* glyph =
		icons && i < ItemIconBank::kRuneSlots ? icons->runeGlyph[i] : nullptr;
	const gfx::Texture* glow =
		icons && i < ItemIconBank::kRuneSlots ? icons->runeGlow[i] : nullptr;
	if (!glyph || !glow) {
		DrawRuneFace(batch, r, s, icons, hot, disabled, /*background*/ false);
		return;
	}
	const Vec4 c = RuneGlowColor(s);
	const float pad = r.w * 0.08f;
	const gfx::Rect in{r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad};
	if (disabled) {
		// Spent or blocked: the mark is still there, but nothing about it is lit.
		batch.DrawSprite(in, {0, 0, 1, 1}, *glyph, {c.x * 0.4f, c.y * 0.4f, c.z * 0.4f, 0.8f});
		return;
	}
	// The halo breathes; hovering lifts it. Slow, so a grid of them shimmers
	// rather than flashes.
	const float pulse = 0.5f + 0.5f * std::sin(phase);
	const float halo = (hot ? 0.70f : 0.40f) + 0.30f * pulse;
	batch.DrawSprite(in, {0, 0, 1, 1}, *glow, {c.x, c.y, c.z, halo});
	// The glyph itself: the colour lifted toward white, a touch more at the crest,
	// so the stroke reads as the bright core of its own light.
	const float lift = 0.35f + 0.20f * pulse + (hot ? 0.15f : 0.0f);
	batch.DrawSprite(in, {0, 0, 1, 1}, *glyph,
					 {c.x + (1.0f - c.x) * lift, c.y + (1.0f - c.y) * lift,
					  c.z + (1.0f - c.z) * lift, 1.0f});
}

void DrawRuneFace(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled,
				  bool background) {
	if (background) batch.DrawRect(r, hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg);
	const gfx::Texture* icon = icons ? icons->For(RuneItemId(s)) : nullptr;
	if (icon) {
		const float pad = r.w * 0.08f;
		batch.DrawSprite({r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad},
						 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	} else {
		// Fallback: an element-tinted fill (ElementColor is premultiplied
		// additive - rebuild it opaque for flat UI ink).
		const Vec4 e = ElementColor(s);
		batch.DrawRect({r.x + 3, r.y + 3, r.w - 6, r.h - 6},
					   {e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
	}
	const Vec4 e = ElementColor(s);
	if (disabled) {
		// Washed out under a dark overlay, border flattened - reads as "spent".
		batch.DrawRect(r, {0.0f, 0.0f, 0.0f, 0.62f});
		ui::DrawBorder(batch, r, {e.x * 0.25f, e.y * 0.25f, e.z * 0.25f, 1.0f});
		return;
	}
	ui::DrawBorder(batch, r, hot ? Vec4{e.x, e.y, e.z, 1.0f}
								 : Vec4{e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
}

} // namespace dungeon::game
