#include "UI/Controls.h"

#include <algorithm>
#include <format>

namespace dungeon::ui {

void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color) {
	batch.DrawRect({rect.x, rect.y, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y, 1, rect.h}, color);
	batch.DrawRect({rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
}

// --- Panel -------------------------------------------------------------

void Panel::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	batch.DrawRect(bounds, ctx.GetTheme().panel);
	DrawBorder(batch, bounds, ctx.GetTheme().panelBorder);
}

// --- Label -------------------------------------------------------------

void Label::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	ctx.GetFont().Draw(batch, text, bounds.x, bounds.y, dim ? theme.textDim : theme.text);
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

void TextOutput::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	if (bounds.Contains(input->MouseX(), input->MouseY()) && input->WheelDelta() != 0) {
		m_scroll += input->WheelDelta() * 3.0f;
		const float maxScroll =
			std::max(0.0f, static_cast<float>(m_lines.size()) -
							   bounds.h / ctx.GetFont().LineAdvance());
		m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
		ctx.ConsumeMouse();
	}
}

void TextOutput::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();

	batch.DrawRect(bounds, theme.panel);
	DrawBorder(batch, bounds, theme.panelBorder);

	const float lineHeight = font.LineAdvance();
	const float pad = 6.0f;
	const gfx::Rect inner{bounds.x + pad, bounds.y + pad, bounds.w - 2 * pad,
						  bounds.h - 2 * pad};
	batch.SetScissor(&inner);

	const int visibleLines = static_cast<int>(inner.h / lineHeight) + 1;
	// Index of the last line shown, offset by scroll (0 = newest).
	const int last = static_cast<int>(m_lines.size()) - 1 - static_cast<int>(m_scroll);
	float y = inner.y + inner.h - lineHeight;
	for (int i = last; i >= 0 && i > last - visibleLines; --i) {
		font.Draw(batch, m_lines[static_cast<size_t>(i)], inner.x, y, theme.text);
		y -= lineHeight;
	}
	batch.SetScissor(nullptr);
}

// --- Button --------------------------------------------------------------

void Button::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() && bounds.Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && onClick) onClick();
		m_held = false;
	}
}

void Button::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Vec4& fill = m_held ? theme.controlActive : (m_hot ? theme.controlHot : theme.control);
	batch.DrawRect(bounds, fill);
	DrawBorder(batch, bounds, theme.panelBorder);

	Font& font = ctx.GetFont();
	const float textW = font.MeasureWidth(text);
	font.Draw(batch, text, bounds.x + (bounds.w - textW) * 0.5f,
			  bounds.y + (bounds.h - font.Height()) * 0.5f, theme.text);
}

// --- Slider --------------------------------------------------------------

void Slider::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const bool hovered =
		!ctx.IsMouseConsumed() && bounds.Contains(input->MouseX(), input->MouseY());
	if (hovered && input->WasMousePressed(MouseButton::Left)) m_dragging = true;
	if (!input->IsMouseDown(MouseButton::Left)) m_dragging = false;
	if (hovered || m_dragging) ctx.ConsumeMouse();

	if (m_dragging) {
		const float t =
			std::clamp((input->MouseX() - bounds.x) / std::max(bounds.w, 1.0f), 0.0f, 1.0f);
		const float value = m_min + t * (m_max - m_min);
		if (value != m_value) {
			m_value = value;
			RefreshDisplay();
			if (onChange) onChange(m_value);
		}
	}
}

void Slider::RefreshDisplay() {
	m_display = std::format("{}: {:.2f}", label, m_value);
}

void Slider::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();

	// Label above the track.
	font.Draw(batch, m_display, bounds.x, bounds.y - font.LineAdvance(), theme.textDim);

	// Track.
	const float trackH = 4.0f;
	const float trackY = bounds.y + (bounds.h - trackH) * 0.5f;
	batch.DrawRect({bounds.x, trackY, bounds.w, trackH}, theme.control);

	// Filled portion + thumb.
	const float t = (m_value - m_min) / std::max(m_max - m_min, 1e-6f);
	batch.DrawRect({bounds.x, trackY, bounds.w * t, trackH}, theme.accent);
	const float thumbX = bounds.x + bounds.w * t - 5.0f;
	batch.DrawRect({thumbX, bounds.y, 10.0f, bounds.h},
				   m_dragging ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, {thumbX, bounds.y, 10.0f, bounds.h}, theme.panelBorder);
}

// --- DropDown ------------------------------------------------------------

gfx::Rect DropDown::ItemRect(size_t index) const {
	return {bounds.x, bounds.y + bounds.h * static_cast<float>(index + 1), bounds.w,
			bounds.h};
}

void DropDown::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	if (m_open) {
		// The open popup owns the mouse entirely.
		m_hoverItem = -1;
		for (size_t i = 0; i < items.size(); ++i) {
			if (!ItemRect(i).Contains(input->MouseX(), input->MouseY())) continue;
			m_hoverItem = static_cast<int>(i);
			if (input->WasMousePressed(MouseButton::Left)) {
				m_selected = static_cast<int>(i);
				m_open = false;
				ctx.ConsumeMouse();
				if (onSelect) onSelect(m_selected);
				return;
			}
		}
		if (input->WasMousePressed(MouseButton::Left)) m_open = false;
		ctx.ConsumeMouse();
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && bounds.Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_open = true;
	}
}

void DropDown::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();

	batch.DrawRect(bounds, m_hot || m_open ? theme.controlHot : theme.control);
	DrawBorder(batch, bounds, theme.panelBorder);
	const std::string& current =
		(m_selected >= 0 && m_selected < static_cast<int>(items.size()))
			? items[static_cast<size_t>(m_selected)]
			: "";
	font.Draw(batch, current, bounds.x + 8,
			  bounds.y + (bounds.h - font.Height()) * 0.5f, theme.text);
	// Arrow indicator.
	font.Draw(batch, m_open ? "^" : "v", bounds.x + bounds.w - 18,
			  bounds.y + (bounds.h - font.Height()) * 0.5f, theme.accent);
}

void DropDown::DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	for (size_t i = 0; i < items.size(); ++i) {
		const gfx::Rect rect = ItemRect(i);
		const bool hovered = static_cast<int>(i) == m_hoverItem;
		batch.DrawRect(rect, hovered ? theme.controlHot : theme.control);
		DrawBorder(batch, rect, theme.panelBorder);
		font.Draw(batch, items[i], rect.x + 8, rect.y + (rect.h - font.Height()) * 0.5f,
				  static_cast<int>(i) == m_selected ? theme.accent : theme.text);
	}
}

// --- MenuList --------------------------------------------------------------

void MenuList::AddItem(std::string label, std::function<void()> onActivate) {
	m_items.push_back({std::move(label), std::move(onActivate)});
}

gfx::Rect MenuList::ItemRect(size_t index) const {
	return {bounds.x, bounds.y + m_itemHeight * static_cast<float>(index), bounds.w,
			m_itemHeight - 8.0f}; // 8px gap between entries
}

void MenuList::MoveSelection(int delta) {
	if (m_items.empty()) return;
	const int count = static_cast<int>(m_items.size());
	m_selected = (m_selected + delta + count) % count; // wrap around
}

void MenuList::Activate() {
	if (m_selected >= 0 && m_selected < static_cast<int>(m_items.size())) {
		const auto& onActivate = m_items[static_cast<size_t>(m_selected)].onActivate;
		if (onActivate) onActivate();
	}
}

void MenuList::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	// Mouse: hovering selects, clicking activates.
	if (!ctx.IsMouseConsumed()) {
		for (size_t i = 0; i < m_items.size(); ++i) {
			if (!ItemRect(i).Contains(input->MouseX(), input->MouseY())) continue;
			m_selected = static_cast<int>(i);
			ctx.ConsumeMouse();
			if (input->WasMousePressed(MouseButton::Left)) Activate();
			break;
		}
	}

	// Keyboard and gamepad: move the selection, Enter/Space/A activates.
	constexpr int kVkUp = 0x26, kVkDown = 0x28, kVkReturn = 0x0D, kVkSpace = 0x20;
	if (input->WasKeyPressed(kVkUp) || input->WasKeyPressed('W') ||
		input->WasPadPressed(PadButton::Up))
		MoveSelection(-1);
	if (input->WasKeyPressed(kVkDown) || input->WasKeyPressed('S') ||
		input->WasPadPressed(PadButton::Down))
		MoveSelection(+1);
	if (input->WasKeyPressed(kVkReturn) || input->WasKeyPressed(kVkSpace) ||
		input->WasPadPressed(PadButton::A))
		Activate();
}

void MenuList::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();

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
			font.Draw(batch, ">", rect.x + 16, textY, theme.accent);
			font.Draw(batch, "<", rect.x + rect.w - 24, textY, theme.accent);
		}
	}
}

} // namespace dungeon::ui
