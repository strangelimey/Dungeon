// ============================================================================
// UI/ScrollBar.cpp - see ScrollBar.h.
// ============================================================================
#include "UI/ScrollBar.h"

#include "Platform/Input.h"
#include "UI/Controls.h"
#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::ui {

ScrollBar::Span ScrollBar::SpanIn(const gfx::Rect& box, float inset, float rem, float view,
								  float maxScroll) {
	const float w = kWidthRem * rem;
	return {{box.x + box.w - w - inset, box.y + inset, w, box.h - 2.0f * inset}, view,
			maxScroll, kMinThumbRem * rem};
}

void ScrollBar::Clamp(float maxScroll) {
	m_offset = std::clamp(m_offset, 0.0f, std::max(0.0f, maxScroll));
}

gfx::Rect ScrollBar::Thumb(const Span& span) const {
	const gfx::Rect& track = span.track;
	const float h = std::max(track.h * span.view / (span.view + span.maxScroll), span.minThumb);
	const float t = span.maxScroll > 0.0f ? m_offset / span.maxScroll : 0.0f;
	return {track.x, track.y + (track.h - h) * t, track.w, h};
}

bool ScrollBar::UpdatePointer(const Input& input, const Span& span, bool mayTake) {
	m_hot = false;
	if (!span.Scrolls()) {
		m_dragging = false;
		return false;
	}
	if (m_dragging && !input.IsMouseDown(MouseButton::Left)) m_dragging = false;
	if (!mayTake && !m_dragging) return false;
	const gfx::Rect thumb = Thumb(span);
	const float my = input.MouseY();
	m_hot = thumb.Contains(input.MouseX(), my);
	if (m_hot && input.WasMousePressed(MouseButton::Left)) {
		m_dragging = true;
		m_grab = my - thumb.y;
	}
	if (m_dragging) {
		const float range = span.track.h - thumb.h;
		if (range > 0.0f)
			m_offset = std::clamp((my - m_grab - span.track.y) / range * span.maxScroll, 0.0f,
								  span.maxScroll);
	}
	return m_hot || m_dragging;
}

void ScrollBar::Wheel(float delta, float step, float maxScroll) {
	m_offset = std::clamp(m_offset - delta * step, 0.0f, std::max(0.0f, maxScroll));
}

void ScrollBar::Release() {
	m_hot = false;
	m_dragging = false;
}

void ScrollBar::Draw(gfx::SpriteBatch& batch, const Theme& theme, const Span& span) const {
	if (!span.Scrolls()) return;
	batch.DrawRect(span.track, theme.control);
	const gfx::Rect thumb = Thumb(span);
	batch.DrawRect(thumb, m_dragging || m_hot ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, thumb, theme.panelBorder);
}

} // namespace dungeon::ui
