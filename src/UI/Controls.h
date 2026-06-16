// ============================================================================
// UI/Controls.h — the control library.
//
//   Panel       framed background rectangle (add first so it draws beneath)
//   Label       single line of text; `dim` switches to the muted color
//   TextOutput  scrolling message log; AddLine appends, wheel scrolls
//   Button      click callback; hot/held visual states
//   Slider      horizontal drag, value in [min, max], change callback
//   DropDown    popup list; overlay-drawn so it covers later widgets
//   ColorPicker labeled swatch; click opens an R/G/B/A slider popup
//   KeyBind     labeled key box; click arms it, the next key press rebinds
//   MenuList    vertical menu; hover or arrows/W/S select, click/Enter fire
//   TabControl  tab strip + framed page; owns child widgets per tab
//
// All bounds are normalized fractions (0..1) of the containing widget or
// window (see Widget.h) — the UI scales with the screen. Fixed-pixel detail
// (1px borders, text padding, the slider thumb) and font sizes do NOT scale.
// Colors come from the shared Theme.
// ============================================================================
#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dungeon::ui {

// Simple framed background rectangle (draws first if added first).
class Panel : public Widget {
public:
	explicit Panel(const gfx::Rect& rect) { bounds = rect; }
	void Update(UIContext&) override {}
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
};

class Label : public Widget {
public:
	Label(const gfx::Rect& rect, std::string text) : text(std::move(text)) {
		bounds = rect;
	}
	void Update(UIContext&) override {}
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string text;
	bool dim = false;
};

// Scrolling multi-line text log (message window). New lines append at the
// bottom; the mouse wheel scrolls when hovered.
class TextOutput : public Widget {
public:
	explicit TextOutput(const gfx::Rect& rect, size_t maxLines = 200)
		: m_maxLines(maxLines) {
		bounds = rect;
	}

	void AddLine(std::string line);
	void Clear();
	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	std::deque<std::string> m_lines;
	size_t m_maxLines;
	float m_scroll = 0.0f; // 0 = pinned to latest
};

class Button : public Widget {
public:
	Button(const gfx::Rect& rect, std::string text, std::function<void()> onClick)
		: text(std::move(text)), onClick(std::move(onClick)) {
		bounds = rect;
	}

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string text;
	std::function<void()> onClick;

private:
	bool m_hot = false;
	bool m_held = false;
};

// Horizontal slider, value in [min, max].
class Slider : public Widget {
public:
	Slider(const gfx::Rect& rect, std::string label, float min, float max, float value,
		   std::function<void(float)> onChange)
		: label(std::move(label)), m_min(min), m_max(max), m_value(value),
		  onChange(std::move(onChange)) {
		bounds = rect;
		RefreshDisplay();
	}

	float Value() const { return m_value; }
	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string label;
	std::function<void(float)> onChange;
	// Fires once when a drag ends — for side effects too costly per tick
	// (e.g. persisting the value to disk).
	std::function<void()> onRelease;

private:
	void RefreshDisplay(); // caches the "label: value" text (not per-frame)

	float m_min, m_max, m_value;
	std::string m_display;
	bool m_dragging = false;
};

class DropDown : public Widget {
public:
	DropDown(const gfx::Rect& rect, std::vector<std::string> items, int selected,
			 std::function<void(int)> onSelect)
		: items(std::move(items)), m_selected(selected), onSelect(std::move(onSelect)) {
		bounds = rect;
	}

	int Selected() const { return m_selected; }
	// Reflect a selection chosen elsewhere (e.g. quality auto-setting the light
	// budget); does not fire onSelect. Out-of-range values are ignored.
	void SetSelected(int index) {
		if (index >= 0 && index < static_cast<int>(items.size())) m_selected = index;
	}
	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::vector<std::string> items;
	std::function<void(int)> onSelect;

private:
	gfx::Rect ItemRect(size_t index) const;

	int m_selected = 0;
	int m_hoverItem = -1;
	bool m_open = false;
	bool m_hot = false;
};

// Labeled color swatch. Clicking the swatch opens a popup with one slider per
// R/G/B/A channel (overlay-drawn, like DropDown, so it covers later widgets;
// clamped to the window). onChange fires per tick while a channel drags;
// onClose fires once when the popup closes — persist there.
class ColorPicker : public Widget {
public:
	ColorPicker(const gfx::Rect& rect, std::string label, const Vec4& color,
				std::function<void(const Vec4&)> onChange)
		: label(std::move(label)), m_color(color), onChange(std::move(onChange)) {
		bounds = rect;
	}

	const Vec4& Color() const { return m_color; }
	void SetColor(const Vec4& color) { m_color = color; }

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string label;
	std::function<void(const Vec4&)> onChange;
	std::function<void()> onClose;

private:
	gfx::Rect SwatchRect() const; // the clickable color square (right end)
	gfx::Rect PopupRect(const UIContext& ctx) const;

	Vec4 m_color;
	bool m_open = false;
	bool m_hot = false;
	int m_dragChannel = -1; // 0..3 while a channel slider drags
};

// Labeled key-binding row. The box at the right end shows the current key's
// name; clicking it arms capture ("press a key...") and the next key press
// rebinds — Esc or any mouse click cancels. onChange fires with the new
// Win32 virtual-key code; the armed box owns the mouse like an open popup.
class KeyBind : public Widget {
public:
	KeyBind(const gfx::Rect& rect, std::string label, int vkey,
			std::function<void(int)> onChange);

	int Key() const { return m_vkey; }
	// External rebind (e.g. the owner swapping a duplicate); no onChange.
	void SetKey(int vkey);
	// While armed the owner should suppress its own Esc handling — Esc is
	// the capture's cancel.
	bool IsCapturing() const { return m_capturing; }

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string label;
	std::function<void(int)> onChange;
	// Shown in the box while armed; the owner localizes it (the UI layer has
	// no access to the language table).
	std::string capturePrompt = "press a key...";

private:
	gfx::Rect BoxRect() const; // the clickable key box (right end)

	int m_vkey;
	std::string m_keyName; // cached display name for m_vkey
	bool m_capturing = false;
	bool m_hot = false;
};

// Single-line text input. Click to focus; while focused it takes printable
// characters (WM_CHAR via Input::TypedChars), Backspace deletes the last one,
// and Enter fires onSubmit. Clicking outside the box unfocuses it. A solid
// caret marks focus (no blink — widget Update has no time step). The owner
// reads/sets `text` directly; onChange fires whenever it changes by input.
class TextField : public Widget {
public:
	TextField(const gfx::Rect& rect, std::string text = "")
		: text(std::move(text)) {
		bounds = rect;
	}

	bool Focused() const { return m_focused; }
	void SetFocused(bool on) { m_focused = on; }

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string text;
	std::string placeholder;          // shown dimmed when text is empty
	size_t maxLength = 32;
	std::function<void()> onChange;   // text changed via keyboard
	std::function<void()> onSubmit;   // Enter while focused

private:
	bool m_focused = false;
	bool m_hot = false;
};

// A scrolling list of save-slot rows. Each row shows a primary label (name)
// and a secondary label (timestamp); the row body is clickable (onActivate),
// and a red Delete icon at the right end opens a modal confirm dialog with
// Delete / Cancel buttons (drawn in the overlay pass, owning the mouse until
// resolved). Overflow scrolls (wheel or thumb drag), clipped to the bounds,
// the same way TabControl scrolls a page.
//
// onDelete typically deletes the file and asks the owner to rebuild the page;
// because that rebuild destroys this widget, the owner must DEFER it (not
// rebuild from inside the callback). Update returns immediately after firing a
// row callback so it touches no members afterward.
class SlotList : public Widget {
public:
	struct Row {
		std::string primary;
		std::string secondary;
		std::function<void()> onActivate;
		std::function<void()> onDelete; // null hides the row's Delete icon
	};

	explicit SlotList(const gfx::Rect& rect) { bounds = rect; }
	void AddRow(Row row) { m_rows.push_back(std::move(row)); }

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) override;

	float rowHeight = 48.0f;                  // pixels (fixed, like borders/fonts)
	const gfx::Texture* deleteIcon = nullptr; // red X; a text "X" is the fallback
	// Confirmation dialog strings (the owner localizes them).
	std::string confirmPrompt = "Delete this save?";
	std::string deleteLabel = "Delete";
	std::string cancelLabel = "Cancel";

private:
	float ContentHeight() const {
		return rowHeight * static_cast<float>(m_rows.size());
	}
	float MaxScroll() const;
	gfx::Rect RowRect(size_t index) const; // pixel space, scroll applied
	gfx::Rect DeleteRect(const gfx::Rect& row) const;
	gfx::Rect ScrollTrackRect() const;
	gfx::Rect ScrollThumbRect(float maxScroll) const;
	gfx::Rect ConfirmRect(const UIContext& ctx) const; // centered dialog
	gfx::Rect ConfirmButton(const UIContext& ctx, bool deleteButton) const;

	std::vector<Row> m_rows;
	float m_scroll = 0.0f;
	int m_hotRow = -1;
	int m_hotDelete = -1;
	int m_confirmRow = -1; // row whose confirm dialog is open (-1 = none)
	int m_confirmHot = -1; // dialog button under the mouse: 0 delete, 1 cancel
	bool m_scrollHot = false;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f;
};

// Vertical list of selectable menu entries (the landing page). One entry is
// always "selected"; the mouse selects by hover, and the keyboard (arrows /
// W/S + Enter/Space) moves the selection and activates it, so the highlight
// works identically for both input methods. Entries with no callback still
// highlight but do nothing when activated.
class MenuList : public Widget {
public:
	// itemHeight is a fraction of the list's own height (e.g. 0.2 for five
	// evenly spaced entries).
	MenuList(const gfx::Rect& rect, float itemHeight) : m_itemHeight(itemHeight) {
		bounds = rect;
	}

	void AddItem(std::string label, std::function<void()> onActivate = {});
	// Replaces an entry's label (e.g. "Quality: Medium" cycling in place).
	void SetLabel(size_t index, std::string label);

	int Selected() const { return m_selected; }
	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	struct Item {
		std::string label;
		std::function<void()> onActivate;
	};

	gfx::Rect ItemRect(size_t index) const;
	void MoveSelection(int delta);
	void Activate();

	std::vector<Item> m_items;
	float m_itemHeight;
	int m_selected = 0;
};

// Tab strip across the top of the bounds plus a framed page area below it.
// Each tab owns its child widgets; child bounds are fractions of the PAGE
// area (the rect below the strip), and only the active tab's children
// receive input and draw.
//
// Pages scroll vertically when their content overflows: children authored
// below the page bottom simply have bounds.y + bounds.h > 1, and the control
// detects that, clips the page while drawing, and shows a scrollbar at the
// page's right edge (mouse wheel over the page, or drag the thumb). Each tab
// keeps its own scroll position. Children scrolled fully out of view are
// skipped for input and drawing; popups can't scroll out from under the
// user because an open popup consumes the mouse, which blocks scrolling.
class TabControl : public Widget {
public:
	// tabHeight is a fraction of the control's own height.
	TabControl(const gfx::Rect& rect, float tabHeight) : m_tabHeight(tabHeight) {
		bounds = rect;
	}

	// Returns the new tab's index, used as the `tab` argument to AddChild.
	size_t AddTab(std::string label);

	// Creates a widget on the given tab; the control owns it (same contract
	// as UIContext::Add, but scoped to one page).
	template <typename T, typename... Args>
	T* AddChild(size_t tab, Args&&... args) {
		auto widget = std::make_unique<T>(std::forward<Args>(args)...);
		T* raw = widget.get();
		m_tabs[tab].children.push_back(std::move(widget));
		return raw;
	}

	int ActiveTab() const { return m_active; }
	void SetActiveTab(int index);

	void Update(UIContext& ctx) override;
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	struct Tab {
		std::string label;
		std::vector<std::unique_ptr<Widget>> children;
		float scroll = 0.0f; // pixels scrolled down, clamped every frame
	};

	gfx::Rect TabRect(size_t index) const;
	// The page area below the tab strip, in pixels (children's container);
	// only valid after Layout().
	gfx::Rect PageRect() const;
	// Content height as a multiple of the page height: the lowest child
	// bottom edge, never less than 1 (> 1 means the page scrolls).
	static float ContentFraction(const Tab& tab);
	gfx::Rect ScrollTrackRect(const gfx::Rect& page) const;
	gfx::Rect ScrollThumbRect(const gfx::Rect& page, const Tab& tab,
							  float maxScroll) const;

	std::vector<Tab> m_tabs;
	float m_tabHeight;
	int m_active = 0;
	int m_hover = -1;
	bool m_scrollHot = false;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f; // pointer offset within the thumb while dragging
};

// Draws a 1px border around a rectangle.
void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color);

} // namespace dungeon::ui
