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
	batch.DrawRect(Pixel(), ctx.GetTheme().panel);
	DrawBorder(batch, Pixel(), ctx.GetTheme().panelBorder);
}

// --- Label -------------------------------------------------------------

void Label::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	ctx.GetFont().Draw(batch, text, Pixel().x, Pixel().y,
					   dim ? theme.textDim : theme.text);
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
	if (Pixel().Contains(input->MouseX(), input->MouseY()) && input->WheelDelta() != 0) {
		m_scroll += input->WheelDelta() * 3.0f;
		const float maxScroll =
			std::max(0.0f, static_cast<float>(m_lines.size()) -
							   Pixel().h / ctx.GetFont().LineAdvance());
		m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
		ctx.ConsumeMouse();
	}
}

void TextOutput::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, theme.panel);
	DrawBorder(batch, px, theme.panelBorder);

	const float lineHeight = font.LineAdvance();
	const float pad = 6.0f;
	const gfx::Rect inner{px.x + pad, px.y + pad, px.w - 2 * pad, px.h - 2 * pad};
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
	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
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
	const gfx::Rect& px = Pixel();
	const Vec4& fill = m_held ? theme.controlActive : (m_hot ? theme.controlHot : theme.control);
	batch.DrawRect(px, fill);
	DrawBorder(batch, px, theme.panelBorder);

	Font& font = ctx.GetFont();
	const float textW = font.MeasureWidth(text);
	font.Draw(batch, text, px.x + (px.w - textW) * 0.5f,
			  px.y + (px.h - font.Height()) * 0.5f, theme.text);
}

// --- Slider --------------------------------------------------------------

void Slider::Update(UIContext& ctx) {
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
	m_display = std::format("{}: {:.2f}", label, m_value);
}

void Slider::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	// Label above the track.
	font.Draw(batch, m_display, px.x, px.y - font.LineAdvance(), theme.textDim);

	// Track.
	const float trackH = 4.0f;
	const float trackY = px.y + (px.h - trackH) * 0.5f;
	batch.DrawRect({px.x, trackY, px.w, trackH}, theme.control);

	// Filled portion + thumb.
	const float t = (m_value - m_min) / std::max(m_max - m_min, 1e-6f);
	batch.DrawRect({px.x, trackY, px.w * t, trackH}, theme.accent);
	const float thumbX = px.x + px.w * t - 5.0f;
	batch.DrawRect({thumbX, px.y, 10.0f, px.h},
				   m_dragging ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, {thumbX, px.y, 10.0f, px.h}, theme.panelBorder);
}

// --- DropDown ------------------------------------------------------------

gfx::Rect DropDown::ItemRect(size_t index) const {
	const gfx::Rect& px = Pixel();
	return {px.x, px.y + px.h * static_cast<float>(index + 1), px.w, px.h};
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

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_open = true;
	}
}

void DropDown::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, m_hot || m_open ? theme.controlHot : theme.control);
	DrawBorder(batch, px, theme.panelBorder);
	const std::string& current =
		(m_selected >= 0 && m_selected < static_cast<int>(items.size()))
			? items[static_cast<size_t>(m_selected)]
			: "";
	font.Draw(batch, current, px.x + 8, px.y + (px.h - font.Height()) * 0.5f,
			  theme.text);
	// Arrow indicator.
	font.Draw(batch, m_open ? "^" : "v", px.x + px.w - 18,
			  px.y + (px.h - font.Height()) * 0.5f, theme.accent);
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

// --- ColorPicker -------------------------------------------------------------

namespace {

// Popup geometry (fixed pixels, like all control detail). Four channel rows,
// each: letter | track | 0..255 value.
constexpr float kPickerPopupW = 320.0f;
constexpr float kPickerPopupPad = 12.0f;
constexpr float kPickerRowPitch = 36.0f;
constexpr float kPickerRowH = 28.0f;
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

gfx::Rect PickerRow(const gfx::Rect& popup, int index) {
	return {popup.x + kPickerPopupPad,
			popup.y + kPickerPopupPad + kPickerRowPitch * static_cast<float>(index),
			popup.w - 2 * kPickerPopupPad, kPickerRowH};
}

gfx::Rect PickerTrack(const gfx::Rect& row) {
	return {row.x + 26.0f, row.y, row.w - 26.0f - 56.0f, row.h};
}

} // namespace

gfx::Rect ColorPicker::SwatchRect() const {
	const gfx::Rect& px = Pixel();
	const float w = std::min(64.0f, px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

gfx::Rect ColorPicker::PopupRect(const UIContext& ctx) const {
	const gfx::Rect swatch = SwatchRect();
	const float x = std::clamp(swatch.x + swatch.w - kPickerPopupW, 0.0f,
							   std::max(0.0f, ctx.Width() - kPickerPopupW));
	float y = swatch.y + swatch.h + 4.0f;
	if (y + kPickerPopupH > ctx.Height()) // no room below: open above instead
		y = swatch.y - 4.0f - kPickerPopupH;
	return {x, y, kPickerPopupW, kPickerPopupH};
}

void ColorPicker::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	if (m_open) {
		constexpr int kVkEscape = 0x1B;
		const gfx::Rect popup = PopupRect(ctx);
		if (m_dragChannel >= 0 && !input->IsMouseDown(MouseButton::Left))
			m_dragChannel = -1;
		if (m_dragChannel < 0 && input->WasMousePressed(MouseButton::Left)) {
			if (popup.Contains(mx, my)) {
				for (int i = 0; i < 4; ++i)
					if (PickerRow(popup, i).Contains(mx, my)) m_dragChannel = i;
			} else {
				m_open = false; // click anywhere else (incl. the swatch) closes
				if (onClose) onClose();
			}
		}
		if (m_open && input->WasKeyPressed(kVkEscape)) {
			m_dragChannel = -1;
			m_open = false;
			if (onClose) onClose();
		}
		if (m_dragChannel >= 0) {
			const gfx::Rect track = PickerTrack(PickerRow(popup, m_dragChannel));
			const float t =
				std::clamp((mx - track.x) / std::max(track.w, 1.0f), 0.0f, 1.0f);
			float& value = Channel(m_color, m_dragChannel);
			if (t != value) {
				value = t;
				if (onChange) onChange(m_color);
			}
		}
		ctx.ConsumeMouse(); // the open popup owns the mouse entirely
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && SwatchRect().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_open = true;
	}
}

void ColorPicker::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect swatch = SwatchRect();
	batch.DrawRect(swatch, {0, 0, 0, 1}); // opaque base so alpha reads as darkness
	batch.DrawRect(swatch, m_color);
	DrawBorder(batch, swatch, m_hot || m_open ? theme.accent : theme.panelBorder);
}

void ColorPicker::DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect popup = PopupRect(ctx);

	Vec4 background = theme.panel;
	background.w = 1.0f; // opaque so the page beneath doesn't bleed through
	batch.DrawRect(popup, background);
	DrawBorder(batch, popup, theme.panelBorder);

	static constexpr const char* kChannelNames[4] = {"R", "G", "B", "A"};
	static constexpr Vec4 kChannelTints[4] = {{0.9f, 0.3f, 0.3f, 1.0f},
											  {0.3f, 0.85f, 0.3f, 1.0f},
											  {0.35f, 0.55f, 1.0f, 1.0f},
											  {0.8f, 0.8f, 0.8f, 1.0f}};
	for (int i = 0; i < 4; ++i) {
		const gfx::Rect row = PickerRow(popup, i);
		const gfx::Rect track = PickerTrack(row);
		const float value = Channel(m_color, i);
		const float textY = row.y + (row.h - font.Height()) * 0.5f;

		font.Draw(batch, kChannelNames[i], row.x, textY, theme.textDim);

		const float trackH = 4.0f;
		const float trackY = track.y + (track.h - trackH) * 0.5f;
		batch.DrawRect({track.x, trackY, track.w, trackH}, theme.control);
		batch.DrawRect({track.x, trackY, track.w * value, trackH}, kChannelTints[i]);
		const gfx::Rect thumb{track.x + track.w * value - 4.0f, track.y + 2.0f, 8.0f,
							  track.h - 4.0f};
		batch.DrawRect(thumb,
					   m_dragChannel == i ? theme.controlActive : theme.controlHot);
		DrawBorder(batch, thumb, theme.panelBorder);

		font.Draw(batch, std::format("{}", static_cast<int>(value * 255.0f + 0.5f)),
				  track.x + track.w + 10.0f, textY, theme.text);
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
	const float w = std::min(200.0f, px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

void KeyBind::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	if (m_capturing) {
		constexpr int kVkEscape = 0x1B;
		if (input->WasMousePressed(MouseButton::Left) ||
			input->WasKeyPressed(kVkEscape)) {
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

void KeyBind::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect box = BoxRect();
	batch.DrawRect(box, m_capturing ? theme.controlActive
									: (m_hot ? theme.controlHot : theme.control));
	DrawBorder(batch, box, m_capturing || m_hot ? theme.accent : theme.panelBorder);

	const std::string& text = m_capturing ? capturePrompt : m_keyName;
	const float textW = font.MeasureWidth(text);
	font.Draw(batch, text, box.x + (box.w - textW) * 0.5f,
			  box.y + (box.h - font.Height()) * 0.5f,
			  m_capturing ? theme.accent : theme.text);
}

// --- MenuList --------------------------------------------------------------

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
			itemH - 8.0f}; // 8px gap between entries
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

	// Keyboard: arrows / W/S move the selection, Enter/Space activates.
	constexpr int kVkUp = 0x26, kVkDown = 0x28, kVkReturn = 0x0D, kVkSpace = 0x20;
	if (input->WasKeyPressed(kVkUp) || input->WasKeyPressed('W')) MoveSelection(-1);
	if (input->WasKeyPressed(kVkDown) || input->WasKeyPressed('S')) MoveSelection(+1);
	if (input->WasKeyPressed(kVkReturn) || input->WasKeyPressed(kVkSpace)) Activate();
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

// --- TabControl ------------------------------------------------------------

size_t TabControl::AddTab(std::string label) {
	m_tabs.push_back({std::move(label), {}});
	return m_tabs.size() - 1;
}

void TabControl::SetActiveTab(int index) {
	if (index >= 0 && index < static_cast<int>(m_tabs.size())) m_active = index;
}

gfx::Rect TabControl::TabRect(size_t index) const {
	const gfx::Rect& px = Pixel();
	const float tabW = px.w / static_cast<float>(std::max<size_t>(m_tabs.size(), 1));
	return {px.x + tabW * static_cast<float>(index), px.y, tabW, m_tabHeight * px.h};
}

gfx::Rect TabControl::PageRect() const {
	const gfx::Rect& px = Pixel();
	const float stripH = m_tabHeight * px.h;
	return {px.x, px.y + stripH, px.w, px.h - stripH};
}

float TabControl::ContentFraction(const Tab& tab) {
	float maxBottom = 1.0f;
	for (const auto& child : tab.children)
		if (child->visible)
			maxBottom = std::max(maxBottom, child->bounds.y + child->bounds.h);
	return maxBottom;
}

gfx::Rect TabControl::ScrollTrackRect(const gfx::Rect& page) const {
	const float barW = 10.0f;
	return {page.x + page.w - barW - 2.0f, page.y + 2.0f, barW, page.h - 4.0f};
}

gfx::Rect TabControl::ScrollThumbRect(const gfx::Rect& page, const Tab& tab,
									  float maxScroll) const {
	const gfx::Rect track = ScrollTrackRect(page);
	const float thumbH =
		std::max(track.h * page.h / (page.h + maxScroll), 24.0f);
	const float t = maxScroll > 0.0f ? tab.scroll / maxScroll : 0.0f;
	return {track.x, track.y + (track.h - thumbH) * t, track.w, thumbH};
}

void TabControl::Update(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	// Active page children first, in reverse add order — same topmost-first
	// claim on the mouse as UIContext itself. Children scrolled fully out of
	// the page get neither layout-visible input nor draw.
	if (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) {
		const gfx::Rect page = PageRect();
		Tab& tab = m_tabs[static_cast<size_t>(m_active)];
		const float maxScroll = (ContentFraction(tab) - 1.0f) * page.h;
		tab.scroll = std::clamp(tab.scroll, 0.0f, maxScroll);
		const gfx::Rect content{page.x, page.y - tab.scroll, page.w, page.h};

		for (auto it = tab.children.rbegin(); it != tab.children.rend(); ++it) {
			Widget& child = **it;
			if (!child.visible) continue;
			const float top = child.bounds.y * page.h - tab.scroll;
			const float bottom = (child.bounds.y + child.bounds.h) * page.h -
								 tab.scroll;
			if (bottom <= 0.0f || top >= page.h) continue;
			child.Layout(content);
			child.Update(ctx);
		}

		// Scrollbar after the children, so an open popup (which consumes the
		// mouse) can't be scrolled out from under the user.
		m_scrollHot = false;
		if (maxScroll > 0.0f) {
			const gfx::Rect track = ScrollTrackRect(page);
			const gfx::Rect thumb = ScrollThumbRect(page, tab, maxScroll);
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
						tab.scroll = std::clamp(
							(my - m_scrollGrab - track.y) / range * maxScroll,
							0.0f, maxScroll);
				}
				if (m_scrollHot || m_scrollDragging) ctx.ConsumeMouse();
			}

			// Mouse wheel anywhere over the page.
			if (!ctx.IsMouseConsumed() && page.Contains(mx, my) &&
				input->WheelDelta() != 0.0f) {
				tab.scroll = std::clamp(tab.scroll - input->WheelDelta() * 48.0f,
										0.0f, maxScroll);
				ctx.ConsumeMouse();
			}
		} else {
			m_scrollDragging = false;
		}
	}

	// Tab strip.
	m_hover = -1;
	if (ctx.IsMouseConsumed()) return;
	for (size_t i = 0; i < m_tabs.size(); ++i) {
		if (!TabRect(i).Contains(mx, my)) continue;
		m_hover = static_cast<int>(i);
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_active = static_cast<int>(i);
		break;
	}
}

void TabControl::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	Font& font = ctx.GetFont();

	// Page frame first so the active tab can open into it.
	const gfx::Rect page = PageRect();
	batch.DrawRect(page, theme.panel);
	DrawBorder(batch, page, theme.panelBorder);

	for (size_t i = 0; i < m_tabs.size(); ++i) {
		const gfx::Rect rect = TabRect(i);
		const bool active = static_cast<int>(i) == m_active;
		batch.DrawRect(rect, active ? theme.panel
									: (static_cast<int>(i) == m_hover ? theme.controlHot
																	  : theme.control));
		DrawBorder(batch, rect, theme.panelBorder);
		if (active) // erase the tab's bottom edge and the page's top border
			batch.DrawRect({rect.x + 1, rect.y + rect.h - 1, rect.w - 2, 2},
						   theme.panel);

		const std::string& label = m_tabs[i].label;
		const float textW = font.MeasureWidth(label);
		font.Draw(batch, label, rect.x + (rect.w - textW) * 0.5f,
				  rect.y + (rect.h - font.Height()) * 0.5f,
				  active ? theme.accent : theme.text);
	}

	if (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) {
		Tab& tab = m_tabs[static_cast<size_t>(m_active)];
		const float maxScroll = (ContentFraction(tab) - 1.0f) * page.h;
		tab.scroll = std::clamp(tab.scroll, 0.0f, maxScroll);
		const gfx::Rect content{page.x, page.y - tab.scroll, page.w, page.h};

		// Clip to the page while content scrolls (sliders draw their labels
		// above their bounds, so a row leaving the top clips cleanly).
		if (maxScroll > 0.0f) batch.SetScissor(&page);
		for (auto& child : tab.children) {
			if (!child->visible) continue;
			const float top = child->bounds.y * page.h - tab.scroll;
			const float bottom = (child->bounds.y + child->bounds.h) * page.h -
								 tab.scroll;
			if (bottom <= 0.0f || top >= page.h) continue;
			child->Layout(content);
			child->Draw(ctx, batch);
		}
		if (maxScroll > 0.0f) {
			batch.SetScissor(nullptr);
			batch.DrawRect(ScrollTrackRect(page), theme.control);
			const gfx::Rect thumb = ScrollThumbRect(page, tab, maxScroll);
			batch.DrawRect(thumb, m_scrollDragging || m_scrollHot
									  ? theme.controlActive
									  : theme.controlHot);
			DrawBorder(batch, thumb, theme.panelBorder);
		}
	}
}

void TabControl::DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) {
	// Children were laid out by Draw earlier in the same render pass.
	if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size())) return;
	for (auto& child : m_tabs[static_cast<size_t>(m_active)].children)
		if (child->visible) child->DrawOverlay(ctx, batch);
}

} // namespace dungeon::ui
