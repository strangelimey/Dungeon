// ============================================================================
// UI/Controls.h — the control library.
//
//   Panel       framed background rectangle (add first so it draws beneath)
//   Separator   horizontal rule (like HTML <hr>) dividing sections
//   Label       single line of text; `dim` switches to the muted color
//   TextOutput  scrolling message log; AddLine appends, wheel scrolls
//   Button      click callback; hot/held visual states
//   Slider      horizontal drag, value in [min, max], change callback
//   DropDown    popup list; overlay-drawn so it covers later widgets
//   ColorPicker labeled swatch; click opens an R/G/B/A slider popup
//   KeyBind     labeled key box; click arms it, the next key press rebinds
//   MenuList    vertical menu; hover or arrows/W/S select, click/Enter fire
//   ScrollArea  container that scrolls + clips its children when they overflow
//   TabControl  tab strip + a framed ScrollArea page per tab
//   Repeater    container whose children come from a per-frame count
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

struct Skin;
class ScrollArea; // defined below; SlotList holds one

// Framed background rectangle, and the plainest container there is: give it
// `padX`/`padY` (fractions of its own width/height) and its children resolve
// against the padded interior, so a plate of rows is authored as fractions of
// the plate rather than of the window. Both default to 0, which leaves
// ContentRect the whole rect — every Panel that predates this is unaffected.
class Panel : public Widget {
public:
	explicit Panel(const gfx::Rect& rect) { bounds = rect; }
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	gfx::Rect ContentRect() const override {
		const gfx::Rect& px = Pixel();
		const float ix = padX * px.w, iy = padY * px.h;
		return {px.x + ix, px.y + iy, px.w - 2 * ix, px.h - 2 * iy};
	}

	float padX = 0.0f;
	float padY = 0.0f;
};

// Horizontal rule (like HTML <hr>): a 1px line centered in its bounds, spanning
// its full width, in the dim border color. Divides sections; takes no input.
class Separator : public Widget {
public:
	explicit Separator(const gfx::Rect& rect) { bounds = rect; }
	void UpdateSelf(UIContext&) override {}
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
};

class Label : public Widget {
public:
	Label(const gfx::Rect& rect, std::string text) : text(std::move(text)) {
		bounds = rect;
	}
	void UpdateSelf(UIContext&) override {}
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string text;
	std::function<void()> onClick;
	// Draw as selected (controlActive fill) regardless of hover — for a row that
	// represents the current selection in a list (the config dialog's state/clip rows).
	bool active = false;
	// Optional icon face drawn centered INSTEAD of the label (the text stays
	// the fallback when the texture is missing). `iconTurns` rotates it in
	// quarter turns clockwise, so one chevron asset serves every direction
	// (the HUD movement pad).
	const gfx::Texture* icon = nullptr;
	int iconTurns = 0;

private:
	bool m_hot = false;
	bool m_held = false;
};

// A labeled on/off box: a small square at the left with the label to its right;
// clicking anywhere in the row toggles it and fires onChange with the new state.
// `highlight` draws the row selected (independent of the check) so it can double
// as a list row. The owner reads Checked()/SetChecked() to sync external state.
class Checkbox : public Widget {
public:
	Checkbox(const gfx::Rect& rect, std::string label, bool checked,
			 std::function<void(bool)> onChange)
		: label(std::move(label)), onChange(std::move(onChange)), m_checked(checked) {
		bounds = rect;
	}

	bool Checked() const { return m_checked; }
	void SetChecked(bool on) { m_checked = on; }
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string label;
	std::function<void(bool)> onChange;
	bool highlight = false; // draw the row highlighted (e.g. selected/previewed)

private:
	bool m_checked = false;
	bool m_hot = false;
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
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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

// Labeled selector whose open list is drawn as an overlay, so it covers the
// widgets laid out after it. The list is CLAMPED to the window: it opens below
// the control, flips above when there is more room there, and scrolls (wheel or
// thumb drag, like SlotList) when it still doesn't fit — an installed-asset list
// is as long as the pool, and it used to run off the bottom of the screen.
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
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::vector<std::string> items;
	std::function<void(int)> onSelect;

private:
	// The open list's box, clamped to the window (below the control, or above it
	// when that side has more room). Everything else resolves against it.
	gfx::Rect PopupRect(const UIContext& ctx) const;
	gfx::Rect ItemRect(const gfx::Rect& popup, size_t index) const;
	float MaxScroll(const gfx::Rect& popup) const;
	gfx::Rect ScrollTrackRect(const gfx::Rect& popup) const;
	gfx::Rect ScrollThumbRect(const gfx::Rect& popup, float maxScroll) const;

	int m_selected = 0;
	int m_hoverItem = -1;
	bool m_open = false;
	bool m_hot = false;
	float m_scroll = 0.0f; // pixels scrolled down the open list
	bool m_scrollHot = false;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f; // pointer offset within the thumb while dragging
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

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string text;
	std::string placeholder;          // shown dimmed when text is empty
	size_t maxLength = 32;
	std::function<void()> onChange;   // text changed via keyboard
	std::function<void()> onSubmit;   // Enter while focused

private:
	bool m_focused = false;
	bool m_hot = false;
};

// A floating right-click context menu: a short list of labelled actions opened
// at a screen point (overlay-drawn, owning the mouse while open, like the
// DropDown popup). Closes when a leaf entry is chosen or the user clicks
// elsewhere. An entry with CHILDREN is a group: clicking it opens the children
// as a CASCADING submenu beside the parent — the parent stays visible, so the
// other groups remain in reach (clicking another group swaps the submenu, the
// same group toggles it). One level deep. It is a persistent widget the owner
// reuses — call Open() with the actions for whatever was right-clicked; an
// empty list is a no-op.
//
// SCREEN-ANCHORED, not parent-relative: it opens at an absolute pixel point and
// draws in the OVERLAY pass, so `bounds` stays zero — a context menu must not be
// clipped or placed by whatever happens to own it. The tree inspector therefore
// shows it as 0x0, which is correct rather than a missing rect.
class ContextMenu : public Widget {
public:
	struct Entry {
		std::string label;
		std::function<void()> onSelect; // leaf action (unused on a group)
		std::vector<Entry> children;    // non-empty = group with a submenu
	};

	ContextMenu() = default;

	// Opens at (x,y) device pixels with the given actions (clamped on screen in
	// Update). No-op for an empty list.
	void Open(float x, float y, std::vector<Entry> entries);
	void Close() {
		m_open = false;
		m_openChild = -1;
	}
	bool IsOpen() const { return m_open; }

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext&, gfx::SpriteBatch&) override {} // overlay-only
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	gfx::Rect EntryRect(size_t i) const;
	gfx::Rect ChildRect(size_t i) const; // row i of the open group's submenu

	bool m_open = false;
	float m_x = 0.0f, m_y = 0.0f; // top-left, device pixels (clamped in Update)
	float m_w = 0.0f, m_rowH = 0.0f; // sized from the font in Update
	std::vector<Entry> m_entries;
	int m_hover = -1;
	// Cascading submenu state: which group's children are showing (-1 = none)
	// and the submenu box, laid out beside the parent in Update.
	int m_openChild = -1;
	int m_childHover = -1;
	float m_childX = 0.0f, m_childY = 0.0f, m_childW = 0.0f;
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
class SlotList;

// One row of a SlotList: the name, the timestamp, and (when the row can be
// deleted) the icon button at its right end. Owns its own hover, and the
// delete icon's hover separately, so the list itself tracks neither.
class SlotRow : public Widget {
public:
	SlotRow(std::string primary, std::string secondary,
			std::function<void()> onActivate, bool deletable,
			// The list's deleteIcon member, read live — the owner sets it after
			// the rows are built.
			const gfx::Texture* const* icon, std::function<void()> onDeleteClick);

private:
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	gfx::Rect DeleteRect() const; // square icon button at the row's right end

	std::string m_primary, m_secondary;
	std::function<void()> m_onActivate;
	std::function<void()> m_onDeleteClick;
	const gfx::Texture* const* m_icon;
	bool m_deletable;
	bool m_hot = false;
	bool m_hotDelete = false;
};

class SlotList : public Widget {
public:
	struct Row {
		std::string primary;
		std::string secondary;
		std::function<void()> onActivate;
		std::function<void()> onDelete; // null hides the row's Delete icon
	};

	explicit SlotList(const gfx::Rect& rect);
	void AddRow(Row row);

	float rowHeight = 48.0f;                  // pixels (fixed, like borders/fonts)
	const gfx::Texture* deleteIcon = nullptr; // red X; a text "X" is the fallback
	// Confirmation dialog strings (the owner localizes them).
	std::string confirmPrompt = "Delete this save?";
	std::string deleteLabel = "Delete";
	std::string cancelLabel = "Cancel";

private:
	// Stacks the rows down the scrolling area (their bounds are fractions of
	// it, and rowHeight is in pixels, so they are assigned per layout).
	void LayoutSelf(UIContext& ctx) override;
	// The modal runs BEFORE the rows so it can take the mouse from them.
	void UpdateBeforeChildren(UIContext& ctx) override;
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	gfx::Rect ConfirmRect(const UIContext& ctx) const; // centered dialog
	gfx::Rect ConfirmButton(const UIContext& ctx, bool deleteButton) const;

	ScrollArea* m_scroll = nullptr;
	// What the modal needs about each row, parallel to the row widgets.
	struct Entry {
		std::string primary;
		std::function<void()> onDelete;
	};
	std::vector<Entry> m_entries;
	int m_confirmRow = -1; // row whose confirm dialog is open (-1 = none)
	int m_confirmHot = -1; // dialog button under the mouse: 0 delete, 1 cancel
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
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

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

// A container that scrolls its children vertically when they overflow it.
// Children are authored as fractions of ContentRect() — this widget's rect
// inset by `padding`, with `gutter` reserved at the right for the scrollbar so
// the layout doesn't shift when the bar appears — and a child authored past the
// bottom (bounds.y + bounds.h > 1) is what makes the area scroll. Overflow is
// clipped, the wheel scrolls anywhere over the area, and the thumb drags.
// Children scrolled fully out of view get neither input nor draw; an open popup
// can't scroll out from under the user because it consumes the mouse, which
// blocks the wheel.
class ScrollArea : public Widget {
public:
	explicit ScrollArea(const gfx::Rect& rect) { bounds = rect; }

	float Scroll() const { return m_scroll; }
	void ScrollToTop() { m_scroll = 0.0f; }

	// The children's container: the view box, shifted up by the scroll.
	gfx::Rect ContentRect() const override;
	// The view box itself (unscrolled) — what the scroll maths and clip use.
	gfx::Rect ViewRect() const;

	float padding = 12.0f; // inset from this widget's own edge
	float gutter = 14.0f;  // scrollbar track + margin, always reserved

private:
	void LayoutSelf(UIContext& ctx) override;
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	bool ChildActive(const Widget& child) const override;
	const gfx::Rect* ChildClip() const override;

	// Content height as a multiple of the view height: the lowest child bottom
	// edge, never less than 1 (> 1 means the area scrolls).
	float ContentFraction() const;
	float MaxScroll() const;
	gfx::Rect ScrollTrackRect() const;
	gfx::Rect ScrollThumbRect(float maxScroll) const;

	float m_scroll = 0.0f; // pixels scrolled down, clamped every layout
	gfx::Rect m_clip{};    // ViewRect cached so ChildClip can hand back a pointer
	bool m_clipping = false;
	bool m_scrollHot = false;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f; // pointer offset within the thumb while dragging
};

// Tab strip across the top of the bounds plus a framed page area below it.
// Each tab is a ScrollArea child filling that page, so a tab's widgets are
// authored as fractions of the page and scroll for free when they overflow it
// (see ScrollArea). Only the active tab's page is visible, so only its children
// receive input and draw; each tab keeps its own scroll position.
class TabControl : public Widget {
public:
	// tabHeight is a fraction of the control's own height.
	TabControl(const gfx::Rect& rect, float tabHeight) : m_tabHeight(tabHeight) {
		bounds = rect;
	}

	// Returns the new tab's index, used as the `tab` argument to AddChild.
	size_t AddTab(std::string label);

	// Creates a widget on the given tab's page; the page owns it (same contract
	// as UIContext::Add, but scoped to that page).
	template <typename T, typename... Args>
	T* AddChild(size_t tab, Args&&... args) {
		return m_tabs[tab].page->Add<T>(std::forward<Args>(args)...);
	}

	// A tab's page, for anything that needs the container itself (e.g. to reset
	// its scroll). Null for an out-of-range index.
	ScrollArea* Page(size_t tab) {
		return tab < m_tabs.size() ? m_tabs[tab].page : nullptr;
	}

	int ActiveTab() const { return m_active; }
	void SetActiveTab(int index);

	// The page area below the strip — what a tab's ScrollArea fills.
	gfx::Rect ContentRect() const override;

private:
	// A tab is its label plus the page that holds its widgets. `page` is one of
	// this control's own children, added by AddTab — so tab index and child
	// index coincide, and nothing else may be added as a direct child.
	struct Tab {
		std::string label;
		ScrollArea* page = nullptr;
	};

	// Sizes the strip before anything resolves against it — the tree calls this
	// right after our own pixel rect lands and before ContentRect() is asked
	// for — and shows only the active tab's page.
	void LayoutSelf(UIContext& ctx) override;
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Measures each tab's label and sizes the strip: every tab is at least the
	// even split, wider when its text needs it, and the control grows + recenters
	// to the total. Caches m_tabWidths / m_effRect for the const rect helpers
	// below (needs the font from ctx).
	void LayoutStrip(UIContext& ctx);

	gfx::Rect TabRect(size_t index) const;
	// The page area below the tab strip, in pixels (the panel frame); only
	// valid after LayoutStrip().
	gfx::Rect PageRect() const;

	std::vector<Tab> m_tabs;
	std::vector<float> m_tabWidths; // per-tab strip width (LayoutStrip)
	gfx::Rect m_effRect{};          // control rect grown to fit the strip
	float m_tabHeight;
	int m_active = 0;
	int m_hover = -1;
};

// A container whose children come from a per-frame COUNT — the status-effect
// strip, a rune grid, a list of rows. Each frame it reads `count`, grows the
// pool with `factory` until it has that many children, and gives child N the
// bounds `place(N)` (fractions of this widget, like any child). The children
// are real widgets, so each repeated item owns its own hover and click.
//
// The pool only ever GROWS: a child past the live count is hidden, never
// destroyed, so nothing dies mid-frame and no pointer can dangle. The corollary
// is the rule every repeated child follows — hold the INDEX and re-resolve
// against the model each frame (the RosterMember pattern), never cache a
// pointer into the model. The repeater owns its children's `visible` flag; a
// repeated child must not set its own.
class Repeater : public Widget {
public:
	using Factory = std::function<std::unique_ptr<Widget>(size_t index)>;
	using Counter = std::function<size_t()>;
	using Placer = std::function<gfx::Rect(size_t index)>;

	Repeater(const gfx::Rect& rect, Factory factory, Counter count, Placer place)
		: m_factory(std::move(factory)), m_count(std::move(count)),
		  m_place(std::move(place)) {
		bounds = rect;
	}

	// How many children were live at the last layout (what `count` returned,
	// capped by what the factory actually produced).
	size_t LiveCount() const { return m_live; }

private:
	void LayoutSelf(UIContext& ctx) override;

	Factory m_factory;
	Counter m_count;
	Placer m_place;
	size_t m_live = 0;
};

// Draws a 1px border around a rectangle.
void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color);

// Draws the shared framed-background look: the context's skin panel part when
// one is set (its frame is baked in; the theme's panel alpha rides the tint so
// the background-opacity preference applies to both looks), else the flat
// theme fill + 1px border. Panel/TextOutput/popups route through it, and so
// does the game-layer chrome (PartyHud's sheet/inventory/tooltip surfaces).
void DrawPanelFace(UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect);

// Draws a button FACE — the one button look (state fill, border, centered
// label). ui::Button routes through it, and so does every hand-drawn chrome
// button (the map editor's header/dock buttons), so hover reads the same
// everywhere. `held` (or an active row) fills controlActive, hover controlHot;
// a disabled button flattens to the panel fill with dim text and ignores `hot`.
// With a `skin` (UI/Skin.h) the face is the skin's button part instead —
// hot/held wash the theme's control colors over it, disabled dims the tint —
// so state still reads through the user's theme. Null skin = the flat look
// (kept as debug mode); hand-drawn chrome callers pass their owner's skin.
void DrawButtonFace(gfx::SpriteBatch& batch, Font& font, const gfx::Rect& rect,
					const std::string& label, const Theme& theme, bool hot,
					bool held = false, bool enabled = true,
					const Skin* skin = nullptr);

// The standard close affordance every dialog uses: a small square button in the
// top-right CORNER of `panel` (window-fraction space, like the widgets it joins).
// `icon` is the shared close box (assets/ui/icon_close); a null icon falls back
// to a text "x". Returns the button (owned by `ui`). The rule is one place so
// every dialog closes the same way — top-right, never a footer button.
gfx::Rect CloseButtonRect(const gfx::Rect& panel);
Button* AddCloseButton(UIContext& ui, const gfx::Rect& panel,
					   const gfx::Texture* icon, std::function<void()> onClose);

} // namespace dungeon::ui
