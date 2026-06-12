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
	};

	gfx::Rect TabRect(size_t index) const;
	// The page area below the tab strip, in pixels (children's container);
	// only valid after Layout().
	gfx::Rect PageRect() const;

	std::vector<Tab> m_tabs;
	float m_tabHeight;
	int m_active = 0;
	int m_hover = -1;
};

// Draws a 1px border around a rectangle.
void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color);

} // namespace dungeon::ui
