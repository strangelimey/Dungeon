// ============================================================================
// Game/PartyHudDraw.cpp — see PartyHudDraw.h.
// ============================================================================
#include "Game/PartyHudDraw.h"

#include "Game/PartyHudTypes.h"

#include "Core/Loc.h"

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
// the frame image. Re-cut the frame, copy the new numbers here. The SILVER
// frame (Mana Status Bars #13) since ui-updates; the iron one was too dark.
constexpr float kFrameAspect = 1024.0f / 160.0f; // image width / height
constexpr float kTubeLeft = 0.0709f;   // the glass, inset from each edge
constexpr float kTubeRight = 0.0696f;
constexpr float kTubeTop = 0.2524f;
constexpr float kTubeBottom = 0.2220f;
constexpr float kCapLeft = 0.1332f;    // where the end caps stop and the
constexpr float kCapRight = 0.1319f;   // plain (stretchable) rim begins

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

gfx::Rect FitFramedTube(gfx::Rect box, float roomH, const ResourceBarStyle& style) {
	if (!style.framed || !style.frame) return box;
	const BarFrameReach unit = FrameReach(1.0f);
	const float fit = roomH * kFramedRowShare / (1.0f + unit.top + unit.bottom);
	if (box.h > fit) {
		box.y += (box.h - fit) * 0.5f;
		box.h = fit;
	}
	const BarFrameReach reach = FrameReach(box.h);
	box.x += reach.left;
	box.w = std::max(box.w - reach.left - reach.right, 0.0f);
	return box;
}

void DrawProgressBar(gfx::SpriteBatch& batch, const gfx::Rect& tube, float fraction,
					 const Vec4& tint, float seed, const ResourceBarStyle& style,
					 const ui::Theme& theme) {
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	if (!style.framed || !style.frame) {
		DrawStatBar(batch, tube, t, tint, theme);
		return;
	}
	gfx::BarFill fill;
	fill.kind = gfx::BarKind::Progress;
	fill.fraction = t;
	fill.tint = tint;
	fill.seed = seed;
	batch.DrawBarFill(tube, fill);
	DrawResourceBarFrame(batch, tube, style);
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
	case ResourceBar::Food:
		flat = &style.food;
		fill.kind = gfx::BarKind::Food;
		break;
	case ResourceBar::Water:
		flat = &style.water;
		fill.kind = gfx::BarKind::Water;
		break;
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

void DrawRuneTip(ui::UIContext& ctx, gfx::SpriteBatch& batch, const ui::Font& font,
				 const gfx::Rect& anchor, SpellSymbol s) {
	const loc::Line text = loc::FormatLine("rune.tip", loc::Line(loc::View(RuneNameKey(s))),
										   loc::ViewKey("symbol.", SymbolId(s)));
	const float em = font.Height();
	const float padX = em * 0.6f, padY = em * 0.35f, gapY = em * 0.3f;
	const float w = font.MeasureWidth(text.View()) + 2.0f * padX;
	const float h = font.Height() + 2.0f * padY;
	const gfx::Rect tip = ui::PlaceTooltip(anchor, w, h, {0, 0, ctx.Width(), ctx.Height()},
										   ui::TipSide::Below, gapY, padX);
	// The hand box's tooltip face (HandSlot::DrawOverlaySelf): near-opaque, as it
	// sits over the world view and other HUD.
	batch.DrawRect(tip, {0.10f, 0.10f, 0.13f, 0.97f});
	ui::DrawBorder(batch, tip, ctx.GetTheme().panelBorder);
	font.Draw(batch, text.View(), tip.x + padX, tip.y + padY, ctx.GetTheme().text);
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

void DrawHeldFlame(gfx::SpriteBatch& batch, const gfx::Rect& in, const Vec2& at,
				   const gfx::Texture& flame, const gfx::Texture* glow, const Vec3* tint) {
	// Where the head is, and how tall the flame may stand: about two fifths of
	// the icon, but never past the top of it, so a head near the top edge
	// burns low rather than spilling out of its socket.
	const float bx = in.x + at.x * in.w;
	const float by = in.y + at.y * in.h;
	const float tall = std::min(in.h * 0.42f, (by - in.y) + in.h * 0.06f);
	// Seeded by where the socket sits, so two torches side by side do not
	// burn in lockstep.
	DrawFlame(batch, {bx, by}, tall, in.w * 0.50f, in.x * 0.37f + in.y * 0.11f, flame, glow,
			  tint);
}

void DrawFlame(gfx::SpriteBatch& batch, const Vec2& head, float tall, float glowSize,
			   float seed, const gfx::Texture& flame, const gfx::Texture* glow,
			   const Vec3* tint) {
	if (tall <= 1.0f) return;
	const float bx = head.x, by = head.y;
	// Three flickers out of step. Fast, like a real flame.
	const float t = batch.Time();
	const float f1 = 0.5f + 0.5f * std::sin(t * 9.1f + seed);
	const float f2 = 0.5f + 0.5f * std::sin(t * 14.3f + seed * 1.7f);
	const float f3 = std::sin(t * 6.3f + seed * 0.6f);
	const float h = tall * (0.88f + 0.16f * f1);
	const float w = h * 0.52f * (0.95f + 0.10f * f2);
	const float sway = w * 0.08f * f3;
	// The base sinks a little into the head, so the flame grows out of it.
	const float base = by + h * 0.10f;
	// Body, heart, core: each smaller, brighter, swaying a little less. A TINTED
	// flame (a magical torch's `flame_color`) takes its body from the tint and
	// whitens toward the core, as the authored orange does.
	struct Layer {
		float scale, sway;
		Vec4 color;
	};
	Layer layers[] = {
		{1.00f, 1.0f, {1.00f, 0.42f, 0.08f, 0.85f}},
		{0.70f, 0.7f, {1.00f, 0.76f, 0.24f, 0.90f}},
		{0.40f, 0.4f, {1.00f, 0.96f, 0.84f, 0.95f}},
	};
	Vec4 glowColor{1.0f, 0.55f, 0.15f, 1.0f};
	if (tint) {
		const auto whiten = [&](float k, float a) {
			return Vec4{tint->x + (1.0f - tint->x) * k, tint->y + (1.0f - tint->y) * k,
						tint->z + (1.0f - tint->z) * k, a};
		};
		layers[0].color = whiten(0.0f, 0.85f);
		layers[1].color = whiten(0.45f, 0.90f);
		layers[2].color = whiten(0.85f, 0.95f);
		glowColor = whiten(0.1f, 1.0f);
	}
	if (glow) {
		const float g = glowSize * (1.0f + 0.12f * f1);
		const float gy = base - h * 0.40f;
		batch.DrawSprite({bx - g * 0.5f, gy - g * 0.5f, g, g}, {0, 0, 1, 1}, *glow,
						 {glowColor.x, glowColor.y, glowColor.z, 0.30f + 0.12f * f2});
	}
	for (const Layer& l : layers) {
		const float lh = h * l.scale, lw = w * l.scale;
		batch.DrawSprite({bx - lw * 0.5f + sway * l.sway, base - lh, lw, lh}, {0, 0, 1, 1}, flame,
						 l.color);
	}
}

bool DrawItemIcon(gfx::SpriteBatch& batch, const gfx::Rect& r, std::string_view typeId,
				  const ItemIconBank* icons, float pad, bool symbolic) {
	if (typeId.empty() || !icons) return false;
	if (SpellSymbol s; RuneSymbolFromItemId(typeId, s)) {
		// The Magic window's slow breath, each socket a little out of step with
		// its neighbours (keyed off where it sits, so a row shimmers).
		constexpr float kTwoPi = 6.2831853f;
		const float phase = batch.Time() * (kTwoPi / kRuneBreathSeconds) - (r.x + r.y) * 0.013f;
		const size_t si = static_cast<size_t>(s);
		const gfx::Texture* tablet = icons->For(typeId);
		const gfx::Texture* glow = si < ItemIconBank::kRuneSlots ? icons->runeGlow[si] : nullptr;
		if (symbolic || !icons->runeTablets || !tablet || !glow) {
			DrawRuneGlow(batch, r, s, icons, /*hot=*/false, /*disabled=*/false, phase);
			return true;
		}
		// The CARVED TABLET (Michael: in the pack, the doll, on the cursor), its
		// groove lit by the school's halo breathing over the face. The glow mask
		// is the glyph's own cell, which the tablet's texture spans across that
		// face, so laid over the face's box it sits in the groove.
		const float p = r.w * pad;
		const gfx::Rect in{r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p};
		batch.DrawSprite(in, {0, 0, 1, 1}, *tablet, {1, 1, 1, 1});
		const Vec2 lo = icons->runeFaceLo, hi = icons->runeFaceHi;
		const gfx::Rect face{in.x + lo.x * in.w, in.y + lo.y * in.h, (hi.x - lo.x) * in.w,
							 (hi.y - lo.y) * in.h};
		const Vec4 c = RuneGlowColor(s);
		const float pulse = 0.5f + 0.5f * std::sin(phase);
		batch.DrawSprite(face, {0, 0, 1, 1}, *glow, {c.x, c.y, c.z, 0.12f + 0.28f * pulse});
		return true;
	}
	const gfx::Texture* icon = icons->For(typeId);
	if (!icon) return false;
	const float p = r.w * pad;
	const gfx::Rect in{r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p};
	batch.DrawSprite(in, {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	if (const Vec2* at = icons->FlameAt(typeId); at && icons->flame)
		DrawHeldFlame(batch, in, *at, *icons->flame, icons->flameGlow, icons->FlameTint(typeId));
	return true;
}

float EffectTimeLeft(const fx::Inst& effect) {
	return effect.duration > 0.0f ? std::clamp(effect.timeLeft / effect.duration, 0.0f, 1.0f)
								  : 1.0f;
}

void DrawEffectIcon(const ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
					const fx::EffectKind* kind, const Vec4& tint, float frac,
					const ItemIconBank* icons) {
	const gfx::Rect well = ui::DrawSlotFace(ctx, batch, rect, kSlotBg);
	// The picture's inset and the sliver's thickness, as shares of the icon: 1
	// and 2 px on the HUD strip's ~20 px icon, 2 and 3 px on the sheet's ~30 px
	// one - the sizes the two copies had, now one rule.
	const float side = std::min(rect.w, rect.h);
	const float pad = std::max(1.0f, std::round(side * 0.06f));
	const float bar = std::max(2.0f, std::round(side * 0.1f));
	const gfx::Rect pic{well.x + pad, well.y + pad, std::max(0.0f, well.w - 2.0f * pad),
						std::max(0.0f, well.h - 2.0f * pad)};
	if (!kind || !DrawItemIcon(batch, pic, kind->IconItem(), icons, 0.0f, /*symbolic=*/true))
		batch.DrawRect(pic, {tint.x, tint.y, tint.z, 0.5f});
	// OPAQUE whatever `tint` carries: a school colour (ElementColor) has an
	// alpha of 0 - it is additive light - and the HUD's copy drew its sliver
	// and border in it as given, so on the strip both were invisible.
	const Vec4 ink{tint.x, tint.y, tint.z, 1.0f};
	batch.DrawRect({pic.x, pic.y + pic.h - bar, pic.w * std::clamp(frac, 0.0f, 1.0f), bar}, ink);
	ui::DrawBorder(batch, rect, ink);
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
