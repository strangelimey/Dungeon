#include "UI/Controls.h"

#include "Core/Utf8.h"
#include "UI/ControlIcons.h"
#include "UI/Skin.h"
#include "UI/Units.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace dungeon::ui {

namespace {

// The context's skin when it can draw a panel face, or null when unskinned
// (flat debug mode) — the widget-side gate for every "skin or flat?" draw
// decision.
const Skin* PanelSkin(const UIContext& ctx) {
	const Skin* skin = ctx.GetSkin();
	return skin && skin->panel.texture ? skin : nullptr;
}

} // namespace

void DrawPanelFace(UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
				   float opacity) {
	if (const Skin* skin = PanelSkin(ctx)) {
		DrawFace(batch, rect, *skin, Face::Panel, {1, 1, 1, ctx.GetTheme().panel.w * opacity});
		return;
	}
	Vec4 fill = ctx.GetTheme().panel;
	fill.w *= opacity;
	batch.DrawRect(rect, fill);
	DrawBorder(batch, rect, ctx.GetTheme().panelBorder);
}

gfx::Rect DrawSlotFace(const UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
					   const Vec4& flatFill, float lift) {
	const Skin* skin = ctx.GetSkin();
	if (!skin || !skin->slot.texture) {
		batch.DrawRect(rect, flatFill);
		DrawBorder(batch, rect, ctx.GetTheme().panelBorder);
		return rect;
	}
	DrawFace(batch, rect, *skin, Face::Slot, {1, 1, 1, 1});
	const float in = FaceInset(*skin, Face::Slot);
	const gfx::Rect well{rect.x + in, rect.y + in, std::max(0.0f, rect.w - 2 * in),
						 std::max(0.0f, rect.h - 2 * in)};
	if (lift > 0.0f) batch.DrawRect(well, {1, 1, 1, lift});
	return well;
}

gfx::Rect DrawFieldFace(const UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
						FieldState state, const Vec4& flatFill, const Vec4& flatBorder) {
	const Skin* skin = ctx.GetSkin();
	if (!skin || !(skin->buttonDown.texture || skin->button.texture)) {
		batch.DrawRect(rect, flatFill);
		DrawBorder(batch, rect, flatBorder);
		return rect;
	}
	// Darker than the tab it is modelled on (Michael: "a darker, in-set look"):
	// the stone dimmed under the sunken bevel, then a black veil over the well.
	// The veil thins on hover so the control answers the pointer.
	DrawFace(batch, rect, *skin, Face::ButtonDown, {0.8f, 0.8f, 0.8f, 1.0f});
	const float in = std::min(FaceInset(*skin, Face::ButtonDown),
							  std::min(rect.w, rect.h) * 0.25f);
	const gfx::Rect well{rect.x + in, rect.y + in, std::max(0.0f, rect.w - 2 * in),
						 std::max(0.0f, rect.h - 2 * in)};
	const float veil = state == FieldState::Normal ? 0.32f : 0.20f;
	batch.DrawRect(well, {0.0f, 0.0f, 0.0f, veil});
	if (state == FieldState::Active) {
		// A soft accent EDGE - two hairlines fading inward - where the flat look
		// drew a hard yellow border.
		Vec4 edge = ctx.GetTheme().accent;
		edge.w = 0.38f;
		DrawBorder(batch, well, edge);
		edge.w = 0.14f;
		DrawBorder(batch, {well.x + 1, well.y + 1, well.w - 2, well.h - 2}, edge);
	}
	return well;
}

void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color) {
	batch.DrawRect({rect.x, rect.y, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y, 1, rect.h}, color);
	batch.DrawRect({rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
}

void DrawGlow(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color,
			  float radius, float strength) {
	const int rings = std::clamp(static_cast<int>(std::lround(radius)), 1, 12);
	for (int i = 1; i <= rings; ++i) {
		const float t = 1.0f - (static_cast<float>(i) - 0.5f) / static_cast<float>(rings);
		const float o = static_cast<float>(i);
		DrawBorder(batch, {rect.x - o, rect.y - o, rect.w + 2 * o, rect.h + 2 * o},
				   {color.x, color.y, color.z, strength * t * t});
	}
}

void DrawCarvedGroove(gfx::SpriteBatch& batch, const gfx::Rect& rect, float width,
					  const Vec4& base) {
	const float w = std::min(width, std::min(rect.w, rect.h) * 0.25f);
	if (w < 2.0f) return;
	const float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h;
	// The floor: four strips, the corners belonging to the top and bottom ones.
	batch.DrawRect({x0, y0, rect.w, w}, base);
	batch.DrawRect({x0, y1 - w, rect.w, w}, base);
	batch.DrawRect({x0, y0 + w, w, rect.h - 2 * w}, base);
	batch.DrawRect({x1 - w, y0 + w, w, rect.h - 2 * w}, base);
	// The walls, as hairlines on the floor's two edges.
	const Vec4 shade{0.0f, 0.0f, 0.0f, 0.65f}, lit{1.0f, 1.0f, 1.0f, 0.24f};
	const gfx::Rect in{x0 + w, y0 + w, rect.w - 2 * w, rect.h - 2 * w};
	// Outer edge: shaded top + left (the walls facing away from the light).
	batch.DrawRect({x0, y0, rect.w, 1}, shade);
	batch.DrawRect({x0, y0, 1, rect.h}, shade);
	batch.DrawRect({x0, y1 - 1, rect.w, 1}, lit);
	batch.DrawRect({x1 - 1, y0, 1, rect.h}, lit);
	// Inner edge: lit top + left, shaded bottom + right - the far walls.
	batch.DrawRect({in.x - 1, in.y - 1, in.w + 2, 1}, lit);
	batch.DrawRect({in.x - 1, in.y - 1, 1, in.h + 2}, lit);
	batch.DrawRect({in.x - 1, in.y + in.h, in.w + 2, 1}, shade);
	batch.DrawRect({in.x + in.w, in.y - 1, 1, in.h + 2}, shade);
}

gfx::Rect PlaceTooltip(const gfx::Rect& anchor, float w, float h, const gfx::Rect& bounds,
					   TipSide prefer, float gap, float margin, TipAlign align) {
	const float left = bounds.x + margin, top = bounds.y + margin;
	const float right = bounds.x + bounds.w - margin, bottom = bounds.y + bounds.h - margin;
	float x = 0.0f, y = 0.0f;
	if (prefer == TipSide::Right) {
		const float besideR = anchor.x + anchor.w + gap, besideL = anchor.x - gap - w;
		const bool fitsR = besideR + w <= right, fitsL = besideL >= left;
		const bool roomierR = right - (anchor.x + anchor.w) >= anchor.x - left;
		x = (fitsR || (!fitsL && roomierR)) ? besideR : besideL;
		y = anchor.y + (anchor.h - h) * 0.5f;
	} else {
		const float below = anchor.y + anchor.h + gap, above = anchor.y - gap - h;
		const bool fitsB = below + h <= bottom, fitsA = above >= top;
		const bool roomierB = bottom - (anchor.y + anchor.h) >= anchor.y - top;
		const bool useBelow = prefer == TipSide::Below ? (fitsB || (!fitsA && roomierB))
													   : !(fitsA || (!fitsB && !roomierB));
		y = useBelow ? below : above;
		x = align == TipAlign::Start ? anchor.x
			: align == TipAlign::End ? anchor.x + anchor.w - w
									 : anchor.x + (anchor.w - w) * 0.5f;
	}
	// Whatever side it took, the whole tip stays on the surface.
	x = std::clamp(x, left, std::max(left, right - w));
	y = std::clamp(y, top, std::max(top, bottom - h));
	return {x, y, w, h};
}

std::string_view FitText(const Font& font, std::string_view text, float room,
						 bool* trimmed) {
	const bool cut = font.MeasureWidth(text) > room;
	if (trimmed) *trimmed = cut;
	if (!cut) return text;
	// Widths add (the font has no kerning), so one forward walk measuring a
	// character at a time finds the cut without re-measuring the prefix.
	const float budget = room - font.MeasureWidth(kTrimMark);
	float w = 0.0f;
	size_t keep = 0;
	for (size_t i = 0; i < text.size();) {
		size_t next = i + 1; // past the lead byte and any continuation bytes
		while (next < text.size() &&
			   (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80)
			++next;
		w += font.MeasureWidth(text.substr(i, next - i));
		if (w > budget) break;
		keep = i = next;
	}
	// "Warm.." rather than "Warm ..": a cut at a word gap reads as one word.
	while (keep > 0 && text[keep - 1] == ' ') --keep;
	return text.substr(0, keep);
}

void DrawFittedText(gfx::SpriteBatch& batch, const Font& font, std::string_view text,
					float x, float y, float room, const Vec4& color) {
	bool trimmed = false;
	const std::string_view fit = FitText(font, text, room, &trimmed);
	if (!trimmed) {
		font.Draw(batch, fit, x, y, color);
		return;
	}
	const float mark = font.MeasureWidth(kTrimMark);
	if (mark > room) return; // not even ".." fits: paint nothing rather than spill
	font.Draw(batch, fit, x, y, color);
	font.Draw(batch, kTrimMark, x + font.MeasureWidth(fit), y, color);
}

namespace {

// A small opaque box naming something in full, above `anchor` - or below it
// when that leaves the window - and clamped sideways. A Button's tooltip and a
// trimmed drop-down's face share it, so every tip reads the same.
void DrawTooltip(UIContext& ctx, gfx::SpriteBatch& batch, const Font& font,
				 std::string_view text, const gfx::Rect& anchor) {
	const Theme& theme = ctx.GetTheme();
	const float pad = font.Height() * 0.33f;
	const float w = font.MeasureWidth(text) + pad * 2.0f;
	const float h = font.Height() + pad;
	const gfx::Rect r = PlaceTooltip(anchor, w, h, {0, 0, ctx.Width(), ctx.Height()},
									 TipSide::Above, pad);
	batch.DrawRect(r, {theme.panel.x, theme.panel.y, theme.panel.z, 0.97f});
	DrawBorder(batch, r, theme.panelBorder);
	font.Draw(batch, text, r.x + pad, r.y + pad * 0.5f, theme.text);
}

} // namespace

void DrawSwatch(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Swatch& swatch) {
	if (swatch.icon)
		batch.DrawSprite(rect, {0, 0, 1, 1}, *swatch.icon, {1, 1, 1, 1});
	else if (swatch.color.w > 0.0f)
		batch.DrawRect(rect, swatch.color);
}

// --- Panel -------------------------------------------------------------

void Panel::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	DrawPanelFace(ctx, batch, Pixel(), opacity ? *opacity : 1.0f);
}

// --- Separator ---------------------------------------------------------

void Separator::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	batch.DrawRect({px.x, px.y + px.h * 0.5f, px.w, 1.0f}, ctx.GetTheme().panelBorder);
}

// --- Label -------------------------------------------------------------

void Label::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();
	const float y =
		centerV ? px.y + (px.h - TextFont().Height()) * 0.5f : px.y;
	TextFont().Draw(batch, text, px.x, y,
					accent ? theme.accent : dim ? theme.textDim : theme.text);
}

gfx::Rect Label::InkRect() const {
	// The LINE, not the row: a label drawn into a row shorter than its font (a
	// starved Stack Fill row gets zero) still paints the whole line. An empty
	// label paints nothing, so it claims only what the layout gave it.
	const gfx::Rect& px = Pixel();
	if (text.empty()) return px;
	const float h = TextFont().Height();
	const float y = centerV ? px.y + (px.h - h) * 0.5f : px.y;
	return {px.x, y, std::max(TextFont().MeasureWidth(text), px.w), h};
}

// --- TextOutput ----------------------------------------------------------

void TextOutput::Clear() {
	m_lines.clear();
	m_scroll = 0.0f;
}

void TextOutput::AddLine(std::string line) {
	m_lines.push_back(std::move(line));
	while (m_lines.size() > m_maxLines) m_lines.pop_front();
	m_scroll = 0.0f; // snap to latest
}

void TextOutput::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsWheelConsumed()) return;
	if (Pixel().Contains(input->MouseX(), input->MouseY()) && input->WheelDelta() != 0) {
		m_scroll += input->WheelDelta() * 3.0f;
		const float maxScroll =
			std::max(0.0f, static_cast<float>(m_lines.size()) -
							   Pixel().h / TextFont().LineAdvance());
		m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
		ctx.ConsumeWheel();
	}
}

void TextOutput::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	DrawPanelFace(ctx, batch, px);

	const float lineHeight = font.LineAdvance();
	const float pad = Rem(0.25f);
	const gfx::Rect inner{px.x + pad, px.y + pad, px.w - 2 * pad, px.h - 2 * pad};
	const ScopedClip clip(batch, inner);

	const int visibleLines = static_cast<int>(inner.h / lineHeight) + 1;
	// Index of the last line shown, offset by scroll (0 = newest).
	const int last = static_cast<int>(m_lines.size()) - 1 - static_cast<int>(m_scroll);
	float y = inner.y + inner.h - lineHeight;
	for (int i = last; i >= 0 && i > last - visibleLines; --i) {
		font.Draw(batch, m_lines[static_cast<size_t>(i)], inner.x, y, theme.text);
		y -= lineHeight;
	}
}

// --- Button --------------------------------------------------------------

void Button::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const Clock::time_point now = Clock::now();
	const auto since = [&](Clock::time_point t) {
		return std::chrono::duration<float>(now - t).count();
	};

	// The push in flight: the action fires at the bottom of the sink, the rise
	// follows it.
	if (m_push == Push::Sinking && since(m_pressAt) >= kSinkSeconds + kHoldSeconds) {
		m_push = Push::Rising;
		m_riseAt = now;
		if (enabled && onClick && !m_fired) onClick();
		m_fired = false;
	} else if (m_push == Push::Rising && since(m_riseAt) >= kRiseSeconds) {
		m_push = Push::None;
	}

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (enabled && input->WasMousePressed(MouseButton::Left)) {
			// A click hard on the heels of the last one: that one's action runs
			// NOW, so a quick double click is still two actions, in order.
			if (m_push == Push::Sinking && onClick && !m_fired) onClick();
			m_push = Push::None;
			m_held = true;
			m_pressAt = now;
			m_fired = false;
			if (fireOnPress) { // acts now; the push plays on regardless
				m_fired = true;
				if (onClick) onClick();
			}
		}
		ctx.ConsumeMouse();
	}
	if (!enabled) { // disabled mid-press: the release does nothing
		m_held = false;
		if (m_push == Push::Sinking) m_push = Push::None;
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		m_held = false;
		// Released ON the button: the push completes (fires once the face has
		// reached the bottom). Released off it: a cancelled press just rises -
		// unless it already acted on the press, which is not taken back.
		if (m_hot || m_fired) {
			m_push = Push::Sinking;
		} else {
			m_push = Push::Rising;
			m_riseAt = now - std::chrono::duration_cast<Clock::duration>(
								 std::chrono::duration<float>(
									 kRiseSeconds * (1.0f - std::min(1.0f, since(m_pressAt) /
																		  kSinkSeconds))));
		}
	}
}

void Button::PressVisual() {
	if (m_held) return;
	m_push = Push::Sinking;
	m_pressAt = Clock::now();
	m_fired = true; // nothing to fire at the bottom: the key already acted
}

float Button::Depth() const {
	const Clock::time_point now = Clock::now();
	const auto since = [&](Clock::time_point t) {
		return std::chrono::duration<float>(now - t).count();
	};
	if (m_held || m_push == Push::Sinking) return std::min(1.0f, since(m_pressAt) / kSinkSeconds);
	if (m_push == Push::Rising) return std::max(0.0f, 1.0f - since(m_riseAt) / kRiseSeconds);
	return 0.0f;
}

void Button::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	if (const Skin* skin = ctx.GetSkin(); etch && skin && skin->block.texture) {
		// A cut-stone block: an active one (a current tab) is held down AND
		// shows its gold lit; disabled dims the stone, edges kept. The gold is
		// the material's (EtchInk), like a carved word's.
		const float dim = enabled ? 1.0f : 0.45f;
		const bool lit = active && etchLit;
		DrawCutStone(batch, px, *skin, lit ? etchLit : etch, active ? 1.0f : Depth(),
					 m_hot && enabled, {dim, dim, dim, 1.0f}, lit);
		return;
	}
	if (const Skin* skin = ctx.GetSkin(); carved && skin && skin->block.texture) {
		const float depth = Depth();
		const float dim = enabled ? 1.0f : 0.45f;
		DrawCutStone(batch, px, *skin, nullptr, depth, m_hot && enabled, {dim, dim, dim, 1.0f});
		const Font& font = TextFont();
		const float sink = depth * std::max(1.0f, px.h * 0.035f);
		DrawCarvedText(batch, font, text, px.x + (px.w - font.MeasureWidth(text)) * 0.5f + sink,
					   px.y + (px.h - font.Height()) * 0.5f + sink,
					   !enabled ? CarvedDisabled(skin)
					   : m_hot  ? CarvedLit(skin)
								: CarvedGold(skin));
		return;
	}
	if (icon) {
		// Icon-only: the round face IS the button (it carries its own chrome
		// and alpha) — no button face behind it. Rotated in quarter turns
		// (screen Y is down: positive turns step right→down→left→up), with
		// hover/held brightening standing in for the face wash.
		// Disabled dims the disc well down (the editor toolbar's 0.32), since
		// an icon has no face to flatten.
		// A push shrinks the disc a touch, as though pressed into the panel.
		const float depth = Depth();
		const float d = std::min(px.w, px.h) * (0.92f - 0.07f * depth);
		const float f = !enabled							? 0.32f
						: (depth > 0.0f || active)		? 1.15f
						: m_hot							? 1.0f
														: 0.82f;
		batch.DrawSpriteRotated({px.x + px.w * 0.5f, px.y + px.h * 0.5f}, {d, d},
								static_cast<float>(iconTurns) * (kPi * 0.5f),
								{0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		return;
	}
	// The face shows PUSHED for the deeper half of the motion, and the label
	// follows the depth down by up to a sixteenth of a line - so the sink and
	// the rise are both seen, not just a flip between two faces.
	const float depth = Depth();
	const float sink = depth * std::max(1.0f, TextFont().Height() * 0.08f);
	static const std::string kNoLabel;
	DrawButtonFace(batch, TextFont(), px, faceIcon ? kNoLabel : text, ctx.GetTheme(),
				   m_hot && enabled, depth >= 0.5f || active, enabled, ctx.GetSkin(), sink);
	if (faceIcon) {
		// The icon in the label's place, in the label's colour, sinking with it.
		// A face glyph is authored in the middle of a disc-sized canvas (the
		// toolbar discs' 84 px space, strokes inside its central half), so only
		// that half is drawn, filling most of the face's height.
		const Theme& theme = ctx.GetTheme();
		const float d = px.h * 0.82f;
		const Vec4 ink = enabled ? theme.text : theme.textDim;
		batch.DrawSprite({px.x + (px.w - d) * 0.5f, px.y + (px.h - d) * 0.5f + sink, d, d},
						 {0.25f, 0.25f, 0.5f, 0.5f}, *faceIcon, ink);
	}
}

void Button::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_hot || tooltip.empty()) return;
	// The editor toolbar's tip, as a control: the button's own font, a pad of
	// a third of a line, the panel colour made opaque (a tip over busy content
	// must read) and the frame's border. Above the button - dialog footers sit
	// at the bottom, where below would run off the card - unless that leaves
	// the window, and clamped to it sideways.
	DrawTooltip(ctx, batch, TextFont(), tooltip, Pixel());
}

gfx::Rect Button::InkRect() const {
	// Mirrors DrawButtonFace: the face fills the bounds, the label is centred on
	// them at its measured size. An icon face is drawn inside the bounds.
	const gfx::Rect& px = Pixel();
	if (icon || faceIcon || etch || text.empty()) return px;
	const Font& font = TextFont();
	const float w = font.MeasureWidth(text);
	const float h = font.Height();
	return {std::min(px.x, px.x + (px.w - w) * 0.5f),
			std::min(px.y, px.y + (px.h - h) * 0.5f), std::max(px.w, w),
			std::max(px.h, h)};
}

void DrawButtonFace(gfx::SpriteBatch& batch, const Font& font,
					const gfx::Rect& rect,
					const std::string& label, const Theme& theme, bool hot,
					bool held, bool enabled, const Skin* skin, float sink) {
	if (skin && skin->button.texture) {
		// Disabled dims the stone (the bevel keeps its edges); held sinks the
		// bevel. Hot and held also wash the theme's control colour over the face,
		// lightly - the stone and bevel carry the look, the theme the state.
		const float dim = enabled ? 1.0f : 0.45f;
		const bool down = enabled && held;
		DrawFace(batch, rect, *skin, down ? Face::ButtonDown : Face::Button,
				 {dim, dim, dim, 1.0f});
		if (enabled && (held || hot)) {
			Vec4 wash = held ? theme.controlActive : theme.controlHot;
			wash.w = 0.22f;
			const float in = FaceInset(*skin, down ? Face::ButtonDown : Face::Button);
			batch.DrawRect({rect.x + in, rect.y + in, rect.w - 2 * in, rect.h - 2 * in}, wash);
		}
	} else {
		const Vec4& fill = !enabled ? theme.panel
						   : held   ? theme.controlActive
						   : hot    ? theme.controlHot
									: theme.control;
		batch.DrawRect(rect, fill);
		DrawBorder(batch, rect, theme.panelBorder);
	}
	const float textW = font.MeasureWidth(label);
	font.Draw(batch, label, rect.x + (rect.w - textW) * 0.5f,
			  rect.y + (rect.h - font.Height()) * 0.5f + sink,
			  enabled ? theme.text : theme.textDim);
}

// --- Checkbox ------------------------------------------------------------

void Checkbox::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) {
			m_checked = !m_checked;
			if (onChange) onChange(m_checked);
		}
	}
}

void Checkbox::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const bool skinned = ctx.GetSkin() != nullptr;
	if (skinned) {
		// On stone the row states are light, not paint: the old brown fills
		// read as mud over the slab.
		if (highlight) {
			batch.DrawRect(px, {theme.accent.x, theme.accent.y, theme.accent.z, 0.12f});
			DrawBorder(batch, px, {theme.accent.x, theme.accent.y, theme.accent.z, 0.40f});
		} else if (m_hot) {
			batch.DrawRect(px, {1.0f, 1.0f, 1.0f, 0.06f});
		}
	} else if (highlight) {
		batch.DrawRect(px, theme.controlActive);
		DrawBorder(batch, px, theme.panelBorder);
	} else if (m_hot) {
		batch.DrawRect(px, theme.controlHot);
	}
	// The check box itself, left-aligned and vertically centered.
	const float box = BoxSide(px);
	const gfx::Rect b{px.x + Rem(0.15f), px.y + (px.h - box) * 0.5f, box, box};
	DrawFieldFace(ctx, batch, b, m_hot ? FieldState::Hot : FieldState::Normal, theme.control,
				  theme.panelBorder);
	if (m_checked) {
		const float in = box * (skinned ? 0.32f : 0.24f);
		const gfx::Rect tick{b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in};
		if (skinned) {
			// A lit mark in the well: a soft halo, then the tick.
			const float g = in * 0.3f;
			batch.DrawRect({tick.x - g, tick.y - g, tick.w + 2 * g, tick.h + 2 * g},
						   {theme.accent.x, theme.accent.y, theme.accent.z, 0.30f});
		}
		batch.DrawRect(tick, theme.accent);
	}
	// The swatch: square, the row's height less a hairline inset, after the box -
	// asked for now, at the moment it is drawn (see the header).
	if (swatch) {
		const float side = px.h - 2.0f;
		DrawSwatch(batch, {b.x + b.w + Rem(0.3f), px.y + 1.0f, side, side}, swatch());
	}
	font.Draw(batch, label, TextX(px), px.y + (px.h - font.Height()) * 0.5f,
			  (m_checked || highlight) ? theme.text : theme.textDim);
}

float Checkbox::TextX(const gfx::Rect& px) const {
	const float box = BoxSide(px);
	float x = px.x + Rem(0.15f) + box + Rem(0.3f);
	if (swatch) x += (px.h - 2.0f) + Rem(0.3f);
	return x;
}

gfx::Rect Checkbox::InkRect() const {
	// Mirrors DrawSelf's geometry: the box (and swatch) are bounded, the label
	// runs on past the bounds if it was not given the room. Vertically the label
	// is centred like a Button's, so a row shorter than the font spills the line
	// out of both edges.
	const gfx::Rect& px = Pixel();
	const float right =
		std::max(px.x + px.w, TextX(px) + TextFont().MeasureWidth(label));
	const float h = label.empty() ? px.h : std::max(px.h, TextFont().Height());
	return {px.x, px.y + (px.h - h) * 0.5f, right - px.x, h};
}

// --- Slider --------------------------------------------------------------

void Slider::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const bool hovered =
		!ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (hovered && input->WasMousePressed(MouseButton::Left)) m_dragging = true;
	if (m_dragging && !input->IsMouseDown(MouseButton::Left)) {
		m_dragging = false;
		if (onRelease) onRelease();
	}
	if (hovered || m_dragging) ctx.ConsumeMouse();

	if (m_dragging) {
		const float t = std::clamp(
			(input->MouseX() - Pixel().x) / std::max(Pixel().w, 1.0f), 0.0f, 1.0f);
		const float value = m_min + t * (m_max - m_min);
		if (value != m_value) {
			m_value = value;
			RefreshDisplay();
			if (onChange) onChange(m_value);
		}
	}
}

void Slider::RefreshDisplay() {
	m_display = std::format("{}: {:.{}f}", label, m_value, m_decimals);
}

void Slider::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	// The whole control lives INSIDE its bounds: the label on the top line, the
	// track + thumb in the band beneath it. (Previously the label was drawn above
	// the bounds, so callers couldn't space sliders by their box — it collided
	// with whatever sat above.) Bounds should be tall enough for both.
	font.Draw(batch, m_display, px.x, px.y, theme.textDim);
	const float bandY = px.y + font.LineAdvance();
	const float bandH = std::max(px.h - font.LineAdvance(), Rem(0.3f));

	const float t = (m_value - m_min) / std::max(m_max - m_min, 1e-6f);
	const float thumbW = Rem(0.7f);
	const Skin* skin = ctx.GetSkin();
	if (skin && skin->button.texture) {
		// Skinned: the track is a sunken GROOVE (the field face, so it reads with
		// the drop-downs and text boxes) holding a lit accent strip, and the thumb
		// a raised stone button - the thing you grab stands proud of the thing it
		// slides in.
		const float grooveH = std::min(Rem(0.45f), bandH);
		const float grooveY = bandY + (bandH - grooveH) * 0.5f;
		const gfx::Rect groove{px.x, grooveY, px.w, grooveH};
		const gfx::Rect well = DrawFieldFace(ctx, batch, groove,
											 m_dragging ? FieldState::Active : FieldState::Normal,
											 theme.control, theme.panelBorder);
		batch.DrawRect({well.x, well.y, well.w * t, well.h},
					   {theme.accent.x, theme.accent.y, theme.accent.z, 0.85f});
		const float thumbH = std::max(bandH * 0.9f, grooveH + 4.0f);
		const float stoneW = Rem(0.9f);
		const gfx::Rect thumb{px.x + px.w * t - stoneW * 0.5f,
							  grooveY + grooveH * 0.5f - thumbH * 0.5f, stoneW, thumbH};
		DrawFace(batch, thumb, *skin, m_dragging ? Face::ButtonDown : Face::Button,
				 {1.0f, 1.0f, 1.0f, 1.0f});
		return;
	}

	// Track.
	const float trackH = Rem(0.15f);
	const float trackY = bandY + (bandH - trackH) * 0.5f;
	batch.DrawRect({px.x, trackY, px.w, trackH}, theme.control);

	// Filled portion + thumb.
	batch.DrawRect({px.x, trackY, px.w * t, trackH}, theme.accent);
	// A stubby block centred on the track: half the band tall, 0.7rem wide (it
	// was the full band and 0.35rem, and read as a thin post). Drawing only -
	// the whole bounds take the drag.
	const float thumbH = bandH * 0.5f;
	const gfx::Rect thumb{px.x + px.w * t - thumbW * 0.5f,
						  trackY + trackH * 0.5f - thumbH * 0.5f, thumbW, thumbH};
	batch.DrawRect(thumb, m_dragging ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, thumb, theme.panelBorder);
}

// --- DropDown ------------------------------------------------------------

// The list opens below the control; when it doesn't fit there and the space
// above is larger, it flips. Whatever is still too tall scrolls — a pool-length
// list used to draw straight off the bottom of the window.
float DropDown::RowH() const {
	return icons.empty() ? Pixel().h : Pixel().h * iconRowScale;
}

float DropDown::IconSide(float rowH) const { return std::max(0.0f, rowH - Rem(0.24f)); }

float DropDown::IconLead(float rowH) const {
	return icons.empty() ? 0.0f : IconSide(rowH) + Rem(0.35f);
}

const gfx::Texture* DropDown::IconAt(int index) const {
	return index >= 0 && index < static_cast<int>(icons.size())
			   ? icons[static_cast<size_t>(index)]
			   : nullptr;
}

// The picture square at the row's left, inset like the text; a hairline round
// it so a dark picture still reads as a tile on a dark field.
void DropDown::DrawIcon(gfx::SpriteBatch& batch, const Theme& theme, int index,
						const gfx::Rect& row) const {
	const gfx::Texture* icon = IconAt(index);
	if (!icon) return;
	const float side = IconSide(row.h);
	const gfx::Rect r{row.x + Rem(0.4f), row.y + (row.h - side) * 0.5f, side, side};
	batch.DrawSprite(r, {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	DrawBorder(batch, r, theme.panelBorder);
}

bool DropDown::Passes(size_t item) const {
	if (filterLabels.empty() || item >= itemFilters.size()) return true;
	return ((itemFilters[item] >> static_cast<unsigned>(m_filter)) & 1u) != 0;
}

size_t DropDown::ShownCount() const {
	if (filterLabels.empty()) return items.size();
	size_t n = 0;
	for (size_t i = 0; i < items.size(); ++i)
		if (Passes(i)) ++n;
	return n;
}

void DropDown::LayoutSelf(UIContext& ctx) {
	// The category buttons' caption size. Em is this control's own text size
	// (fontScale included), so the buttons track whatever the list is drawn at.
	m_chipFont = filterLabels.empty()
					 ? nullptr
					 : &ctx.FontAt(ResolvedRole(), std::max(8.0f, Em(filterScale)));
}

// A button's colour chip: a square the caption's cap height, before its text.
static float ChipSwatch(const Font& font) { return font.Height() * 0.62f; }

bool DropDown::HasChip(size_t i) const {
	return i < filterColors.size() && filterColors[i].w > 0.0f;
}

// One category button's width: its caption, its colour chip and their gap, and
// the padding either side. The draw lays out against exactly these terms.
float DropDown::ChipWidth(size_t i) const {
	const Font& font = ChipFont();
	return font.MeasureWidth(filterLabels[i]) + Rem(0.4f) +
		   (HasChip(i) ? ChipSwatch(font) + Rem(0.2f) : 0.0f);
}

template <class F>
float DropDown::ForEachChip(const gfx::Rect& popup, F&& f) const {
	if (filterLabels.empty()) return 0.0f;
	const Font& font = ChipFont();
	const float pad = Rem(0.25f), gap = Rem(0.2f), h = font.LineAdvance() + Rem(0.15f);
	float x = popup.x + pad, y = popup.y + pad;
	for (size_t i = 0; i < filterLabels.size(); ++i) {
		const float w = ChipWidth(i);
		// Wrap to a new line of buttons, unless this is the first on its line
		// (a button wider than the list is trimmed, never stranded alone).
		if (x > popup.x + pad && x + w > popup.x + popup.w - pad) {
			x = popup.x + pad;
			y += h + gap;
		}
		f(i, gfx::Rect{x, y, std::min(w, popup.w - 2.0f * pad), h});
		x += w + gap;
	}
	return y + h + pad - popup.y;
}

gfx::Rect DropDown::ListRect(const gfx::Rect& popup) const {
	const float band = ForEachChip(popup, [](size_t, const gfx::Rect&) {});
	return {popup.x, popup.y + band, popup.w, std::max(0.0f, popup.h - band)};
}

gfx::Rect DropDown::PopupRect(const UIContext& ctx) const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(0.15f);
	// Wide enough for the longest item - the face may be trimming it - with the
	// face's own text inset on both sides and the scrollbar's gutter when the
	// list scrolls; never past the window, so it shifts left before it widens
	// off the edge. A row that still does not fit is trimmed like the face.
	// The WIDTH comes first because the category buttons wrap to it, and how
	// many lines of them there are is part of the height.
	float want = m_popupTextW + Rem(0.4f) * 2.0f + IconLead(RowH());
	// Wide enough that the category buttons wrap onto at most TWO lines: half
	// their run, plus the widest one so the greedy wrap cannot spill a third.
	if (!filterLabels.empty()) {
		float run = 0.0f, widest = 0.0f;
		for (size_t i = 0; i < filterLabels.size(); ++i) {
			run += ChipWidth(i) + Rem(0.2f);
			widest = std::max(widest, ChipWidth(i));
		}
		want = std::max(want, run * 0.5f + widest + Rem(0.5f));
	}
	const float w0 = std::min(std::max(px.w, want), ctx.Width());
	const float band = ForEachChip({0, 0, w0, 0}, [](size_t, const gfx::Rect&) {});
	const float content = band + RowH() * static_cast<float>(ShownCount());
	const float below = std::max(0.0f, ctx.Height() - (px.y + px.h) - pad);
	const float above = std::max(0.0f, px.y - pad);
	gfx::Rect r;
	if (content <= below || below >= above) {
		r = {px.x, px.y + px.h, w0, std::min(content, below)};
	} else {
		const float h = std::min(content, above);
		r = {px.x, px.y - h, w0, h};
	}
	const float gutter = content > r.h ? Rem(0.45f) : 0.0f;
	r.w = std::min(std::max(px.w, want + gutter), ctx.Width());
	r.x = std::clamp(px.x, 0.0f, std::max(0.0f, ctx.Width() - r.w));
	return r;
}

float DropDown::MaxScroll(const gfx::Rect& popup) const {
	return std::max(0.0f,
					RowH() * static_cast<float>(ShownCount()) - ListRect(popup).h);
}

gfx::Rect DropDown::ItemRect(const gfx::Rect& popup, size_t slot) const {
	const float rowH = RowH();
	const gfx::Rect list = ListRect(popup);
	// Rows stop short of the scrollbar gutter, so a row hover never sits under
	// the thumb (SlotList's rule).
	const float gutter = MaxScroll(popup) > 0.0f ? Rem(0.45f) : 0.0f;
	return {list.x, list.y + rowH * static_cast<float>(slot) - m_scroll, list.w - gutter,
			rowH};
}

gfx::Rect DropDown::ScrollTrackRect(const gfx::Rect& popup) const {
	const float barW = Rem(0.35f);
	const gfx::Rect list = ListRect(popup);
	return {list.x + list.w - barW - 1.0f, list.y + 1.0f, barW, list.h - 2.0f};
}

gfx::Rect DropDown::ScrollThumbRect(const gfx::Rect& popup, float maxScroll) const {
	const gfx::Rect track = ScrollTrackRect(popup);
	const float listH = ListRect(popup).h;
	const float thumbH =
		std::max(track.h * listH / (listH + maxScroll), Rem(0.9f));
	const float t = maxScroll > 0.0f ? m_scroll / maxScroll : 0.0f;
	return {track.x, track.y + (track.h - thumbH) * t, track.w, thumbH};
}

void DropDown::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();

	// Esc closes an open list and picks nothing - the colour picker's rule. The
	// page beneath sees the popup was open (UIContext::PopupOpen) and does not
	// also take the Esc as "back".
	if (m_open && input->WasKeyPressed(vk::Escape)) {
		m_open = false;
		m_scrollDragging = false;
		ctx.ConsumeMouse();
		return;
	}
	if (m_open) {
		// The open popup owns the mouse entirely — including the wheel, which a
		// modal claims whether or not it scrolls: a list open over a page must
		// not let the page scroll out from under it. ClaimPopup extends that to
		// the controls updated BEFORE this one (added after it), which would
		// otherwise see the click on a row first.
		ctx.ClaimPopup();
		ctx.ConsumeWheel();
		const gfx::Rect popup = PopupRect(ctx);

		// The category buttons sit above the rows and do not scroll. A press
		// switches the filter - the list closes up to what passes and goes back
		// to its top - and never picks a row or closes the list.
		m_hoverChip = -1;
		bool chipPressed = false;
		ForEachChip(popup, [&](size_t i, const gfx::Rect& r) {
			if (!r.Contains(mx, my)) return;
			m_hoverChip = static_cast<int>(i);
			chipPressed = input->WasMousePressed(MouseButton::Left);
		});
		if (m_hoverChip >= 0) {
			if (chipPressed) {
				m_filter = m_hoverChip;
				m_scroll = 0.0f;
			}
			m_hoverItem = -1;
			ctx.ConsumeMouse();
			return;
		}

		const float maxScroll = MaxScroll(popup);
		m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);

		// The scrollbar runs before the rows: a press on the thumb must neither
		// pick the row behind it nor read as the click-outside that closes.
		m_scrollHot = false;
		if (maxScroll > 0.0f) {
			const gfx::Rect track = ScrollTrackRect(popup);
			const gfx::Rect thumb = ScrollThumbRect(popup, maxScroll);
			if (m_scrollDragging && !input->IsMouseDown(MouseButton::Left))
				m_scrollDragging = false;
			m_scrollHot = thumb.Contains(mx, my);
			if (m_scrollHot && input->WasMousePressed(MouseButton::Left)) {
				m_scrollDragging = true;
				m_scrollGrab = my - thumb.y;
			}
			if (m_scrollDragging) {
				const float range = track.h - thumb.h;
				if (range > 0.0f)
					m_scroll = std::clamp(
						(my - m_scrollGrab - track.y) / range * maxScroll, 0.0f,
						maxScroll);
			}
			if (input->WheelDelta() != 0.0f && popup.Contains(mx, my))
				m_scroll = std::clamp(m_scroll - input->WheelDelta() * RowH(),
									  0.0f, maxScroll);
			if (m_scrollHot || m_scrollDragging || track.Contains(mx, my)) {
				ctx.ConsumeMouse();
				return;
			}
		} else {
			m_scrollDragging = false;
		}

		m_hoverItem = -1;
		// Inside the LIST area only: a part-scrolled row reaches past the box,
		// and under the category buttons there is no row to pick.
		if (ListRect(popup).Contains(mx, my)) {
			size_t slot = 0;
			for (size_t i = 0; i < items.size(); ++i) {
				if (!Passes(i)) continue;
				if (!ItemRect(popup, slot++).Contains(mx, my)) continue;
				m_hoverItem = static_cast<int>(i);
				if (input->WasMousePressed(MouseButton::Left)) {
					m_selected = static_cast<int>(i);
					m_open = false;
					ctx.ConsumeMouse();
					if (onSelect) onSelect(m_selected);
					return;
				}
			}
		}
		// A click OUTSIDE the list closes it; one on the button band's gaps or
		// below the last row does nothing.
		if (input->WasMousePressed(MouseButton::Left) && !popup.Contains(mx, my))
			m_open = false;
		ctx.ConsumeMouse();
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) OpenPopup(ctx);
	}
}

void DropDown::OpenPopup(UIContext& ctx) {
	m_open = true;
	ctx.ClaimPopup();
	m_popupTextW = 0.0f;
	for (const std::string& item : items)
		m_popupTextW = std::max(m_popupTextW, TextFont().MeasureWidth(item));
	// Open with the current selection in view - a long list otherwise opens at
	// the top, nowhere near what it says it is showing. The selection's SLOT
	// among the rows the filter shows (it may be hidden, and then the list
	// opens at its top).
	size_t slot = 0;
	for (int i = 0; i < m_selected && i < static_cast<int>(items.size()); ++i)
		if (Passes(static_cast<size_t>(i))) ++slot;
	if (m_selected >= 0 && !Passes(static_cast<size_t>(m_selected))) slot = 0;
	const gfx::Rect popup = PopupRect(ctx);
	const float rowH = RowH();
	m_scroll = std::clamp(rowH * static_cast<float>(slot) -
							  (ListRect(popup).h - rowH) * 0.5f,
						  0.0f, MaxScroll(popup));
}

void DropDown::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	DrawFieldFace(ctx, batch, px,
				  m_open ? FieldState::Active : (m_hot ? FieldState::Hot : FieldState::Normal),
				  m_hot || m_open ? theme.controlHot : theme.control, theme.panelBorder);
	DrawIcon(batch, theme, m_selected, px);
	const float textY = px.y + (px.h - font.Height()) * 0.5f;
	DrawFittedText(batch, font, Current(), TextX(), textY, TextRoom(), theme.text);
	DrawDropDownExpander(batch, font, px, theme, m_open, m_hot);
}

const std::string& DropDown::Current() const {
	// The empty case is a named string, NOT a "" literal: a ternary mixing
	// std::string with const char* has common type std::string, so binding the
	// reference COPIED the selected item — a heap allocation per dropdown per
	// frame, in a draw path (found by the steady-state allocation guard).
	static const std::string kNoSelection;
	return (m_selected >= 0 && m_selected < static_cast<int>(items.size()))
			   ? items[static_cast<size_t>(m_selected)]
			   : kNoSelection;
}

float DropDown::TextX() const { return Pixel().x + Rem(0.4f) + IconLead(Pixel().h); }

float DropDown::TextRoom() const {
	return std::max(0.0f, DropDownTextRight(TextFont(), Pixel()) - TextX());
}

gfx::Rect DropDown::InkRect() const {
	const gfx::Rect& px = Pixel();
	if (Current().empty()) return px;
	const float h = std::max(px.h, TextFont().Height());
	return {px.x, px.y + (px.h - h) * 0.5f, px.w, h};
}

float DropDown::TextOverrun() const {
	return std::max(0.0f, TextFont().MeasureWidth(Current()) - TextRoom());
}

void DrawDropDownExpander(gfx::SpriteBatch& batch, const Font& font,
						  const gfx::Rect& rect, const Theme& theme, bool open,
						  bool hot) {
	// The authored box is a SQUARE sized off the control's height and inset so
	// it clears the border. Open swaps to the up-triangle twin rather than
	// turning the box, which would carry its top-lit rim round to the bottom;
	// without that asset the closed one is turned, as before.
	// Brightness carries the hover/open read the flat glyph used to get from
	// the accent color (the same idiom Button's icon path uses).
	//
	// The inset is a fraction of the TEXT height, not of the rect: the box has
	// to clear a 1px border beside type of whatever size, and a rect fraction
	// would grow the gap on a tall control and lose it on a short one.
	const float inset = font.Height() * 0.12f;
	const ControlIcons& icons = GetControlIcons();
	if (const gfx::Texture* icon = icons.dropDown) {
		const bool swap = open && icons.dropDownOpen;
		if (swap) icon = icons.dropDownOpen;
		const float d = rect.h - inset * 2.0f;
		const float f = (hot || open) ? 1.15f : 0.9f;
		batch.DrawSpriteRotated(
			{rect.x + rect.w - inset - d * 0.5f, rect.y + rect.h * 0.5f}, {d, d},
			open && !swap ? kPi : 0.0f, {0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		return;
	}
	// Fallback with no icon installed: the text arrow, right-aligned with a
	// margin so it clears the border at any font size (measure it rather than
	// assume a fixed width).
	const char* arrow = open ? "^" : "v";
	font.Draw(batch, arrow, rect.x + rect.w - font.MeasureWidth(arrow) - inset * 3.0f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.accent);
}

float DropDownTextRight(const Font& font, const gfx::Rect& rect) {
	// DrawDropDownExpander's geometry, then the same inset again as a gap, so
	// text that runs to the limit still reads as separate from the box.
	const float inset = font.Height() * 0.12f;
	const float left = GetControlIcons().dropDown
						   ? rect.x + rect.w - inset - (rect.h - inset * 2.0f)
						   : rect.x + rect.w - font.MeasureWidth("v") - inset * 3.0f;
	return left - inset;
}

void DropDown::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) {
		// A trimmed face says itself in full while hovered, as a palette row does.
		if (m_hot && TextOverrun() > 0.0f) DrawTooltip(ctx, batch, TextFont(), Current(), Pixel());
		return;
	}
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect popup = PopupRect(ctx);
	const gfx::Rect list = ListRect(popup);
	const float maxScroll = MaxScroll(popup);
	const bool skinned = ctx.GetSkin() != nullptr;

	// The list's backing. A category band needs one even unskinned and
	// unscrolled, since its buttons do not cover it.
	if (skinned) {
		// The open list is one sunken field, opaque so the page under it
		// cannot show through the veil; rows are light on it, not boxes.
		batch.DrawRect(popup, {0.05f, 0.045f, 0.04f, 1.0f});
		DrawFieldFace(ctx, batch, popup, FieldState::Active, theme.control,
					  theme.panelBorder);
	} else if (maxScroll > 0.0f || !filterLabels.empty()) {
		batch.DrawRect(popup, theme.control); // backing behind the part-rows
	}

	// The category buttons: the one in force lit in the accent, the hovered one
	// brightened, each label trimmed to its button.
	ForEachChip(popup, [&](size_t i, const gfx::Rect& r) {
		const bool on = static_cast<int>(i) == m_filter;
		const bool hot = static_cast<int>(i) == m_hoverChip;
		if (skinned) {
			batch.DrawRect(r, on	? Vec4{theme.accent.x, theme.accent.y, theme.accent.z, 0.22f}
							  : hot ? Vec4{1.0f, 1.0f, 1.0f, 0.08f}
									: Vec4{0.0f, 0.0f, 0.0f, 0.25f});
		} else {
			batch.DrawRect(r, on ? theme.controlActive : hot ? theme.controlHot : theme.control);
		}
		DrawBorder(batch, r, on ? theme.accent : theme.panelBorder);
		// Colour chip, then the caption, the pair centred on the button.
		const Font& cf = ChipFont();
		const bool chip = HasChip(i);
		const float sw = chip ? ChipSwatch(cf) : 0.0f, sgap = chip ? Rem(0.2f) : 0.0f;
		// The room is the very sum the width was built from, so a pixel of
		// slack stops float round-off trimming a caption that fits.
		const std::string_view fit =
			FitText(cf, filterLabels[i], r.w - Rem(0.4f) - sw - sgap + 1.0f);
		float x = r.x + (r.w - (sw + sgap + cf.MeasureWidth(fit))) * 0.5f;
		if (chip) {
			const gfx::Rect s{x, r.y + (r.h - sw) * 0.5f, sw, sw};
			batch.DrawRect(s, filterColors[i]);
			DrawBorder(batch, s, {0.0f, 0.0f, 0.0f, 0.6f});
			x += sw + sgap;
		}
		cf.Draw(batch, fit, x, r.y + (r.h - cf.Height()) * 0.5f, on ? theme.accent : theme.text);
	});

	{
		// Scoped so the clip lifts before the scrollbar draws beside the list.
		// Rows scroll UNDER the category band, never over it.
		std::optional<ScopedClip> clip;
		if (maxScroll > 0.0f || !filterLabels.empty()) clip.emplace(batch, list);
		size_t slot = 0;
		for (size_t i = 0; i < items.size(); ++i) {
			if (!Passes(i)) continue;
			const gfx::Rect rect = ItemRect(popup, slot++);
			if (rect.y + rect.h <= list.y || rect.y >= list.y + list.h) continue;
			const bool hovered = static_cast<int>(i) == m_hoverItem;
			if (skinned) {
				if (hovered)
					batch.DrawRect(rect, {theme.accent.x, theme.accent.y, theme.accent.z, 0.18f});
			} else {
				batch.DrawRect(rect, hovered ? theme.controlHot : theme.control);
				DrawBorder(batch, rect, theme.panelBorder);
			}
			DrawIcon(batch, theme, static_cast<int>(i), rect);
			const float inset = Rem(0.4f), lead = inset + IconLead(rect.h);
			DrawFittedText(batch, font, items[i], rect.x + lead,
						   rect.y + (rect.h - font.Height()) * 0.5f,
						   rect.w - lead - inset,
						   static_cast<int>(i) == m_selected ? theme.accent : theme.text);
		}
	}
	if (maxScroll > 0.0f) {
		batch.DrawRect(ScrollTrackRect(popup), theme.control);
		const gfx::Rect thumb = ScrollThumbRect(popup, maxScroll);
		batch.DrawRect(thumb, m_scrollDragging || m_scrollHot ? theme.controlActive
															  : theme.controlHot);
		DrawBorder(batch, thumb, theme.panelBorder);
	}
}

// --- ContextMenu -------------------------------------------------------------

namespace {

// A row's insets in REM (code-review C382), which the box is sized from AND the
// text drawn at: the label starts kMenuLabelInset in from the row's left, the
// text ends kMenuRightInset short of its right, and a group's marker keeps
// kMenuMarkerGap from the label before it. Together they are the old 0.85 rem of
// padding, so a menu without a group keeps its width.
constexpr float kMenuLabelInset = 0.6f;
constexpr float kMenuRightInset = 0.25f;
constexpr float kMenuMarkerGap = 0.35f;
constexpr float kMenuMinW = 2.85f; // a box's narrowest, label or not
constexpr std::string_view kMenuMarker = "»";

} // namespace

float ContextMenu::LabelX(const gfx::Rect& row) const {
	return row.x + Rem(kMenuLabelInset);
}

float ContextMenu::MarkerX(const gfx::Rect& row) const {
	return row.x + row.w - Rem(kMenuRightInset) - TextFont().MeasureWidth(kMenuMarker);
}

float ContextMenu::RowOverrun(const Row& row, const gfx::Rect& rect) const {
	// The text's room ends at the row's right inset - or at the window's edge,
	// for a menu wider than the window (clamped to its left edge, it runs off
	// the right).
	const float right = std::min(rect.x + rect.w, m_screenW) - Rem(kMenuRightInset);
	const float labelEnd = LabelX(rect) + TextFont().MeasureWidth(row.Label());
	if (!row.isGroup) return labelEnd - right;
	// A group's label stops short of its marker, and the marker itself, which
	// ends at the row's inset, has to be on screen.
	const float markerEnd = rect.x + rect.w - Rem(kMenuRightInset);
	return std::max(labelEnd - (MarkerX(rect) - Rem(kMenuMarkerGap)), markerEnd - right);
}

gfx::Rect ContextMenu::InkRect() const {
	if (!m_open || !m_laidOut) return Pixel();
	gfx::Rect box{m_x, m_y, m_w, m_rowH * static_cast<float>(TopCount())};
	if (m_openChild >= 0 && m_openChild == m_childLaidOut) {
		const float x0 = std::min(box.x, m_childX), y0 = std::min(box.y, m_childY);
		const float x1 = std::max(box.x + box.w, m_childX + m_childW);
		const float y1 = std::max(box.y + box.h,
								  m_childY + m_rowH * static_cast<float>(ChildCount(m_openChild)));
		box = {x0, y0, x1 - x0, y1 - y0};
	}
	return box;
}

float ContextMenu::TextOverrun() const {
	if (!m_open || !m_laidOut) return 0.0f;
	const bool childShown = m_openChild >= 0 && m_openChild == m_childLaidOut;
	float over = 0.0f;
	size_t pos = 0, kid = 0;
	for (size_t r = 0; r < m_count; ++r) {
		const Row& row = m_rows[r];
		if (row.group == kTopLevel)
			over = std::max(over, RowOverrun(row, EntryRect(pos++)));
		else if (childShown && row.group == m_openChild)
			over = std::max(over, RowOverrun(row, ChildRect(kid++)));
	}
	return over;
}

void ContextMenu::Begin(float x, float y) {
	m_count = 0;
	m_x = x;
	m_y = y;
	m_hover = -1;
	m_openChild = -1;
	m_childHover = -1;
	m_open = false;
	m_laidOut = false;
	m_childLaidOut = -1;
}

int ContextMenu::Append(std::string_view label, int id, int group, bool isGroup) {
	if (m_count >= kMaxRows) return kTopLevel;
	Row& row = m_rows[m_count];
	row.len = std::min(label.size(), kLabelCapacity);
	std::copy_n(label.data(), row.len, row.text);
	row.text[row.len] = '\0';
	row.id = id;
	row.group = group;
	row.isGroup = isGroup;
	return static_cast<int>(m_count++);
}

bool ContextMenu::Add(std::string_view label, int id, int group) {
	// A group handle is the group row's index; anything else is top level.
	if (group != kTopLevel &&
		(group < 0 || group >= static_cast<int>(m_count) || !m_rows[group].isGroup))
		group = kTopLevel;
	return Append(label, id, group, false) != kTopLevel;
}

int ContextMenu::AddGroup(std::string_view label) {
	return Append(label, 0, kTopLevel, true);
}

void ContextMenu::Show() {
	if (m_count > 0) m_open = true;
}

size_t ContextMenu::Groups() const {
	size_t n = 0;
	for (size_t r = 0; r < m_count; ++r)
		if (m_rows[r].isGroup) ++n;
	return n;
}

bool ContextMenu::OpenGroup(size_t n) {
	for (size_t r = 0; r < m_count; ++r) {
		if (!m_rows[r].isGroup) continue;
		if (n-- > 0) continue;
		m_openChild = static_cast<int>(r); // laid out by the next Update
		return true;
	}
	return false;
}

size_t ContextMenu::TopPosition(int row) const {
	size_t pos = 0;
	for (int r = 0; r < row; ++r)
		if (m_rows[r].group == kTopLevel) ++pos;
	return pos;
}

size_t ContextMenu::TopCount() const {
	return TopPosition(static_cast<int>(m_count));
}

size_t ContextMenu::ChildCount(int group) const {
	size_t n = 0;
	for (size_t r = 0; r < m_count; ++r)
		if (m_rows[r].group == group) ++n;
	return n;
}

gfx::Rect ContextMenu::EntryRect(size_t i) const {
	return {m_x, m_y + m_rowH * static_cast<float>(i), m_w, m_rowH};
}

gfx::Rect ContextMenu::ChildRect(size_t i) const {
	return {m_childX, m_childY + m_rowH * static_cast<float>(i), m_childW, m_rowH};
}

void ContextMenu::Pick(UIContext& ctx, int id) {
	auto fn = onPick; // copy: the callback may rebuild us
	Close();
	ctx.ConsumeMouse();
	if (fn) fn(id);
}

void ContextMenu::UpdateSelf(UIContext& ctx) {
	if (!m_open) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	// Size to the widest label (a group's leaves room for its marker) at the
	// insets the draw uses, then clamp the box on screen.
	const Font& font = TextFont();
	const float pad = Rem(kMenuLabelInset + kMenuRightInset);
	const float marker = Rem(kMenuMarkerGap) + font.MeasureWidth(kMenuMarker);
	m_rowH = Rem(1.45f);
	float w = Rem(kMenuMinW);
	for (size_t r = 0; r < m_count; ++r) {
		const Row& row = m_rows[r];
		if (row.group != kTopLevel) continue;
		w = std::max(w, font.MeasureWidth(row.Label()) + pad + (row.isGroup ? marker : 0.0f));
	}
	m_w = w;
	m_screenW = ctx.Width();
	m_laidOut = true;
	const float menuH = m_rowH * static_cast<float>(TopCount());
	m_x = std::clamp(m_x, 0.0f, std::max(0.0f, ctx.Width() - m_w));
	m_y = std::clamp(m_y, 0.0f, std::max(0.0f, ctx.Height() - menuH));

	// Lay the open group's submenu beside the parent: at its row, flush with
	// the parent's right edge — flipped to the left edge when it would run off
	// screen — with the parent still fully visible.
	if (m_openChild >= 0) {
		float cw = Rem(kMenuMinW);
		for (size_t r = 0; r < m_count; ++r)
			if (m_rows[r].group == m_openChild)
				cw = std::max(cw, font.MeasureWidth(m_rows[r].Label()) + pad);
		m_childW = cw;
		m_childX = m_x + m_w;
		if (m_childX + cw > ctx.Width()) m_childX = std::max(0.0f, m_x - cw);
		const float childH = m_rowH * static_cast<float>(ChildCount(m_openChild));
		const float rowY =
			m_y + m_rowH * static_cast<float>(TopPosition(m_openChild));
		m_childY = std::clamp(rowY, 0.0f, std::max(0.0f, ctx.Height() - childH));
		m_childLaidOut = m_openChild;
	}

	// The open menu owns the mouse. The submenu is checked first (it can
	// overlap the parent when flipped left): a leaf pick closes everything.
	m_hover = -1;
	m_childHover = -1;
	if (m_openChild >= 0) {
		size_t pos = 0;
		for (size_t r = 0; r < m_count; ++r) {
			if (m_rows[r].group != m_openChild) continue;
			if (!ChildRect(pos++).Contains(input->MouseX(), input->MouseY())) continue;
			m_childHover = static_cast<int>(r);
			if (input->WasMousePressed(MouseButton::Left)) {
				Pick(ctx, m_rows[r].id);
				return;
			}
			ctx.ConsumeMouse();
			return; // over the submenu — the parent rows don't hit-test
		}
	}
	size_t pos = 0;
	for (size_t r = 0; r < m_count; ++r) {
		const Row& row = m_rows[r];
		if (row.group != kTopLevel) continue;
		if (!EntryRect(pos++).Contains(input->MouseX(), input->MouseY())) continue;
		m_hover = static_cast<int>(r);
		if (input->WasMousePressed(MouseButton::Left)) {
			if (row.isGroup) {
				// A group: open its submenu (same group toggles, another
				// swaps). The parent stays up for the next pick.
				const int idx = static_cast<int>(r);
				m_openChild = m_openChild == idx ? -1 : idx;
			} else {
				Pick(ctx, row.id);
				return;
			}
		}
		ctx.ConsumeMouse();
		return;
	}
	if (input->WasMousePressed(MouseButton::Left) ||
		input->WasMousePressed(MouseButton::Right))
		Close();
	ctx.ConsumeMouse();
}

void ContextMenu::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	// Skinned: ONE panel face behind the whole menu (opaque, like the other
	// popups), rows keep only their hover/active washes; flat mode keeps the
	// per-row fills + borders.
	const Skin* skin = PanelSkin(ctx);
	const bool skinned = skin != nullptr;
	const size_t topCount = TopCount();
	if (skinned && topCount > 0) {
		const gfx::Rect box{m_x, m_y, m_w, m_rowH * static_cast<float>(topCount)};
		DrawFace(batch, box, *skin, Face::Panel, {1, 1, 1, 1});
	}
	size_t pos = 0;
	for (size_t r = 0; r < m_count; ++r) {
		const Row& row = m_rows[r];
		if (row.group != kTopLevel) continue;
		const gfx::Rect rect = EntryRect(pos++);
		const bool groupOpen = static_cast<int>(r) == m_openChild;
		const bool hovered = static_cast<int>(r) == m_hover;
		if (skinned) {
			if (groupOpen || hovered) {
				Vec4 wash = groupOpen ? theme.controlActive : theme.controlHot;
				wash.w = 0.4f;
				batch.DrawRect(rect, wash);
			}
		} else {
			batch.DrawRect(rect, groupOpen ? theme.controlActive
										   : (hovered ? theme.controlHot
													  : theme.control));
			DrawBorder(batch, rect, theme.panelBorder);
		}
		font.Draw(batch, row.Label(), LabelX(rect),
				  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
		if (row.isGroup) // group marker at the right edge
			font.Draw(batch, kMenuMarker, MarkerX(rect),
					  rect.y + (rect.h - font.Height()) * 0.5f,
					  groupOpen ? theme.text : theme.textDim);
	}
	// The open group's submenu, beside the parent (drawn after = on top).
	if (m_openChild >= 0) {
		const size_t kids = ChildCount(m_openChild);
		if (skinned && kids > 0) {
			const gfx::Rect box{m_childX, m_childY, m_childW,
								m_rowH * static_cast<float>(kids)};
			DrawFace(batch, box, *skin, Face::Panel, {1, 1, 1, 1});
		}
		size_t kid = 0;
		for (size_t r = 0; r < m_count; ++r) {
			const Row& row = m_rows[r];
			if (row.group != m_openChild) continue;
			const gfx::Rect rect = ChildRect(kid++);
			const bool hovered = static_cast<int>(r) == m_childHover;
			if (skinned) {
				if (hovered) {
					Vec4 wash = theme.controlHot;
					wash.w = 0.4f;
					batch.DrawRect(rect, wash);
				}
			} else {
				batch.DrawRect(rect, hovered ? theme.controlHot : theme.control);
				DrawBorder(batch, rect, theme.panelBorder);
			}
			font.Draw(batch, row.Label(), LabelX(rect),
					  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
		}
	}
}

// --- ColorPicker -------------------------------------------------------------

namespace {

// Popup geometry in REM (UI/Units.h). Four channel rows, each:
// letter | track | 0..255 value.
constexpr float kPickerPopupW = 11.4f;
constexpr float kPickerPopupPad = 0.45f;
constexpr float kPickerRowPitch = 1.3f;
constexpr float kPickerRowH = 1.0f;
constexpr float kPickerLetter = 0.95f; // column before the track
constexpr float kPickerValue = 2.0f;   // column after it
constexpr float kPickerPopupH =
	kPickerPopupPad * 2 + kPickerRowPitch * 3 + kPickerRowH;

float& Channel(Vec4& color, int index) {
	switch (index) {
	case 0: return color.x;
	case 1: return color.y;
	case 2: return color.z;
	default: return color.w;
	}
}

gfx::Rect PickerRow(const gfx::Rect& popup, int index, float rem) {
	return {popup.x + kPickerPopupPad,
			popup.y + (kPickerPopupPad + kPickerRowPitch * static_cast<float>(index)) * rem,
			popup.w - 2 * kPickerPopupPad * rem, kPickerRowH * rem};
}

gfx::Rect PickerTrack(const gfx::Rect& row, float rem) {
	const float letter = kPickerLetter * rem, value = kPickerValue * rem;
	return {row.x + letter, row.y, row.w - letter - value, row.h};
}

} // namespace

gfx::Rect ColorPicker::SwatchRect() const {
	const gfx::Rect& px = Pixel();
	const float w = std::min(Rem(2.3f), px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

gfx::Rect ColorPicker::PopupRect(const UIContext& ctx) const {
	const gfx::Rect swatch = SwatchRect();
	const float w = Rem(kPickerPopupW), h = Rem(kPickerPopupH);
	const float x =
		std::clamp(swatch.x + swatch.w - w, 0.0f, std::max(0.0f, ctx.Width() - w));
	float y = swatch.y + swatch.h + Rem(0.15f);
	if (y + h > ctx.Height()) // no room below: open above instead
		y = swatch.y - Rem(0.15f) - h;
	return {x, y, w, h};
}

void ColorPicker::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	if (m_open) {
		const gfx::Rect popup = PopupRect(ctx);
		if (m_dragChannel >= 0 && !input->IsMouseDown(MouseButton::Left))
			m_dragChannel = -1;
		if (m_dragChannel < 0 && input->WasMousePressed(MouseButton::Left)) {
			if (popup.Contains(mx, my)) {
				for (int i = 0; i < 4; ++i)
					if (PickerRow(popup, i, Rem()).Contains(mx, my)) m_dragChannel = i;
			} else {
				m_open = false; // click anywhere else (incl. the swatch) closes
				if (onClose) onClose();
			}
		}
		if (m_open && input->WasKeyPressed(vk::Escape)) {
			m_dragChannel = -1;
			m_open = false;
			if (onClose) onClose();
		}
		if (m_dragChannel >= 0) {
			const gfx::Rect track = PickerTrack(PickerRow(popup, m_dragChannel, Rem()), Rem());
			const float t =
				std::clamp((mx - track.x) / std::max(track.w, 1.0f), 0.0f, 1.0f);
			float& value = Channel(m_color, m_dragChannel);
			if (t != value) {
				value = t;
				if (onChange) onChange(m_color);
			}
		}
		// The open popup owns the mouse entirely — wheel included, so the page
		// behind cannot scroll the popup off its own swatch — and, through
		// ClaimPopup, before any control the walk reaches first.
		ctx.ConsumeMouse();
		ctx.ConsumeWheel();
		if (m_open) ctx.ClaimPopup();
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && SwatchRect().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) OpenPopup(ctx);
	}
}

void ColorPicker::OpenPopup(UIContext& ctx) {
	m_open = true;
	ctx.ClaimPopup();
}

void ColorPicker::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect swatch = SwatchRect();
	batch.DrawRect(swatch, {0, 0, 0, 1}); // opaque base so alpha reads as darkness
	batch.DrawRect(swatch, m_color);
	DrawBorder(batch, swatch, m_hot || m_open ? theme.accent : theme.panelBorder);
}

void ColorPicker::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect popup = PopupRect(ctx);

	if (const Skin* skin = PanelSkin(ctx)) {
		DrawFace(batch, popup, *skin, Face::Panel, {1, 1, 1, 1}); // opaque, unlike Panel
	} else {
		Vec4 background = theme.panel;
		background.w = 1.0f; // opaque so the page beneath doesn't bleed through
		batch.DrawRect(popup, background);
		DrawBorder(batch, popup, theme.panelBorder);
	}

	static constexpr const char* kChannelNames[4] = {"R", "G", "B", "A"};
	static constexpr Vec4 kChannelTints[4] = {{0.9f, 0.3f, 0.3f, 1.0f},
											  {0.3f, 0.85f, 0.3f, 1.0f},
											  {0.35f, 0.55f, 1.0f, 1.0f},
											  {0.8f, 0.8f, 0.8f, 1.0f}};
	for (int i = 0; i < 4; ++i) {
		const gfx::Rect row = PickerRow(popup, i, Rem());
		const gfx::Rect track = PickerTrack(row, Rem());
		const float value = Channel(m_color, i);
		const float textY = row.y + (row.h - font.Height()) * 0.5f;

		font.Draw(batch, kChannelNames[i], row.x, textY, theme.textDim);

		const float trackH = Rem(0.15f);
		const float trackY = track.y + (track.h - trackH) * 0.5f;
		batch.DrawRect({track.x, trackY, track.w, trackH}, theme.control);
		batch.DrawRect({track.x, trackY, track.w * value, trackH}, kChannelTints[i]);
		// Slider's proportions: half the height, twice the width it had.
		const float tw = Rem(0.6f);
		const float th = (track.h - Rem(0.16f)) * 0.5f;
		const gfx::Rect thumb{track.x + track.w * value - tw * 0.5f,
							  trackY + trackH * 0.5f - th * 0.5f, tw, th};
		batch.DrawRect(thumb,
					   m_dragChannel == i ? theme.controlActive : theme.controlHot);
		DrawBorder(batch, thumb, theme.panelBorder);

		font.Draw(batch, std::format("{}", static_cast<int>(value * 255.0f + 0.5f)),
				  track.x + track.w + Rem(0.35f), textY, theme.text);
	}
}

// --- KeyBind -----------------------------------------------------------------

KeyBind::KeyBind(const gfx::Rect& rect, std::string label, int vkey,
				 std::function<void(int)> onChange)
	: label(std::move(label)), onChange(std::move(onChange)) {
	bounds = rect;
	SetKey(vkey);
}

void KeyBind::SetKey(int vkey) {
	m_vkey = vkey & 0xFF;
	m_keyName = KeyName(m_vkey);
}

gfx::Rect KeyBind::BoxRect() const {
	const gfx::Rect& px = Pixel();
	const float w = std::min(Rem(7.1f), px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

void KeyBind::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	if (m_capturing) {
		if (input->WasMousePressed(MouseButton::Left) ||
			input->WasKeyPressed(vk::Escape)) {
			m_capturing = false; // any click (incl. the box) or Esc cancels
		} else if (const int vkey = input->FirstPressedKey(); vkey >= 0) {
			m_capturing = false;
			SetKey(vkey);
			if (onChange) onChange(m_vkey);
		}
		ctx.ConsumeMouse(); // the armed box owns the mouse entirely
		return;
	}

	m_hot = !ctx.IsMouseConsumed() &&
			BoxRect().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_capturing = true;
	}
}

void KeyBind::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect box = BoxRect();
	DrawFieldFace(ctx, batch, box,
				  m_capturing ? FieldState::Active : (m_hot ? FieldState::Hot : FieldState::Normal),
				  m_capturing ? theme.controlActive : (m_hot ? theme.controlHot : theme.control),
				  m_capturing || m_hot ? theme.accent : theme.panelBorder);

	const std::string& text = m_capturing ? capturePrompt : m_keyName;
	const float textW = font.MeasureWidth(text);
	font.Draw(batch, text, box.x + (box.w - textW) * 0.5f,
			  box.y + (box.h - font.Height()) * 0.5f,
			  m_capturing ? theme.accent : theme.text);
}

// --- TextField -------------------------------------------------------------

void TextField::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (input->WasMousePressed(MouseButton::Left)) {
		if (m_hot) {
			m_focused = true;
			ctx.ConsumeMouse();
		} else if (!Pixel().Contains(input->MouseX(), input->MouseY())) {
			m_focused = false; // a click elsewhere drops focus (don't consume it)
		}
	} else if (m_hot) {
		ctx.ConsumeMouse();
	}
	if (!m_focused) return;

	// The typed text in order (Input::TypedChars): Backspace where it fell, and
	// an Enter ends this frame's typing - submitting usually closes whatever the
	// field is on, so nothing typed after it is applied to the old text.
	// WHOLE CHARACTERS (C383): Backspace takes the last character however many
	// bytes it is, and maxLength counts characters, so a full field refuses a
	// letter whole rather than keeping half of it.
	bool changed = false;
	bool submit = false;
	const std::string_view typed = input->TypedChars();
	for (size_t i = 0; i < typed.size();) {
		const std::string_view ch = utf8::CharAt(typed, i);
		i += ch.size();
		if (ch[0] == Input::kTypedEnter) {
			submit = true;
			break;
		}
		if (ch[0] == Input::kTypedBack) {
			if (!utf8::PopBack(text)) continue;
		} else {
			if (utf8::Length(text) >= maxLength) continue;
			text.append(ch);
		}
		changed = true;
	}
	if (submit && onSubmit) onSubmit();
	if (changed && onChange) onChange();
}

void TextField::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	const gfx::Rect face = DrawFieldFace(
		ctx, batch, px,
		m_focused ? FieldState::Active : (m_hot ? FieldState::Hot : FieldState::Normal),
		m_focused ? theme.controlActive : (m_hot ? theme.controlHot : theme.control),
		m_focused || m_hot ? theme.accent : theme.panelBorder);

	const float pad = Rem(0.3f);
	const float ty = px.y + (px.h - font.Height()) * 0.5f;
	// Text is CLIPPED to the field, and scrolled to follow the caret. Without
	// this a value wider than the box simply paints past its own border: the
	// control was fine for as long as every field held a short number, and the
	// first long value put in one (a list of theme tags) spilled ink outside the
	// control and over the panel edge. That breaks the rule the whole layout
	// rests on — a widget's area is its own — and `uioverlap` scores it as an
	// escape.
	// Inside the face's frame (the flat face returns the whole rect, so that is
	// the old 1px inset).
	const gfx::Rect inner{face.x + 1.0f, face.y + 1.0f, face.w - 2.0f, face.h - 2.0f};
	ScopedClip clip(batch, inner);
	if (text.empty() && !m_focused) {
		font.Draw(batch, placeholder, px.x + pad, ty, theme.textDim);
	} else {
		// FOCUSED: slide left so the caret stays in view — you have to be able to
		// see what you are typing. UNFOCUSED: show the START of the value, which
		// is the part that identifies it, and snapping back on blur is what every
		// other text field does.
		const float textW = font.MeasureWidth(text);
		const float avail = std::max(px.w - pad * 2.0f, 0.0f);
		const float scroll = m_focused ? std::max(0.0f, textW - avail) : 0.0f;
		font.Draw(batch, text, px.x + pad - scroll, ty, theme.text);
		if (m_focused) {
			const float caretX = px.x + pad - scroll + textW + 1.0f;
			// 2px caret is a hairline and stays one (Units.h).
			batch.DrawRect({caretX, px.y + Rem(0.22f), 2.0f, px.h - Rem(0.44f)},
						   theme.accent);
		}
	}
}

// --- SlotList ----------------------------------------------------------------

SlotRow::SlotRow(std::string primary, std::string secondary,
				 std::function<void()> onActivate, bool deletable,
				 const gfx::Texture* const* icon,
				 std::function<void()> onDeleteClick)
	: m_primary(std::move(primary)), m_secondary(std::move(secondary)),
	  m_onActivate(std::move(onActivate)),
	  m_onDeleteClick(std::move(onDeleteClick)), m_icon(icon),
	  m_deletable(deletable) {
	debugName = "SlotRow";
}

gfx::Rect SlotRow::DeleteRect() const {
	const gfx::Rect& r = Pixel();
	const float s = r.h - Rem(0.5f);
	return {r.x + r.w - s - Rem(0.3f), r.y + (r.h - s) * 0.5f, s, s};
}

float SlotRow::Depth() const {
	const auto since = [](Clock::time_point t) {
		return std::chrono::duration<float>(Clock::now() - t).count();
	};
	if (m_held || m_sinking) return std::min(1.0f, since(m_pressAt) / Button::kSinkSeconds);
	if (m_rising) return std::max(0.0f, 1.0f - since(m_riseAt) / Button::kRiseSeconds);
	return 0.0f;
}

void SlotRow::UpdateSelf(UIContext& ctx) {
	m_hot = m_hotDelete = false;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const Clock::time_point now = Clock::now();
	const auto since = [&](Clock::time_point t) {
		return std::chrono::duration<float>(now - t).count();
	};
	// The push in flight acts at the bottom of its sink. The callback may
	// (deferred) rebuild the page that owns this widget - fire and touch
	// nothing afterwards.
	if (m_sinking && since(m_pressAt) >= Button::kSinkSeconds + Button::kHoldSeconds) {
		m_sinking = false;
		m_rising = true;
		m_riseAt = now;
		if (m_onActivate) m_onActivate();
		return;
	}
	if (m_rising && since(m_riseAt) >= Button::kRiseSeconds) m_rising = false;

	const float mx = input->MouseX(), my = input->MouseY();
	if (!ctx.IsMouseConsumed() && Pixel().Contains(mx, my)) {
		m_hot = true;
		m_hotDelete = m_deletable && DeleteRect().Contains(mx, my);
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left) && !m_sinking) {
			if (m_hotDelete) {
				// The confirm the click opens is modal: claim the pointer from the
				// first widget of the next walk on, before the list renews it.
				ctx.ClaimPopup();
				if (m_onDeleteClick) m_onDeleteClick();
				return;
			}
			m_held = true;
			m_rising = false;
			m_pressAt = now;
		}
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		m_held = false;
		if (m_hot && !m_hotDelete) {
			m_sinking = true;
		} else { // cancelled: rise from wherever the sink had got to
			m_rising = true;
			m_riseAt = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(
								 Button::kRiseSeconds *
								 (1.0f - std::min(1.0f, since(m_pressAt) / Button::kSinkSeconds))));
		}
	}
}

void SlotRow::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& r = Pixel();
	const Skin* skin = ctx.GetSkin();
	const bool stone = skin && skin->block.texture;
	const bool lit = m_hot && !m_hotDelete;
	float sink = 0.0f;
	if (stone) {
		// A cut stone like a menu entry: its words carved, the name in gold (lit
		// under the pointer, with the entry's gold hairline), the date plain.
		const float depth = Depth();
		DrawCutStone(batch, r, *skin, nullptr, depth, lit, {1, 1, 1, 1});
		if (lit) {
			const float in = FaceInset(*skin, Face::Block) * 0.35f;
			DrawBorder(batch, {r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in},
					   {theme.accent.x, theme.accent.y, theme.accent.z, 0.65f});
		}
		sink = depth * std::max(1.0f, r.h * 0.035f);
	} else {
		batch.DrawRect(r, m_hot ? theme.controlHot : theme.control);
		DrawBorder(batch, r, theme.panelBorder);
	}

	const float ty = r.y + (r.h - font.Height()) * 0.5f + sink;
	const float left = r.x + (stone ? FaceInset(*skin, Face::Block) + Rem(0.45f) : Rem(0.45f));
	if (stone) DrawCarvedText(batch, font, m_primary, left + sink, ty, lit ? CarvedLit(skin) : CarvedGold(skin));
	else font.Draw(batch, m_primary, left, ty, theme.text);

	const gfx::Rect del = DeleteRect();
	if (!m_secondary.empty()) {
		const float sw = font.MeasureWidth(m_secondary);
		const float sx = (m_deletable ? del.x : r.x + r.w) - sw - Rem(0.6f) -
						 (stone && !m_deletable ? FaceInset(*skin, Face::Block) : 0.0f);
		if (stone) DrawCarvedText(batch, font, m_secondary, sx + sink, ty, CarvedPlain(skin));
		else font.Draw(batch, m_secondary, sx, ty, theme.textDim);
	}
	if (!m_deletable) return;
	if (stone) {
		// On the stone the red X shouted over the names (Michael: "too loud",
		// twice - a muted, smaller icon was still too much). It is a CARVED
		// cross now, like the words: the bare cut at rest, a dull red in it only
		// under the pointer.
		// At the row's text size the cross was a speck ("now it's too small"):
		// it gets a face of its own, sized to its box.
		static constexpr std::string_view kCross = "\xC3\x97"; // U+00D7, in the Latin-1 bake
		const Font& crossFont = ctx.FontAt(FontRole::Body, del.h * 1.35f);
		const float xw = crossFont.MeasureWidth(kCross);
		DrawCarvedText(batch, crossFont, kCross, del.x + (del.w - xw) * 0.5f + sink,
					   del.y + (del.h - crossFont.Height()) * 0.5f + sink,
					   m_hotDelete ? Vec4{0.78f, 0.34f, 0.24f, 1.0f} : Vec4{0.60f, 0.56f, 0.50f, 0.55f});
	} else if (const gfx::Texture* icon = m_icon ? *m_icon : nullptr) {
		batch.DrawSprite(del, {0, 0, 1, 1}, *icon,
						 {1, 1, 1, m_hotDelete ? 1.0f : 0.8f});
	} else { // fallback: an "X" glyph in the accent color
		const float xw = font.MeasureWidth("X");
		font.Draw(batch, "X", del.x + (del.w - xw) * 0.5f,
				  del.y + (del.h - font.Height()) * 0.5f, theme.accent);
	}
}

SlotList::SlotList(const gfx::Rect& rect) {
	bounds = rect;
	debugName = "SlotList";
	// The rows are DIRECT children of the scroll area (no repeater — they are
	// known when the page is built), so their bounds are what it measures
	// overflow from.
	m_scroll = Add<ScrollArea>(gfx::Rect{0, 0, 1, 1});
	m_scroll->padding = 0.0f;
	m_scroll->debugName = "SlotScroll";
}

void SlotList::AddRow(Row row) {
	const size_t index = m_entries.size();
	const bool deletable = static_cast<bool>(row.onDelete);
	m_entries.push_back({row.primary, std::move(row.onDelete)});
	m_scroll->Add<SlotRow>(std::move(row.primary), std::move(row.secondary),
						   std::move(row.onActivate), deletable, &deleteIcon,
						   [this, index] { m_confirmRow = static_cast<int>(index); });
}

// rowHeight is in rem (known only at layout) and the rows are fractions of the
// scrolling area, so the stack is assigned per layout. A left inset keeps them off the frame; the
// area's gutter already holds the scrollbar clear.
void SlotList::LayoutSelf(UIContext&) {
	// gutter is in REM (Controls.h) — ScrollArea multiplies it by its own root
	// font size. Handing it Rem(0.5f) passed ALREADY-CONVERTED PIXELS, which it
	// then converted again: 0.5rem became 14rem, a 392px gutter that ate more
	// than half of a 720px list and squeezed every row down to 324px, so the
	// name and the right-aligned timestamp landed on top of each other.
	m_scroll->gutter = 0.5f;
	const float view = m_scroll->ViewRect().h;
	const float width = m_scroll->ViewRect().w;
	if (view <= 0.0f || width <= 0.0f) return;
	const float x = Rem(0.15f) / width;
	const auto& rows = m_scroll->Children();
	for (size_t i = 0; i < rows.size(); ++i)
		rows[i]->bounds = {x, rowHeight * Rem() * static_cast<float>(i) / view,
						   1.0f - x, (rowHeight * Rem() - Rem(0.2f)) / view};
}

// Modal confirm dialog: it owns the mouse entirely until Delete/Cancel (or a
// click outside it). Running before the children is what takes the mouse off
// the rows underneath.
void SlotList::UpdateBeforeChildren(UIContext& ctx) {
	if (m_confirmRow < 0) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	ctx.ConsumeMouse();
	ctx.ConsumeWheel(); // a modal freezes the list behind it, scroll included
	// ...and the page around it: a control added after the list (the page's
	// Back stone) is updated before it and would see the click first.
	ctx.ClaimPopup();
	const float mx = input->MouseX(), my = input->MouseY();
	const gfx::Rect del = ConfirmButton(ctx, true);
	const gfx::Rect cancel = ConfirmButton(ctx, false);
	m_confirmHot = del.Contains(mx, my) ? 0 : (cancel.Contains(mx, my) ? 1 : -1);
	if (!input->WasMousePressed(MouseButton::Left)) return;
	if (m_confirmHot == 0) {
		// Copy first: onDelete may (deferred) rebuild the page, which destroys
		// this widget — touch nothing after.
		auto fn = static_cast<size_t>(m_confirmRow) < m_entries.size()
					  ? m_entries[static_cast<size_t>(m_confirmRow)].onDelete
					  : std::function<void()>{};
		m_confirmRow = -1;
		if (fn) fn();
		return;
	}
	if (m_confirmHot == 1 || !ConfirmRect(ctx).Contains(mx, my))
		m_confirmRow = -1; // Cancel button or a click outside the dialog
}

gfx::Rect SlotList::ConfirmRect(const UIContext& ctx) const {
	const float w = Rem(13.5f), h = Rem(6.6f);
	return {(ctx.Width() - w) * 0.5f, (ctx.Height() - h) * 0.5f, w, h};
}

gfx::Rect SlotList::ConfirmButton(const UIContext& ctx, bool deleteButton) const {
	const gfx::Rect d = ConfirmRect(ctx);
	const float m = Rem(0.7f); // margin / gutter around the pair
	const float bw = (d.w - 3.0f * m) * 0.5f, bh = Rem(2.0f);
	const float by = d.y + d.h - bh - m;
	return deleteButton ? gfx::Rect{d.x + m, by, bw, bh}
						: gfx::Rect{d.x + d.w - m - bw, by, bw, bh};
}

void SlotList::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (m_confirmRow < 0 || static_cast<size_t>(m_confirmRow) >= m_entries.size())
		return;
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();

	// Dim the whole surface, then the dialog on top.
	batch.DrawRect({0, 0, ctx.Width(), ctx.Height()}, {0, 0, 0, 0.55f});
	const gfx::Rect d = ConfirmRect(ctx);
	DrawPanelFace(ctx, batch, d);

	const float pw = font.MeasureWidth(confirmPrompt);
	font.Draw(batch, confirmPrompt, d.x + (d.w - pw) * 0.5f, d.y + Rem(1.0f),
			  theme.text);
	const std::string& name = m_entries[static_cast<size_t>(m_confirmRow)].primary;
	const float nw = font.MeasureWidth(name);
	font.Draw(batch, name, d.x + (d.w - nw) * 0.5f, d.y + Rem(2.35f), theme.accent);

	const Skin* skin = ctx.GetSkin();
	auto button = [&](const gfx::Rect& b, const std::string& label, bool hot,
					  bool danger) {
		if (skin && skin->block.texture) {
			// Cut stones with carved words, like the page's own buttons.
			DrawCutStone(batch, b, *skin, nullptr, 0.0f, hot, {1, 1, 1, 1});
			DrawCarvedText(batch, font, label, b.x + (b.w - font.MeasureWidth(label)) * 0.5f,
						   b.y + (b.h - font.Height()) * 0.5f,
						   hot ? CarvedLit(skin) : (danger ? Vec4{0.86f, 0.40f, 0.26f, 1.0f} : CarvedGold(skin)));
			return;
		}
		batch.DrawRect(b, hot ? theme.controlActive : theme.control);
		DrawBorder(batch, b, hot || danger ? theme.accent : theme.panelBorder);
		const float lw = font.MeasureWidth(label);
		font.Draw(batch, label, b.x + (b.w - lw) * 0.5f,
				  b.y + (b.h - font.Height()) * 0.5f,
				  danger ? theme.accent : theme.text);
	};
	button(ConfirmButton(ctx, true), deleteLabel, m_confirmHot == 0, true);
	button(ConfirmButton(ctx, false), cancelLabel, m_confirmHot == 1, false);
}

// --- MenuList --------------------------------------------------------------

namespace {
// The flat look's "> <" markers sit this far in from either edge of the
// selected entry, in REM.
constexpr float kListMarkerInset = 0.57f;
} // namespace

void MenuList::AddItem(std::string label, std::function<void()> onActivate) {
	m_items.push_back({std::move(label), std::move(onActivate)});
}

void MenuList::SetLabel(size_t index, std::string label) {
	if (index < m_items.size()) m_items[index].label = std::move(label);
}

gfx::Rect MenuList::ItemRect(size_t index) const {
	const gfx::Rect& px = Pixel();
	const float itemH = m_itemHeight * px.h;
	return {px.x, px.y + itemH * static_cast<float>(index), px.w,
			itemH - Rem(gapRem)}; // gap between entries
}

void MenuList::MoveSelection(int delta) {
	if (m_items.empty()) return;
	const int count = static_cast<int>(m_items.size());
	m_selected = (m_selected + delta + count) % count; // wrap around
}

void MenuList::Activate(int index) {
	if (index >= 0 && index < static_cast<int>(m_items.size())) {
		const auto& onActivate = m_items[static_cast<size_t>(index)].onActivate;
		if (onActivate) onActivate();
	}
}

float MenuList::Depth(int index) const {
	if (index != m_pushItem) return 0.0f;
	const auto since = [](Clock::time_point t) {
		return std::chrono::duration<float>(Clock::now() - t).count();
	};
	if (m_held || m_sinking) return std::min(1.0f, since(m_pressAt) / Button::kSinkSeconds);
	if (m_rising) return std::max(0.0f, 1.0f - since(m_riseAt) / Button::kRiseSeconds);
	return 0.0f;
}

void MenuList::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const Clock::time_point now = Clock::now();
	const auto since = [&](Clock::time_point t) {
		return std::chrono::duration<float>(now - t).count();
	};

	// The push in flight: an entry ACTS at the bottom of its sink (Button's
	// rule), then rises. Acting may rebuild the page that owns this list, so it
	// is the last thing done.
	if (m_sinking && since(m_pressAt) >= Button::kSinkSeconds + Button::kHoldSeconds) {
		m_sinking = false;
		m_rising = true;
		m_riseAt = now;
		Activate(m_pushItem);
		return;
	}
	if (m_rising && since(m_riseAt) >= Button::kRiseSeconds) {
		m_rising = false;
		m_pushItem = -1;
	}

	// Mouse: hovering selects; a press holds the entry down and the RELEASE
	// over it completes the push (released elsewhere, it just rises).
	int over = -1;
	if (!ctx.IsMouseConsumed()) {
		for (size_t i = 0; i < m_items.size(); ++i) {
			if (!ItemRect(i).Contains(input->MouseX(), input->MouseY())) continue;
			over = static_cast<int>(i);
			m_selected = over;
			ctx.ConsumeMouse();
			if (input->WasMousePressed(MouseButton::Left) && !m_sinking) {
				m_pushItem = over;
				m_held = true;
				m_rising = false;
				m_pressAt = now;
			}
			break;
		}
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		m_held = false;
		if (over == m_pushItem) {
			m_sinking = true;
		} else { // cancelled: rise from wherever the sink had got to
			m_rising = true;
			m_riseAt = now - std::chrono::duration_cast<Clock::duration>(
								 std::chrono::duration<float>(
									 Button::kRiseSeconds *
									 (1.0f - std::min(1.0f, since(m_pressAt) /
																Button::kSinkSeconds))));
		}
	}

	// Keyboard: arrows / W/S move the selection, Enter/Space press it.
	if (m_held || m_sinking) return; // one push at a time
	if (input->WasKeyPressed(vk::Up) || input->WasKeyPressed('W')) MoveSelection(-1);
	if (input->WasKeyPressed(vk::Down) || input->WasKeyPressed('S')) MoveSelection(+1);
	if (input->WasKeyPressed(vk::Return) || input->WasKeyPressed(vk::Space)) {
		m_pushItem = m_selected;
		m_sinking = true;
		m_rising = false;
		m_pressAt = now;
	}
}

void DrawCarvedText(gfx::SpriteBatch& batch, const Font& font, std::string_view text,
					float x, float y, const Vec4& fill) {
	const Vec4 ring = batch.TextOutline();
	batch.SetTextOutline({0, 0, 0, 0});
	// ONE pixel each way, whatever the size, and the lit edge only a whisper:
	// scaled with the font (3 px on a title) and at 0.30, the pale copy read as
	// an echo of the word rather than a cut edge - Michael: "a pale outline
	// behind the text that makes things blurry".
	font.Draw(batch, text, x - 1.0f, y - 1.0f, {0.0f, 0.0f, 0.0f, 0.85f}); // the shadowed near wall
	font.Draw(batch, text, x + 1.0f, y + 1.0f, {1.0f, 1.0f, 1.0f, 0.10f}); // the lit far wall
	font.Draw(batch, text, x, y, fill);                                     // the gold in the cut
	batch.SetTextOutline(ring);
}

namespace {
Vec4 Mix(const Vec4& a, const Vec4& b, float t) {
	return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t};
}
// sRGB channel -> linear light, and a colour's relative luminance (WCAG 2).
float Linear(float c) {
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float RelLuminance(const Vec4& c) {
	return 0.2126f * Linear(c.x) + 0.7152f * Linear(c.y) + 0.0722f * Linear(c.z);
}
// The ends an ink is pushed toward when it does not read: a pale gold (still
// warm, so it reads as the same metal lit) and a dark bronze. A mid-grey stone
// caps any colour near 5:1, so these sit close to white and black.
constexpr Vec4 kInkPale{1.0f, 0.95f, 0.80f, 1.0f};
constexpr Vec4 kInkDeep{0.12f, 0.08f, 0.03f, 1.0f};
// What a part-transparent ink looks like over `bg` - the colour the eye judges.
Vec4 Over(const Vec4& ink, const Vec4& bg) {
	Vec4 seen = Mix(bg, ink, ink.w);
	seen.w = 1.0f;
	return seen;
}
// `ink` kept if it reads on `bg` at `target`, else moved the least distance along
// either ramp that gets there; if neither does, the ramp end that reads best.
Vec4 Legible(const Vec4& ink, const Vec4& bg, float target) {
	if (ContrastRatio(Over(ink, bg), bg) >= target) return ink;
	constexpr int kSteps = 20;
	int best = kSteps + 1;
	Vec4 pick = ink;
	for (const Vec4& end : {kInkPale, kInkDeep})
		for (int i = 1; i <= kSteps && i < best; ++i) {
			Vec4 c = Mix(ink, end, static_cast<float>(i) / kSteps);
			c.w = ink.w;
			if (ContrastRatio(Over(c, bg), bg) >= target) {
				best = i;
				pick = c;
			}
		}
	if (best <= kSteps) return pick;
	Vec4 pale = kInkPale, deep = kInkDeep;
	pale.w = deep.w = ink.w;
	return ContrastRatio(Over(pale, bg), bg) >= ContrastRatio(Over(deep, bg), bg) ? pale : deep;
}
} // namespace

float ContrastRatio(const Vec4& a, const Vec4& b) {
	const float la = RelLuminance(a), lb = RelLuminance(b);
	return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

void ResolveInks(Skin& skin) {
	const Vec4 bg{skin.stoneMean.x, skin.stoneMean.y, skin.stoneMean.z, 1.0f};
	const Skin authored; // the dark-stone inks the defaults carry
	skin.inkGold = Legible(authored.inkGold, bg, kInkContrast);
	skin.inkTitle = Legible(authored.inkTitle, bg, kInkContrast);
	skin.inkPlain = Legible(authored.inkPlain, bg, kInkContrastPlain);
	// The hover must still SHOW: lit reads at least as well as the gold, and
	// further from the stone than it, or it is the gold pushed further along.
	skin.inkLit = Legible(authored.inkLit, bg, kInkContrast);
	if (ContrastRatio(skin.inkLit, bg) < ContrastRatio(skin.inkGold, bg) + 0.5f) {
		const bool paler = RelLuminance(skin.inkGold) > RelLuminance(bg);
		skin.inkLit = Mix(skin.inkGold, paler ? Vec4{1, 1, 1, 1} : Vec4{0, 0, 0, 1}, 0.5f);
		skin.inkLit.w = 1.0f;
	}
	// Disabled: the gold sunk most of the way back into its own stone.
	skin.inkDisabled = Mix(skin.inkGold, bg, 0.55f);
	skin.inkDisabled.w = 1.0f;
}

Vec4 CarvedGold(const Skin* skin) { return skin ? skin->inkGold : Skin{}.inkGold; }
Vec4 CarvedLit(const Skin* skin) { return skin ? skin->inkLit : Skin{}.inkLit; }
Vec4 CarvedTitle(const Skin* skin) { return skin ? skin->inkTitle : Skin{}.inkTitle; }
Vec4 CarvedPlain(const Skin* skin) { return skin ? skin->inkPlain : Skin{}.inkPlain; }
Vec4 CarvedDisabled(const Skin* skin) {
	return skin ? skin->inkDisabled : Skin{}.inkDisabled;
}

void MenuList::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();

	if (const Skin* skin = ctx.GetSkin(); skin && skin->block.texture) {
		// Cut stones with carved words; the selected entry's gold lit.
		const Vec4 gold = CarvedGold(skin);
		const Vec4 lit = CarvedLit(skin);
		for (size_t i = 0; i < m_items.size(); ++i) {
			const gfx::Rect rect = ItemRect(i);
			const bool selected = static_cast<int>(i) == m_selected;
			const float depth = Depth(static_cast<int>(i));
			DrawCutStone(batch, rect, *skin, nullptr, depth, selected, {1, 1, 1, 1});
			if (selected) {
				// Lit gold alone was too quiet a mark for the keyboard's
				// selection: a gold hairline just inside the stone's joint too.
				const float in = FaceInset(*skin, Face::Block) * 0.35f;
				DrawBorder(batch, {rect.x + in, rect.y + in, rect.w - 2 * in, rect.h - 2 * in},
						   {theme.accent.x, theme.accent.y, theme.accent.z, 0.65f});
			}
			const std::string& label = m_items[i].label;
			const float sink = depth * std::max(1.0f, rect.h * 0.035f);
			DrawCarvedText(batch, font, label,
						   rect.x + (rect.w - font.MeasureWidth(label)) * 0.5f + sink,
						   rect.y + (rect.h - font.Height()) * 0.5f + sink,
						   selected ? lit : gold);
		}
		return;
	}

	for (size_t i = 0; i < m_items.size(); ++i) {
		const gfx::Rect rect = ItemRect(i);
		const bool selected = static_cast<int>(i) == m_selected;

		if (selected) {
			// Highlight: warm translucent bar + accent border + side markers.
			Vec4 fill = theme.accent;
			fill.w = 0.22f;
			batch.DrawRect(rect, fill);
			DrawBorder(batch, rect, theme.accent);
		}

		const std::string& label = m_items[i].label;
		const float textW = font.MeasureWidth(label);
		const float textX = rect.x + (rect.w - textW) * 0.5f;
		const float textY = rect.y + (rect.h - font.Height()) * 0.5f;
		font.Draw(batch, label, textX, textY, selected ? theme.accent : theme.text);

		if (selected) {
			// Side markers at REM insets (code-review C382; they were 16 and 24
			// raw pixels): the same distance in from either edge, which is what
			// those two numbers were at the 900p menu font.
			const float inset = Rem(kListMarkerInset);
			font.Draw(batch, ">", rect.x + inset, textY, theme.accent);
			font.Draw(batch, "<", rect.x + rect.w - inset - font.MeasureWidth("<"), textY,
					  theme.accent);
		}
	}
}

// --- ScrollArea ------------------------------------------------------------

gfx::Rect ScrollArea::ViewRect() const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(padding), gut = Rem(gutter);
	return {px.x + pad, px.y + pad, px.w - pad - gut, px.h - 2.0f * pad};
}

gfx::Rect ScrollArea::ContentRect() const {
	const gfx::Rect view = ViewRect();
	return {view.x, view.y - m_scroll, view.w, view.h};
}

float ScrollArea::ContentFraction() const {
	float maxBottom = 1.0f;
	for (const auto& child : Children())
		if (child->visible)
			maxBottom = std::max(maxBottom, child->bounds.y + child->bounds.h);
	return maxBottom;
}

float ScrollArea::MaxScroll() const {
	return (ContentFraction() - 1.0f) * ViewRect().h;
}

void ScrollArea::ScrollIntoView(const Widget& child) {
	const gfx::Rect view = ViewRect();
	if (view.h <= 0.0f) return;
	// The child's rect is already shifted by the current scroll; undo that to get
	// where it sits in the CONTENT, then move the window the shortest way.
	const float top = child.Pixel().y - view.y + m_scroll;
	const float bottom = top + child.Pixel().h;
	if (top < m_scroll) m_scroll = top;
	else if (bottom > m_scroll + view.h) m_scroll = bottom - view.h;
	m_scroll = std::clamp(m_scroll, 0.0f, MaxScroll());
}

gfx::Rect ScrollArea::ScrollTrackRect() const {
	const gfx::Rect& px = Pixel();
	const float barW = Rem(0.35f), inset = Rem(0.08f);
	return {px.x + px.w - barW - inset, px.y + inset, barW, px.h - 2 * inset};
}

gfx::Rect ScrollArea::ScrollThumbRect(float maxScroll) const {
	const gfx::Rect track = ScrollTrackRect();
	const float view = ViewRect().h;
	const float thumbH = std::max(track.h * view / (view + maxScroll), Rem(0.9f));
	const float t = maxScroll > 0.0f ? m_scroll / maxScroll : 0.0f;
	return {track.x, track.y + (track.h - thumbH) * t, track.w, thumbH};
}

// Clamp the scroll and cache the clip before the children resolve against
// ContentRect() — which is the view box shifted by exactly this scroll.
void ScrollArea::LayoutSelf(UIContext&) {
	m_scroll = std::clamp(m_scroll, 0.0f, MaxScroll());
	m_clip = ViewRect();
	// Clip only while there is something to scroll: sliders draw their labels
	// slightly above their bounds, and a static page shouldn't crop them.
	m_clipping = MaxScroll() > 0.0f;
}

const gfx::Rect* ScrollArea::ChildClip() const {
	return m_clipping ? &m_clip : nullptr;
}

// Skip children scrolled fully out of the view, in every pass.
bool ScrollArea::ChildActive(const Widget& child) const {
	const float view = ViewRect().h;
	const float top = child.bounds.y * view - m_scroll;
	const float bottom = (child.bounds.y + child.bounds.h) * view - m_scroll;
	return bottom > 0.0f && top < view;
}

// Runs after the children (the tree walk's order), so an open popup — which
// consumes the mouse — can't be scrolled out from under the user.
void ScrollArea::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	const float maxScroll = MaxScroll();
	m_scrollHot = false;
	if (maxScroll <= 0.0f) {
		m_scrollDragging = false;
		return;
	}

	const gfx::Rect track = ScrollTrackRect();
	const gfx::Rect thumb = ScrollThumbRect(maxScroll);
	if (m_scrollDragging && !input->IsMouseDown(MouseButton::Left))
		m_scrollDragging = false;
	if (!ctx.IsMouseConsumed() || m_scrollDragging) {
		m_scrollHot = thumb.Contains(mx, my);
		if (m_scrollHot && input->WasMousePressed(MouseButton::Left)) {
			m_scrollDragging = true;
			m_scrollGrab = my - thumb.y;
		}
		if (m_scrollDragging) {
			const float range = track.h - thumb.h;
			if (range > 0.0f)
				m_scroll = std::clamp((my - m_scrollGrab - track.y) / range * maxScroll,
									  0.0f, maxScroll);
		}
		if (m_scrollHot || m_scrollDragging) ctx.ConsumeMouse();
	}

	// Mouse wheel anywhere over the area — including over the controls INSIDE
	// it, which is the whole point of the wheel having its own claim: a slider
	// under the cursor wants the click, not the scroll.
	if (!ctx.IsWheelConsumed() && Pixel().Contains(mx, my) &&
		input->WheelDelta() != 0.0f) {
		m_scroll = std::clamp(m_scroll - input->WheelDelta() * Rem(1.75f), 0.0f,
							  maxScroll);
		ctx.ConsumeWheel();
	}
}

// The track sits in the reserved gutter, outside ContentRect, so drawing it
// before the children (the tree walk's order) never puts it under them.
void ScrollArea::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const float maxScroll = MaxScroll();
	if (maxScroll <= 0.0f) return;
	const Theme& theme = ctx.GetTheme();
	batch.DrawRect(ScrollTrackRect(), theme.control);
	const gfx::Rect thumb = ScrollThumbRect(maxScroll);
	batch.DrawRect(thumb, m_scrollDragging || m_scrollHot ? theme.controlActive
														  : theme.controlHot);
	DrawBorder(batch, thumb, theme.panelBorder);
}

// --- TabControl ------------------------------------------------------------

size_t TabControl::AddTab(std::string label) {
	// The page fills ContentRect() (the area below the strip) and is this
	// control's Nth child, matching the tab's index.
	ScrollArea* page = Add<ScrollArea>(gfx::Rect{0, 0, 1, 1});
	page->debugName = "TabPage";
	m_tabs.push_back({std::move(label), page});
	return m_tabs.size() - 1;
}

void TabControl::SetActiveTab(int index) {
	if (index >= 0 && index < static_cast<int>(m_tabs.size())) m_active = index;
}

gfx::Rect TabControl::ContentRect() const { return PageRect(); }

void TabControl::LayoutSelf(UIContext& ctx) {
	LayoutStrip(ctx); // size the strip + control before any rect math
	// Only the active tab's page takes part in the walk.
	for (size_t i = 0; i < m_tabs.size(); ++i)
		m_tabs[i].page->visible = static_cast<int>(i) == m_active;
}

void TabControl::LayoutStrip(UIContext&) {
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const float count = static_cast<float>(std::max<size_t>(m_tabs.size(), 1));
	const float evenW = px.w / count;
	const float padX = Rem(0.8f); // breathing room each side of the label
	m_tabWidths.resize(m_tabs.size());
	float total = 0.0f;
	for (size_t i = 0; i < m_tabs.size(); ++i) {
		// Never below the even split, so short labels keep the original look;
		// a long one (e.g. "Controls") widens just its own tab.
		m_tabWidths[i] = std::max(evenW, font.MeasureWidth(m_tabs[i].label) + 2.0f * padX);
		total += m_tabWidths[i];
	}
	// Grow the control to the strip total and recenter on the authored center,
	// so it expands symmetrically rather than off to one side.
	m_effRect = {px.x - (total - px.w) * 0.5f, px.y, total, px.h};
}

gfx::Rect TabControl::TabRect(size_t index) const {
	// Fallback to an even split before LayoutStrip has run (e.g. first frame).
	if (m_tabWidths.size() != m_tabs.size()) {
		const gfx::Rect& px = Pixel();
		const float tabW = px.w / static_cast<float>(std::max<size_t>(m_tabs.size(), 1));
		return {px.x + tabW * static_cast<float>(index), px.y, tabW, m_tabHeight * px.h};
	}
	float x = m_effRect.x;
	for (size_t i = 0; i < index; ++i) x += m_tabWidths[i];
	return {x, m_effRect.y, m_tabWidths[index], m_tabHeight * m_effRect.h};
}

gfx::Rect TabControl::PageRect() const {
	const gfx::Rect base = m_effRect.w > 0.0f ? m_effRect : Pixel();
	const float stripH = m_tabHeight * base.h;
	return {base.x, base.y + stripH, base.w, base.h - stripH};
}

// The pages (and everything on them) are walked by the tree before this runs,
// so the strip only gets the mouse when nothing on the page claimed it.
void TabControl::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hover = -1;
	if (ctx.IsMouseConsumed()) return;
	const float mx = input->MouseX(), my = input->MouseY();
	for (size_t i = 0; i < m_tabs.size(); ++i) {
		if (!TabRect(i).Contains(mx, my)) continue;
		m_hover = static_cast<int>(i);
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_active = static_cast<int>(i);
		break;
	}
}

// Frame and strip only — the active page and its widgets are children, drawn
// by the tree straight after this.
void TabControl::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();

	// Page frame first so the active tab can open into it.
	const gfx::Rect page = PageRect();
	const bool skinned = PanelSkin(ctx) != nullptr;
	DrawPanelFace(ctx, batch, page);

	for (size_t i = 0; i < m_tabs.size(); ++i) {
		const gfx::Rect rect = TabRect(i);
		const bool active = static_cast<int>(i) == m_active;
		if (skinned) {
			// Skinned tabs are button faces riding the page's top edge; the
			// flat mode's border-erase trick can't merge a texture, so the
			// active tab reads by its held state + accent label instead.
			DrawButtonFace(batch, font, rect, "", theme,
						   static_cast<int>(i) == m_hover, active, true,
						   ctx.GetSkin());
		} else {
			batch.DrawRect(rect, active ? theme.panel
										: (static_cast<int>(i) == m_hover
											   ? theme.controlHot
											   : theme.control));
			DrawBorder(batch, rect, theme.panelBorder);
			if (active) // erase the tab's bottom edge and the page's top border
				batch.DrawRect({rect.x + 1, rect.y + rect.h - 1, rect.w - 2, 2},
							   theme.panel);
		}

		const std::string& label = m_tabs[i].label;
		const float textW = font.MeasureWidth(label);
		font.Draw(batch, label, rect.x + (rect.w - textW) * 0.5f,
				  rect.y + (rect.h - font.Height()) * 0.5f,
				  active ? theme.accent : theme.text);
	}
}

// --- Repeater ------------------------------------------------------------

// Grow the pool to the live count, then place and reveal exactly that many.
// Runs before the tree lays the children out, so the bounds set here are the
// ones they resolve with this frame. Growing BUILDS widgets, so a repeater whose
// count can rise in play is Warm()ed to its ceiling when it is made (the party
// bar's effect strip, code-review C219); here growth is the cold path.
void Repeater::LayoutSelf(UIContext&) {
	m_live = m_count ? m_count() : 0;
	while (Children().size() < m_live) {
		std::unique_ptr<Widget> child = m_factory(Children().size());
		if (!child) break; // factory declined — don't spin
		AddChild(std::move(child));
	}
	m_live = std::min(m_live, Children().size());
	const auto& kids = Children();
	for (size_t i = 0; i < kids.size(); ++i) {
		Widget& child = *kids[i];
		child.visible = i < m_live;
		if (child.visible && m_place) child.bounds = m_place(i);
	}
}

void Repeater::Warm(size_t n) {
	while (Children().size() < n) {
		std::unique_ptr<Widget> child = m_factory(Children().size());
		if (!child) break;
		child->visible = false; // LayoutSelf reveals it once the count reaches it
		AddChild(std::move(child));
	}
}

// --- shared close button -------------------------------------------------

gfx::Rect CloseButtonRect(const gfx::Rect& panel) {
	// A small square button just inside the panel's top-right corner. The icon
	// self-squares to the box's min dimension, so ~square window fractions on a
	// typical widescreen keep it from stretching.
	constexpr float kMargin = 0.008f, kW = 0.026f, kH = 0.044f;
	return {panel.x + panel.w - kW - kMargin, panel.y + kMargin, kW, kH};
}

Button* AddCloseButton(UIContext& ui, const gfx::Rect& panel,
					   const gfx::Texture* icon, std::function<void()> onClose) {
	Button* b = ui.Add<Button>(CloseButtonRect(panel), "x", std::move(onClose));
	b->icon = icon; // the text "x" shows only if the asset is missing
	return b;
}

Button* AddCloseButton(Widget& slot, const gfx::Texture* icon,
					   std::function<void()> onClose) {
	// Fills the slot; Button's icon path squares the icon to the smaller side
	// and centres it, so the slot only has to be big enough.
	Button* b = slot.Add<Button>(gfx::Rect{0, 0, 1, 1}, "x", std::move(onClose));
	b->icon = icon;
	return b;
}

const Font& DialogTitleFont(const UIContext& ctx) {
	// Scaled off the context's own authored size so it still tracks the window
	// like everything else — and off DesignHeight rather than GetFont().Height()
	// because the library applies the role's optical scale inside Get (see
	// UIContext::DesignHeight); the two agree for a Body root and diverge for
	// any other.
	return ctx.FontAt(ctx.RootRole(), ctx.DesignHeight() * kDialogTitleScale);
}

const Font& DialogTextFont(const UIContext& ctx) {
	return ctx.FontAt(ctx.RootRole(), ctx.DesignHeight() * kDialogTextScale);
}

} // namespace dungeon::ui
