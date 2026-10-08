// ============================================================================
// UI/Controls.cpp - the shared drawing helpers (faces, borders, fitted
// and carved text, tooltips) and the plain controls: Panel, Separator, Label,
// TextOutput, Button, and the shared close button. The other families are
// Controls_Fields.cpp (Checkbox, Slider, ColorPicker, KeyBind, TextField),
// Controls_Popups.cpp (DropDown, ContextMenu), Controls_Lists.cpp (SlotList,
// MenuList) and Controls_Containers.cpp (ScrollArea, TabControl, Repeater).
// Split by widget family by code-review C127; declared in UI/Controls.h.
// ============================================================================
#include "UI/Controls.h"
#include "UI/Controls_Detail.h"
#include "UI/Units.h"

#include <algorithm>
#include <cmath>

namespace dungeon::ui {

namespace detail {

// See Controls_Detail.h.
const Skin* PanelSkin(const UIContext& ctx) {
	const Skin* skin = ctx.GetSkin();
	return skin && skin->panel.texture ? skin : nullptr;
}

} // namespace detail

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

namespace detail {

// See Controls_Detail.h.
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

} // namespace detail

void DrawSwatch(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Swatch& swatch) {
	if (swatch.icon)
		batch.DrawSprite(rect, {0, 0, 1, 1}, *swatch.icon, {1, 1, 1, 1});
	else if (swatch.color.w > 0.0f)
		batch.DrawRect(rect, swatch.color);
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
	DrawButtonFace(batch, TextFont(), px, faceIcon ? std::string_view{} : std::string_view{text},
				   ctx.GetTheme(),
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
					std::string_view label, const Theme& theme, bool hot,
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

FittedFace DrawFittedButtonFace(gfx::SpriteBatch& batch, const Font& font,
								const gfx::Rect& rect, std::string_view label, float room,
								const Theme& theme, bool hot, bool held, bool enabled,
								const Skin* skin) {
	// The face with no label, then the fitted label centred where
	// DrawButtonFace would have put the whole one.
	DrawButtonFace(batch, font, rect, {}, theme, hot, held, enabled, skin);
	bool trimmed = false;
	const std::string_view fit = FitText(font, label, room, &trimmed);
	const float mark = trimmed ? font.MeasureWidth(kTrimMark) : 0.0f;
	const float fitW = font.MeasureWidth(fit);
	const float x = rect.x + (rect.w - (fitW + mark)) * 0.5f;
	const float y = rect.y + (rect.h - font.Height()) * 0.5f;
	const Vec4& ink = enabled ? theme.text : theme.textDim;
	// What is painted is what is returned: a report quotes these very views.
	FittedFace drawn;
	if (trimmed) drawn.cut = std::max(0.0f, font.MeasureWidth(label) - room);
	if (!trimmed || mark <= room) { // not even ".." fits: paint nothing (DrawFittedText's rule)
		font.Draw(batch, fit, x, y, ink);
		drawn.text = fit;
		if (trimmed) {
			font.Draw(batch, kTrimMark, x + fitW, y, ink);
			drawn.mark = kTrimMark;
		}
	}
	return drawn;
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
