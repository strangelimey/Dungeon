// ============================================================================
// Game/MessageLog.cpp — see MessageLog.h for the behavior overview.
// ============================================================================
#include "Game/MessageLog.h"

#include "UI/Controls.h" // ui::DrawBorder
#include "UI/Skin.h"     // ui::DrawFace

#include <algorithm>

namespace dungeon::game {

namespace {
// (kMaxLines is the ring's capacity and lives on the class — see MessageLog.h.)
constexpr float kHold = 6.0f;          // seconds a message stays fully opaque
constexpr float kFade = 5.0f;          // seconds it then takes to fade out
constexpr float kPadRem = 0.35f;       // inner text padding, in rem (UI/Units.h)
constexpr float kCollapsedLines = 2.0f;
constexpr float kExpandedLines = 11.0f;
constexpr float kMaxExpandedFrac = 0.5f; // never taller than half the screen
constexpr float kHeightRate = 12.0f;     // height ease (per second)
constexpr float kAlphaRate = 8.0f;       // opacity ease (per second)

// Frame-rate-independent exponential approach toward a target.
float Approach(float value, float target, float rate, float dt) {
	return value + (target - value) * std::min(1.0f, dt * rate);
}
} // namespace

void MessageLog::AddLine(std::string_view line, std::optional<Vec4> color) {
	// Claim the next slot, evicting the oldest once the ring is full. Nothing
	// is constructed or destroyed here — the slot already exists and its text
	// buffer is inline, so this is a copy into storage the log already owns.
	Msg& slot = m_ring[(m_head + m_count) % kMaxLines];
	if (m_count == kMaxLines)
		m_head = (m_head + 1) % kMaxLines; // overwrote the oldest
	else
		++m_count;
	slot.text.Assign(line);
	slot.color = color;
	slot.age = 0.0f;
	m_scroll = 0.0f; // snap to the newest line
}

void MessageLog::Clear() {
	// Drops the history without releasing the ring: the slots stay put and are
	// simply forgotten, so clearing costs nothing and refilling costs nothing.
	m_head = 0;
	m_count = 0;
	m_scroll = 0.0f;
}

float MessageLog::MsgAlpha(const Msg& msg) const {
	if (m_expanded) return 1.0f; // full opacity while the player is reading
	if (msg.age < kHold) return 1.0f;
	if (msg.age < kHold + kFade) return 1.0f - (msg.age - kHold) / kFade;
	return 0.0f;
}

gfx::Rect MessageLog::FooterRect(ui::UIContext& ctx) const {
	const float w = ctx.Width();
	const float h = ctx.Height();
	const float fh = m_heightFrac * h;
	return {0.0f, h - fh, w, fh}; // full width, flush to the bottom edge
}

const std::string& MessageLog::CornerLabel(size_t i) const {
	if (i == 0) return restoreLabel;
	const CornerButton& b = cornerButtons[i - 1];
	return b.alt && !b.altLabel.empty() ? b.altLabel : b.label;
}

gfx::Rect MessageLog::CornerRect(ui::UIContext& ctx, size_t i) const {
	const ui::Font& font = TextFont();
	const float bh = font.LineAdvance() + Rem(0.6f);
	const float gap = Rem(0.3f);
	// Bottom-left, where the footer sat, then rightward. A button is as wide as
	// the LONGER of its two captions, so Rest -> Wake never moves its neighbour.
	float x = Rem(0.5f);
	for (size_t k = 0;; ++k) {
		float textW = font.MeasureWidth(k == 0 ? restoreLabel : cornerButtons[k - 1].label);
		if (k > 0) textW = std::max(textW, font.MeasureWidth(cornerButtons[k - 1].altLabel));
		const float bw = textW + Rem(1.4f);
		if (k == i) return {x, ctx.Height() - bh - Rem(0.35f), bw, bh};
		x += bw + gap;
	}
}

gfx::Rect MessageLog::CornerRow(ui::UIContext& ctx) const {
	const gfx::Rect first = CornerRect(ctx, 0);
	const gfx::Rect last = CornerRect(ctx, CornerCount() - 1);
	return {first.x, first.y, last.x + last.w - first.x, first.h};
}

// Height targets track the live font (so they scale with the window), then the
// rect this widget actually occupies is written back into `bounds`: the footer
// while it shows, the small restore button once it has faded out. Both are
// screen-anchored, so the fractions are of the window — which is this widget's
// parent. During the brief cross-fade both are painted; only one of them ever
// takes input, and that is the one `bounds` follows.
void MessageLog::LayoutSelf(ui::UIContext& ctx) {
	const ui::Font& font = TextFont();
	const float lineH = font.LineAdvance();
	const float w = ctx.Width(), h = ctx.Height();
	const float pad = Rem(kPadRem);
	m_collapsedFrac = (kCollapsedLines * lineH + 2.0f * pad) / h;
	m_expandedFrac =
		std::min(kMaxExpandedFrac, (kExpandedLines * lineH + 2.0f * pad) / h);
	if (m_heightFrac <= 0.0f) m_heightFrac = m_collapsedFrac; // seed first frame

	if (Dormant()) {
		const gfx::Rect btn = CornerRow(ctx);
		bounds = {btn.x / w, btn.y / h, btn.w / w, btn.h / h};
	} else {
		bounds = {0.0f, 1.0f - m_heightFrac, 1.0f, m_heightFrac};
	}
}

void MessageLog::UpdateSelf(ui::UIContext& ctx) {
	const ui::Font& font = TextFont();
	const float lineH = font.LineAdvance();
	m_hot = -1;

	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	// THE LOG BUTTON is the only way the history opens or closes (Michael,
	// ui-updates: hovering used to expand it, and it got in the way). It sits at
	// the bottom-left in every state - alone once the footer has faded, inside
	// the footer's corner while it shows - and toggles. The rest of the corner
	// row behaves the same way and calls its own action.
	if (!ctx.IsMouseConsumed())
		for (size_t i = 0; i < CornerCount(); ++i) {
			if (!CornerRect(ctx, i).Contains(mx, my)) continue;
			m_hot = static_cast<int>(i);
			ctx.ConsumeMouse();
			if (input->WasMousePressed(MouseButton::Left)) {
				if (i == 0) {
					m_expanded = !m_expanded;
					m_scroll = 0.0f;
				} else if (cornerButtons[i - 1].onClick) {
					cornerButtons[i - 1].onClick();
				}
			}
			return;
		}
	if (Dormant()) return; // faded out: the button is all there is

	// Footer is shown: it claims the pointer (it paints there), and while open
	// the wheel scrolls the history.
	const gfx::Rect footer = FooterRect(ctx);
	if (!ctx.IsMouseConsumed() && footer.Contains(mx, my)) {
		ctx.ConsumeMouse();
		if (m_expanded && input->WheelDelta() != 0.0f && !ctx.IsWheelConsumed()) {
			const float innerH = footer.h - 2.0f * Rem(kPadRem);
			const float maxScroll = std::max(
				0.0f, static_cast<float>(Count()) - innerH / lineH);
			m_scroll = std::clamp(m_scroll + input->WheelDelta() * 3.0f, 0.0f,
								  maxScroll);
			ctx.ConsumeWheel();
		}
	}
}

void MessageLog::Tick(float dt) {
	// Messages age only while collapsed, so reading (expanded) freezes the fade.
	if (!m_expanded)
		for (size_t i = 0; i < Count(); ++i) At(i).age += dt;

	// The footer shows while expanded or while any message is still visible;
	// otherwise it fades out (cross-fading into the restore button).
	float maxAlpha = 0.0f;
	for (size_t i = 0; i < Count(); ++i)
		maxAlpha = std::max(maxAlpha, MsgAlpha(At(i)));
	const float chromeTarget = (m_expanded || maxAlpha > 0.01f) ? 1.0f : 0.0f;
	m_chromeAlpha = Approach(m_chromeAlpha, chromeTarget, kAlphaRate, dt);

	const float heightTarget = m_expanded ? m_expandedFrac : m_collapsedFrac;
	m_heightFrac = Approach(m_heightFrac, heightTarget, kHeightRate, dt);
}

void MessageLog::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const float ca = m_chromeAlpha;

	if (ca > 0.02f) {
		const gfx::Rect footer = FooterRect(ctx);
		const ui::Skin* skin = ctx.GetSkin();
		if (skin && skin->panel.texture) {
			// A stone panel like every other piece of chrome, faded as a whole.
			ui::DrawFace(batch, footer, *skin, ui::Face::Panel, {1, 1, 1, theme.panel.w * ca});
		} else {
			Vec4 bg = theme.panel;
			bg.w *= ca;
			batch.DrawRect(footer, bg);
			Vec4 border = theme.panelBorder;
			border.w *= ca;
			ui::DrawBorder(batch, footer, border);
		}

		// The text starts right of the corner row, which keeps the corner.
		const gfx::Rect btn = CornerRow(ctx);
		const float left = btn.x + btn.w + Rem(kPadRem);
		const gfx::Rect inner{left, footer.y + Rem(kPadRem), footer.w - left - Rem(kPadRem),
							  footer.h - 2.0f * Rem(kPadRem)};
		const ui::ScopedClip clip(batch, inner);
		const float lineH = font.LineAdvance();
		// Newest at the bottom, offset upward by the scroll.
		const int last =
			static_cast<int>(Count()) - 1 - static_cast<int>(m_scroll);
		float y = inner.y + inner.h - lineH;
		for (int i = last; i >= 0 && y + lineH > inner.y; --i) {
			const Msg& msg = At(static_cast<size_t>(i));
			Vec4 col = msg.color.value_or(theme.text);
			col.w *= ca * MsgAlpha(msg);
			font.Draw(batch, msg.text, inner.x, y, col);
			y -= lineH;
		}
	}

	// The corner row, always: translucent on its own once the footer has faded,
	// solid in the footer's corner while it shows. The Log button is pressed
	// while the history is open.
	const ui::Skin* skin = ctx.GetSkin();
	for (size_t i = 0; i < CornerCount(); ++i) {
		const bool hot = m_hot == static_cast<int>(i);
		const bool down = i == 0 && m_expanded;
		const float ba = std::max(ca, hot ? 1.0f : 0.75f);
		const gfx::Rect btn = CornerRect(ctx, i);
		if (skin && skin->button.texture) {
			ui::DrawFace(batch, btn, *skin, down ? ui::Face::ButtonDown : ui::Face::Button,
						 {1, 1, 1, ba});
		} else {
			Vec4 bg = theme.panel;
			bg.w *= ba * (hot || down ? 0.9f : 0.5f);
			batch.DrawRect(btn, bg);
			Vec4 border = down ? theme.accent : theme.panelBorder;
			border.w *= ba * 0.7f;
			ui::DrawBorder(batch, btn, border);
		}
		Vec4 col = down ? theme.accent : theme.text;
		col.w *= ba;
		const std::string& label = CornerLabel(i);
		const float tw = font.MeasureWidth(label);
		font.Draw(batch, label, btn.x + (btn.w - tw) * 0.5f,
				  btn.y + (btn.h - font.Height()) * 0.5f + (down ? 1.0f : 0.0f), col);
	}
}

} // namespace dungeon::game
