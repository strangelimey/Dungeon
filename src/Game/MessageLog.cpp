// ============================================================================
// Game/MessageLog.cpp — see MessageLog.h for the behavior overview.
// ============================================================================
#include "Game/MessageLog.h"

#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>

namespace dungeon::game {

namespace {
constexpr size_t kMaxLines = 200;
constexpr float kHold = 6.0f;          // seconds a message stays fully opaque
constexpr float kFade = 5.0f;          // seconds it then takes to fade out
constexpr float kShrinkDelay = 0.5f;   // pause before collapsing once unhovered
constexpr float kPad = 6.0f;           // inner text padding (pixels, like other controls)
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

void MessageLog::AddLine(std::string line) {
	m_msgs.push_back({std::move(line), 0.0f});
	while (m_msgs.size() > kMaxLines) m_msgs.pop_front();
	m_scroll = 0.0f; // snap to the newest line
}

void MessageLog::Clear() {
	m_msgs.clear();
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

gfx::Rect MessageLog::RestoreRect(ui::UIContext& ctx) const {
	ui::Font& font = ctx.GetFont();
	const float bh = font.LineAdvance() + 10.0f;
	const float bw = font.MeasureWidth(restoreLabel) + 24.0f;
	return {8.0f, ctx.Height() - bh - 6.0f, bw, bh}; // bottom-left, where the footer sat
}

void MessageLog::Update(ui::UIContext& ctx) {
	// Height targets track the live font, so they scale with the window.
	ui::Font& font = ctx.GetFont();
	const float lineH = font.LineAdvance();
	m_collapsedFrac = (kCollapsedLines * lineH + 2.0f * kPad) / ctx.Height();
	m_expandedFrac = std::min(kMaxExpandedFrac,
							  (kExpandedLines * lineH + 2.0f * kPad) / ctx.Height());
	if (m_heightFrac <= 0.0f) m_heightFrac = m_collapsedFrac; // seed first frame

	m_hovered = false;
	m_restoreHot = false;

	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	// While the footer is faded out only the restore button is live.
	const bool dormant = m_chromeAlpha < 0.5f && !m_expanded;
	if (dormant) {
		if (!ctx.IsMouseConsumed() && RestoreRect(ctx).Contains(mx, my)) {
			m_restoreHot = true;
			ctx.ConsumeMouse();
			if (input->WasMousePressed(MouseButton::Left)) {
				m_expanded = true; // reveal and expand straight to the history
				m_shrinkTimer = 0.0f;
				m_scroll = 0.0f;
			}
		}
		return;
	}

	// Footer is shown: hovering keeps it expanded; the wheel scrolls history.
	const gfx::Rect footer = FooterRect(ctx);
	if (!ctx.IsMouseConsumed() && footer.Contains(mx, my)) {
		m_hovered = true;
		ctx.ConsumeMouse();
		if (input->WheelDelta() != 0.0f) {
			const float innerH = footer.h - 2.0f * kPad;
			const float maxScroll = std::max(
				0.0f, static_cast<float>(m_msgs.size()) - innerH / lineH);
			m_scroll = std::clamp(m_scroll + input->WheelDelta() * 3.0f, 0.0f,
								  maxScroll);
		}
	}
}

void MessageLog::Tick(float dt) {
	// Hover holds it open; leaving starts the shrink countdown.
	if (m_hovered) {
		m_expanded = true;
		m_shrinkTimer = 0.0f;
	} else if (m_expanded) {
		m_shrinkTimer += dt;
		if (m_shrinkTimer >= kShrinkDelay) m_expanded = false;
	}

	// Messages age only while collapsed, so reading (expanded) freezes the fade.
	if (!m_expanded)
		for (Msg& msg : m_msgs) msg.age += dt;

	// The footer shows while expanded or while any message is still visible;
	// otherwise it fades out (cross-fading into the restore button).
	float maxAlpha = 0.0f;
	for (const Msg& msg : m_msgs) maxAlpha = std::max(maxAlpha, MsgAlpha(msg));
	const float chromeTarget = (m_expanded || maxAlpha > 0.01f) ? 1.0f : 0.0f;
	m_chromeAlpha = Approach(m_chromeAlpha, chromeTarget, kAlphaRate, dt);

	const float heightTarget = m_expanded ? m_expandedFrac : m_collapsedFrac;
	m_heightFrac = Approach(m_heightFrac, heightTarget, kHeightRate, dt);
}

void MessageLog::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	const float ca = m_chromeAlpha;

	if (ca > 0.02f) {
		const gfx::Rect footer = FooterRect(ctx);
		Vec4 bg = theme.panel;
		bg.w *= ca;
		batch.DrawRect(footer, bg);
		Vec4 border = theme.panelBorder;
		border.w *= ca;
		ui::DrawBorder(batch, footer, border);

		const gfx::Rect inner{footer.x + kPad, footer.y + kPad,
							  footer.w - 2.0f * kPad, footer.h - 2.0f * kPad};
		batch.SetScissor(&inner);
		const float lineH = font.LineAdvance();
		// Newest at the bottom, offset upward by the scroll.
		const int last =
			static_cast<int>(m_msgs.size()) - 1 - static_cast<int>(m_scroll);
		float y = inner.y + inner.h - lineH;
		for (int i = last; i >= 0 && y + lineH > inner.y; --i) {
			Vec4 col = theme.text;
			col.w *= ca * MsgAlpha(m_msgs[static_cast<size_t>(i)]);
			font.Draw(batch, m_msgs[static_cast<size_t>(i)].text, inner.x, y, col);
			y -= lineH;
		}
		batch.SetScissor(nullptr);
	}

	// Restore button cross-fades in as the footer fades out.
	const float ba = 1.0f - ca;
	if (ba > 0.02f) {
		const gfx::Rect btn = RestoreRect(ctx);
		Vec4 bg = theme.panel;
		bg.w *= ba * (m_restoreHot ? 0.9f : 0.5f);
		batch.DrawRect(btn, bg);
		Vec4 border = theme.panelBorder;
		border.w *= ba * 0.7f;
		ui::DrawBorder(batch, btn, border);
		Vec4 col = theme.text;
		col.w *= ba;
		const float tw = font.MeasureWidth(restoreLabel);
		font.Draw(batch, restoreLabel, btn.x + (btn.w - tw) * 0.5f,
				  btn.y + (btn.h - font.Height()) * 0.5f, col);
	}
}

} // namespace dungeon::game
