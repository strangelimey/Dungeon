// ============================================================================
// UI/Controls_Lists.cpp - the lists laid on a page: SlotList (rows in a
// ScrollArea, with its delete confirm) and MenuList (the cut-stone menus).
// Split by widget family by code-review C127; declared in UI/Controls.h.
// ============================================================================
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::ui {

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

} // namespace dungeon::ui
