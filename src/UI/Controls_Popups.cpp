// ============================================================================
// UI/Controls_Popups.cpp - the controls that open a list in the overlay pass:
// DropDown (its scrollbar is ui::ScrollBar, ScrollArea's own) and ContextMenu.
// Split by widget family by code-review C127; declared in UI/Controls.h.
// ============================================================================
#include "UI/Controls.h"
#include "UI/Controls_Detail.h"
#include "UI/ControlIcons.h"

#include <algorithm>
#include <optional>

namespace dungeon::ui {

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
	return {list.x, list.y + rowH * static_cast<float>(slot) - m_bar.Offset(), list.w - gutter,
			rowH};
}

ScrollBar::Span DropDown::BarSpan(const gfx::Rect& popup) const {
	const gfx::Rect list = ListRect(popup);
	return ScrollBar::SpanIn(list, 1.0f, Rem(), list.h, MaxScroll(popup));
}

ScrollProbe DropDown::ProbeScroll(const UIContext& ctx) const {
	const gfx::Rect popup = PopupRect(ctx);
	const ScrollBar::Span span = BarSpan(popup);
	return {span, m_bar.Thumb(span), ListRect(popup), m_bar.Offset(), RowH(), m_bar.Dragging()};
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
		m_bar.Release();
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
				m_bar.SetOffset(0.0f);
			}
			m_hoverItem = -1;
			ctx.ConsumeMouse();
			return;
		}

		const ScrollBar::Span bar = BarSpan(popup);
		m_bar.Clamp(bar.maxScroll);

		// The scrollbar runs before the rows: a press on the thumb must neither
		// pick the row behind it nor read as the click-outside that closes. The
		// pointer is the bar's to take - the open list owns it whole. No row is
		// lit while the bar holds it: a drag carried across the rows lights none.
		m_hoverItem = -1;
		const bool barHeld = m_bar.UpdatePointer(*input, bar, true);
		if (bar.Scrolls()) {
			// The wheel steps one ROW a notch (a page steps 1.75 rem).
			if (input->WheelDelta() != 0.0f && popup.Contains(mx, my))
				m_bar.Wheel(input->WheelDelta(), RowH(), bar.maxScroll);
			if (barHeld || bar.track.Contains(mx, my)) {
				ctx.ConsumeMouse();
				return;
			}
		}

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
	m_bar.Release();
	m_bar.SetOffset(rowH * static_cast<float>(slot) - (ListRect(popup).h - rowH) * 0.5f);
	m_bar.Clamp(MaxScroll(popup));
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

std::string_view DropDown::Current() const {
	// A VIEW, so the empty case needs no named empty string (code-review C224):
	// returning `const std::string&` from a ternary with a "" literal COPIED the
	// selected item, a heap allocation per dropdown per frame in a draw path.
	if (m_selected < 0 || m_selected >= static_cast<int>(items.size())) return {};
	return items[static_cast<size_t>(m_selected)];
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
	m_bar.Draw(batch, theme, BarSpan(popup));
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

} // namespace dungeon::ui
