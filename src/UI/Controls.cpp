#include "UI/Controls.h"

#include "UI/ControlIcons.h"
#include "UI/Skin.h"
#include "UI/Units.h"

#include <algorithm>
#include <format>
#include <optional>

namespace dungeon::ui {

namespace {

// The context's panel part, or null when unskinned (flat debug mode) — the
// widget-side gate for every "skin or flat?" draw decision.
const SkinPart* PanelPart(const UIContext& ctx) {
	const Skin* skin = ctx.GetSkin();
	return skin && skin->panel.texture ? &skin->panel : nullptr;
}

} // namespace

void DrawPanelFace(UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect) {
	if (const SkinPart* part = PanelPart(ctx)) {
		DrawNineSlice(batch, rect, *part, {1, 1, 1, ctx.GetTheme().panel.w});
		return;
	}
	batch.DrawRect(rect, ctx.GetTheme().panel);
	DrawBorder(batch, rect, ctx.GetTheme().panelBorder);
}

void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color) {
	batch.DrawRect({rect.x, rect.y, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
	batch.DrawRect({rect.x, rect.y, 1, rect.h}, color);
	batch.DrawRect({rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
}

// --- Panel -------------------------------------------------------------

void Panel::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	DrawPanelFace(ctx, batch, Pixel());
}

// --- Separator ---------------------------------------------------------

void Separator::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	batch.DrawRect({px.x, px.y + px.h * 0.5f, px.w, 1.0f}, ctx.GetTheme().panelBorder);
}

// --- Label -------------------------------------------------------------

void Label::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	TextFont().Draw(batch, text, Pixel().x, Pixel().y,
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

void TextOutput::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	if (Pixel().Contains(input->MouseX(), input->MouseY()) && input->WheelDelta() != 0) {
		m_scroll += input->WheelDelta() * 3.0f;
		const float maxScroll =
			std::max(0.0f, static_cast<float>(m_lines.size()) -
							   Pixel().h / TextFont().LineAdvance());
		m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
		ctx.ConsumeMouse();
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

void Button::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	if (icon) {
		// Icon-only: the round face IS the button (it carries its own chrome
		// and alpha) — no button face behind it. Rotated in quarter turns
		// (screen Y is down: positive turns step right→down→left→up), with
		// hover/held brightening standing in for the face wash.
		const float d = std::min(px.w, px.h) * 0.92f;
		const float f = (m_held || active) ? 1.15f : (m_hot ? 1.0f : 0.82f);
		batch.DrawSpriteRotated({px.x + px.w * 0.5f, px.y + px.h * 0.5f}, {d, d},
								static_cast<float>(iconTurns) * (kPi * 0.5f),
								{0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		return;
	}
	DrawButtonFace(batch, TextFont(), px, text, ctx.GetTheme(), m_hot,
				   m_held || active, true, ctx.GetSkin());
}

void DrawButtonFace(gfx::SpriteBatch& batch, const Font& font,
					const gfx::Rect& rect,
					const std::string& label, const Theme& theme, bool hot,
					bool held, bool enabled, const Skin* skin) {
	if (skin && skin->button.texture) {
		// Disabled dims the face itself; hot/held wash the theme's control
		// color over the texture so state keeps reading through the theme.
		const float dim = enabled ? 1.0f : 0.45f;
		DrawNineSlice(batch, rect, skin->button, {dim, dim, dim, 1.0f});
		if (enabled && (held || hot)) {
			Vec4 wash = held ? theme.controlActive : theme.controlHot;
			wash.w = 0.4f;
			batch.DrawRect(rect, wash);
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
			  rect.y + (rect.h - font.Height()) * 0.5f,
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
	if (highlight) {
		batch.DrawRect(px, theme.controlActive);
		DrawBorder(batch, px, theme.panelBorder);
	} else if (m_hot) {
		batch.DrawRect(px, theme.controlHot);
	}
	// The check box itself, left-aligned and vertically centered.
	const float box = std::min(px.h * 0.6f, Rem(0.65f));
	const gfx::Rect b{px.x + Rem(0.15f), px.y + (px.h - box) * 0.5f, box, box};
	batch.DrawRect(b, theme.control);
	DrawBorder(batch, b, theme.panelBorder);
	if (m_checked) {
		const float in = box * 0.24f;
		batch.DrawRect({b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in}, theme.accent);
	}
	font.Draw(batch, label, b.x + b.w + Rem(0.3f), px.y + (px.h - font.Height()) * 0.5f,
			  (m_checked || highlight) ? theme.text : theme.textDim);
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
	m_display = std::format("{}: {:.2f}", label, m_value);
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

	// Track.
	const float trackH = Rem(0.15f);
	const float trackY = bandY + (bandH - trackH) * 0.5f;
	batch.DrawRect({px.x, trackY, px.w, trackH}, theme.control);

	// Filled portion + thumb.
	const float t = (m_value - m_min) / std::max(m_max - m_min, 1e-6f);
	batch.DrawRect({px.x, trackY, px.w * t, trackH}, theme.accent);
	const float thumbW = Rem(0.35f);
	const float thumbX = px.x + px.w * t - thumbW * 0.5f;
	batch.DrawRect({thumbX, bandY, thumbW, bandH},
				   m_dragging ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, {thumbX, bandY, thumbW, bandH}, theme.panelBorder);
}

// --- DropDown ------------------------------------------------------------

// The list opens below the control; when it doesn't fit there and the space
// above is larger, it flips. Whatever is still too tall scrolls — a pool-length
// list used to draw straight off the bottom of the window.
gfx::Rect DropDown::PopupRect(const UIContext& ctx) const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(0.15f);
	const float content = px.h * static_cast<float>(items.size());
	const float below = std::max(0.0f, ctx.Height() - (px.y + px.h) - pad);
	const float above = std::max(0.0f, px.y - pad);
	if (content <= below || below >= above) return {px.x, px.y + px.h, px.w,
													std::min(content, below)};
	const float h = std::min(content, above);
	return {px.x, px.y - h, px.w, h};
}

float DropDown::MaxScroll(const gfx::Rect& popup) const {
	return std::max(0.0f, Pixel().h * static_cast<float>(items.size()) - popup.h);
}

gfx::Rect DropDown::ItemRect(const gfx::Rect& popup, size_t index) const {
	const float rowH = Pixel().h;
	// Rows stop short of the scrollbar gutter, so a row hover never sits under
	// the thumb (SlotList's rule).
	const float gutter = MaxScroll(popup) > 0.0f ? Rem(0.45f) : 0.0f;
	return {popup.x, popup.y + rowH * static_cast<float>(index) - m_scroll,
			popup.w - gutter, rowH};
}

gfx::Rect DropDown::ScrollTrackRect(const gfx::Rect& popup) const {
	const float barW = Rem(0.35f);
	return {popup.x + popup.w - barW - 1.0f, popup.y + 1.0f, barW, popup.h - 2.0f};
}

gfx::Rect DropDown::ScrollThumbRect(const gfx::Rect& popup, float maxScroll) const {
	const gfx::Rect track = ScrollTrackRect(popup);
	const float thumbH =
		std::max(track.h * popup.h / (popup.h + maxScroll), Rem(0.9f));
	const float t = maxScroll > 0.0f ? m_scroll / maxScroll : 0.0f;
	return {track.x, track.y + (track.h - thumbH) * t, track.w, thumbH};
}

void DropDown::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();

	if (m_open) {
		// The open popup owns the mouse entirely.
		const gfx::Rect popup = PopupRect(ctx);
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
				m_scroll = std::clamp(m_scroll - input->WheelDelta() * Pixel().h,
									  0.0f, maxScroll);
			if (m_scrollHot || m_scrollDragging || track.Contains(mx, my)) {
				ctx.ConsumeMouse();
				return;
			}
		} else {
			m_scrollDragging = false;
		}

		m_hoverItem = -1;
		if (popup.Contains(mx, my)) { // a part-scrolled row reaches past the box
			for (size_t i = 0; i < items.size(); ++i) {
				if (!ItemRect(popup, i).Contains(mx, my)) continue;
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
		if (input->WasMousePressed(MouseButton::Left)) m_open = false;
		ctx.ConsumeMouse();
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) {
			m_open = true;
			// Open with the current selection in view — a long list otherwise
			// opens at the top, nowhere near what it says it is showing.
			const gfx::Rect popup = PopupRect(ctx);
			const float rowH = Pixel().h;
			m_scroll = std::clamp(rowH * static_cast<float>(m_selected) -
									  (popup.h - rowH) * 0.5f,
								  0.0f, MaxScroll(popup));
		}
	}
}

void DropDown::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, m_hot || m_open ? theme.controlHot : theme.control);
	DrawBorder(batch, px, theme.panelBorder);
	// The empty case is a named string, NOT a "" literal: a ternary mixing
	// std::string with const char* has common type std::string, so binding the
	// reference COPIED the selected item — a heap allocation per dropdown per
	// frame, in a draw path (found by the steady-state allocation guard).
	static const std::string kNoSelection;
	const std::string& current =
		(m_selected >= 0 && m_selected < static_cast<int>(items.size()))
			? items[static_cast<size_t>(m_selected)]
			: kNoSelection;
	const float textY = px.y + (px.h - font.Height()) * 0.5f;
	font.Draw(batch, current, px.x + 8, textY, theme.text);
	DrawDropDownExpander(batch, font, px, theme, m_open, m_hot);
}

void DrawDropDownExpander(gfx::SpriteBatch& batch, const Font& font,
						  const gfx::Rect& rect, const Theme& theme, bool open,
						  bool hot) {
	// The authored box is a SQUARE sized off the control's height and inset so
	// it clears the border, turned half a rotation while open — the triangle is
	// the only asymmetric thing in it, so ONE asset serves both states.
	// Brightness carries the hover/open read the flat glyph used to get from
	// the accent color (the same idiom Button's icon path uses).
	//
	// The inset is a fraction of the TEXT height, not of the rect: the box has
	// to clear a 1px border beside type of whatever size, and a rect fraction
	// would grow the gap on a tall control and lose it on a short one.
	const float inset = font.Height() * 0.12f;
	if (const gfx::Texture* icon = GetControlIcons().dropDown) {
		const float d = rect.h - inset * 2.0f;
		const float f = (hot || open) ? 1.15f : 0.9f;
		batch.DrawSpriteRotated(
			{rect.x + rect.w - inset - d * 0.5f, rect.y + rect.h * 0.5f}, {d, d},
			open ? kPi : 0.0f, {0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		return;
	}
	// Fallback with no icon installed: the text arrow, right-aligned with a
	// margin so it clears the border at any font size (measure it rather than
	// assume a fixed width).
	const char* arrow = open ? "^" : "v";
	font.Draw(batch, arrow, rect.x + rect.w - font.MeasureWidth(arrow) - inset * 3.0f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.accent);
}

void DropDown::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect popup = PopupRect(ctx);
	const float maxScroll = MaxScroll(popup);

	{
		// Scoped so the clip lifts before the scrollbar draws beside the list.
		std::optional<ScopedClip> clip;
		if (maxScroll > 0.0f) {
			batch.DrawRect(popup, theme.control); // backing behind the part-rows
			clip.emplace(batch, popup);
		}
		for (size_t i = 0; i < items.size(); ++i) {
			const gfx::Rect rect = ItemRect(popup, i);
			if (rect.y + rect.h <= popup.y || rect.y >= popup.y + popup.h) continue;
			const bool hovered = static_cast<int>(i) == m_hoverItem;
			batch.DrawRect(rect, hovered ? theme.controlHot : theme.control);
			DrawBorder(batch, rect, theme.panelBorder);
			font.Draw(batch, items[i], rect.x + 8,
					  rect.y + (rect.h - font.Height()) * 0.5f,
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

void ContextMenu::Open(float x, float y, std::vector<Entry> entries) {
	if (entries.empty()) return;
	m_entries = std::move(entries);
	m_x = x;
	m_y = y;
	m_hover = -1;
	m_openChild = -1;
	m_childHover = -1;
	m_open = true;
}

gfx::Rect ContextMenu::EntryRect(size_t i) const {
	return {m_x, m_y + m_rowH * static_cast<float>(i), m_w, m_rowH};
}

gfx::Rect ContextMenu::ChildRect(size_t i) const {
	return {m_childX, m_childY + m_rowH * static_cast<float>(i), m_childW, m_rowH};
}

void ContextMenu::UpdateSelf(UIContext& ctx) {
	if (!m_open) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	// Size to the widest label (groups reserve room for the "»" marker), then
	// clamp the box on screen.
	const Font& font = TextFont();
	m_rowH = Rem(1.45f);
	float w = Rem(2.85f);
	for (const Entry& e : m_entries)
		w = std::max(w, font.MeasureWidth(e.label) + Rem(0.85f) +
							(e.children.empty() ? 0.0f : Rem(0.6f)));
	m_w = w;
	const float menuH = m_rowH * static_cast<float>(m_entries.size());
	m_x = std::clamp(m_x, 0.0f, std::max(0.0f, ctx.Width() - m_w));
	m_y = std::clamp(m_y, 0.0f, std::max(0.0f, ctx.Height() - menuH));

	// Lay the open group's submenu beside the parent: at its row, flush with
	// the parent's right edge — flipped to the left edge when it would run off
	// screen — with the parent still fully visible.
	if (m_openChild >= 0 && m_openChild < static_cast<int>(m_entries.size())) {
		const std::vector<Entry>& kids =
			m_entries[static_cast<size_t>(m_openChild)].children;
		float cw = Rem(2.85f);
		for (const Entry& e : kids)
			cw = std::max(cw, font.MeasureWidth(e.label) + Rem(0.85f));
		m_childW = cw;
		m_childX = m_x + m_w;
		if (m_childX + cw > ctx.Width()) m_childX = std::max(0.0f, m_x - cw);
		const float childH = m_rowH * static_cast<float>(kids.size());
		m_childY = std::clamp(m_y + m_rowH * static_cast<float>(m_openChild), 0.0f,
							  std::max(0.0f, ctx.Height() - childH));
	}

	// The open menu owns the mouse. The submenu is checked first (it can
	// overlap the parent when flipped left): a leaf pick closes everything.
	m_hover = -1;
	m_childHover = -1;
	if (m_openChild >= 0 && m_openChild < static_cast<int>(m_entries.size())) {
		const std::vector<Entry>& kids =
			m_entries[static_cast<size_t>(m_openChild)].children;
		for (size_t i = 0; i < kids.size(); ++i) {
			if (!ChildRect(i).Contains(input->MouseX(), input->MouseY())) continue;
			m_childHover = static_cast<int>(i);
			if (input->WasMousePressed(MouseButton::Left)) {
				auto fn = kids[i].onSelect; // copy: the callback may rebuild us
				Close();
				ctx.ConsumeMouse();
				if (fn) fn();
				return;
			}
			ctx.ConsumeMouse();
			return; // over the submenu — the parent rows don't hit-test
		}
	}
	for (size_t i = 0; i < m_entries.size(); ++i) {
		if (!EntryRect(i).Contains(input->MouseX(), input->MouseY())) continue;
		m_hover = static_cast<int>(i);
		if (input->WasMousePressed(MouseButton::Left)) {
			if (!m_entries[i].children.empty()) {
				// A group: open its submenu (same group toggles, another
				// swaps). The parent stays up for the next pick.
				const int idx = static_cast<int>(i);
				m_openChild = m_openChild == idx ? -1 : idx;
			} else {
				auto fn = m_entries[i].onSelect; // copy: may rebuild us
				Close();
				ctx.ConsumeMouse();
				if (fn) fn();
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
	const bool skinned = PanelPart(ctx) != nullptr;
	if (skinned && !m_entries.empty()) {
		const gfx::Rect box{m_x, m_y, m_w,
							m_rowH * static_cast<float>(m_entries.size())};
		DrawNineSlice(batch, box, *PanelPart(ctx), {1, 1, 1, 1});
	}
	for (size_t i = 0; i < m_entries.size(); ++i) {
		const gfx::Rect rect = EntryRect(i);
		const bool groupOpen = static_cast<int>(i) == m_openChild;
		const bool hovered = static_cast<int>(i) == m_hover;
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
		font.Draw(batch, m_entries[i].label, rect.x + 10,
				  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
		if (!m_entries[i].children.empty()) // group marker at the right edge
			font.Draw(batch, "»", rect.x + rect.w - 16,
					  rect.y + (rect.h - font.Height()) * 0.5f,
					  groupOpen ? theme.text : theme.textDim);
	}
	// The open group's submenu, beside the parent (drawn after = on top).
	if (m_openChild >= 0 && m_openChild < static_cast<int>(m_entries.size())) {
		const std::vector<Entry>& kids =
			m_entries[static_cast<size_t>(m_openChild)].children;
		if (skinned && !kids.empty()) {
			const gfx::Rect box{m_childX, m_childY, m_childW,
								m_rowH * static_cast<float>(kids.size())};
			DrawNineSlice(batch, box, *PanelPart(ctx), {1, 1, 1, 1});
		}
		for (size_t i = 0; i < kids.size(); ++i) {
			const gfx::Rect rect = ChildRect(i);
			const bool hovered = static_cast<int>(i) == m_childHover;
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
			font.Draw(batch, kids[i].label, rect.x + 10,
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
		ctx.ConsumeMouse(); // the open popup owns the mouse entirely
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && SwatchRect().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_open = true;
	}
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

	if (const SkinPart* part = PanelPart(ctx)) {
		DrawNineSlice(batch, popup, *part, {1, 1, 1, 1}); // opaque, unlike Panel
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
		const float tw = Rem(0.3f);
		const gfx::Rect thumb{track.x + track.w * value - tw * 0.5f,
							  track.y + Rem(0.08f), tw, track.h - Rem(0.16f)};
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
	batch.DrawRect(box, m_capturing ? theme.controlActive
									: (m_hot ? theme.controlHot : theme.control));
	DrawBorder(batch, box, m_capturing || m_hot ? theme.accent : theme.panelBorder);

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

	bool changed = false;
	for (const char c : input->TypedChars()) {
		if (text.size() >= maxLength) break;
		text.push_back(c); // OnChar already filtered to printable characters
		changed = true;
	}
	if (input->WasKeyPressed(vk::Back) && !text.empty()) {
		text.pop_back();
		changed = true;
	}
	if (input->WasKeyPressed(vk::Return) && onSubmit) onSubmit();
	if (changed && onChange) onChange();
}

void TextField::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, m_focused ? theme.controlActive
								 : (m_hot ? theme.controlHot : theme.control));
	DrawBorder(batch, px, m_focused || m_hot ? theme.accent : theme.panelBorder);

	const float pad = Rem(0.3f);
	const float ty = px.y + (px.h - font.Height()) * 0.5f;
	if (text.empty() && !m_focused) {
		font.Draw(batch, placeholder, px.x + pad, ty, theme.textDim);
	} else {
		font.Draw(batch, text, px.x + pad, ty, theme.text);
		if (m_focused) {
			const float caretX = px.x + pad + font.MeasureWidth(text) + 1.0f;
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

void SlotRow::UpdateSelf(UIContext& ctx) {
	m_hot = m_hotDelete = false;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const float mx = input->MouseX(), my = input->MouseY();
	if (!Pixel().Contains(mx, my)) return;
	m_hot = true;
	m_hotDelete = m_deletable && DeleteRect().Contains(mx, my);
	ctx.ConsumeMouse();
	if (!input->WasMousePressed(MouseButton::Left)) return;
	// Either callback may (deferred) rebuild the page that owns this widget —
	// fire and touch nothing afterwards.
	if (m_hotDelete) {
		if (m_onDeleteClick) m_onDeleteClick();
	} else if (m_onActivate) {
		m_onActivate();
	}
}

void SlotRow::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& r = Pixel();
	batch.DrawRect(r, m_hot ? theme.controlHot : theme.control);
	DrawBorder(batch, r, theme.panelBorder);

	const float ty = r.y + (r.h - font.Height()) * 0.5f;
	font.Draw(batch, m_primary, r.x + Rem(0.45f), ty, theme.text);

	const gfx::Rect del = DeleteRect();
	if (!m_secondary.empty()) {
		const float sw = font.MeasureWidth(m_secondary);
		const float sx = (m_deletable ? del.x : r.x + r.w) - sw - Rem(0.6f);
		font.Draw(batch, m_secondary, sx, ty, theme.textDim);
	}
	if (!m_deletable) return;
	if (const gfx::Texture* icon = m_icon ? *m_icon : nullptr) {
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

// rowHeight is in pixels and the rows are fractions of the scrolling area, so
// the stack is assigned per layout. A left inset keeps them off the frame; the
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
	const float w = Rem(13.5f), h = Rem(6.0f);
	return {(ctx.Width() - w) * 0.5f, (ctx.Height() - h) * 0.5f, w, h};
}

gfx::Rect SlotList::ConfirmButton(const UIContext& ctx, bool deleteButton) const {
	const gfx::Rect d = ConfirmRect(ctx);
	const float m = Rem(0.7f); // margin / gutter around the pair
	const float bw = (d.w - 3.0f * m) * 0.5f, bh = Rem(1.6f);
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

	auto button = [&](const gfx::Rect& b, const std::string& label, bool hot,
					  bool danger) {
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
			itemH - Rem(0.3f)}; // gap between entries
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

void MenuList::UpdateSelf(UIContext& ctx) {
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
	if (input->WasKeyPressed(vk::Up) || input->WasKeyPressed('W')) MoveSelection(-1);
	if (input->WasKeyPressed(vk::Down) || input->WasKeyPressed('S')) MoveSelection(+1);
	if (input->WasKeyPressed(vk::Return) || input->WasKeyPressed(vk::Space)) Activate();
}

void MenuList::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();

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

	// Mouse wheel anywhere over the area.
	if (!ctx.IsMouseConsumed() && Pixel().Contains(mx, my) &&
		input->WheelDelta() != 0.0f) {
		m_scroll = std::clamp(m_scroll - input->WheelDelta() * Rem(1.75f), 0.0f,
							  maxScroll);
		ctx.ConsumeMouse();
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
	const bool skinned = PanelPart(ctx) != nullptr;
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
// ones they resolve with this frame.
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
