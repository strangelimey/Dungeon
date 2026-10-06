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

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dungeon::ui {

struct Skin;
class ScrollArea; // defined below; SlotList holds one

// A small square of a texture, or of a flat colour when the texture is not
// there (DrawSwatch, below). Empty - no texture and a clear colour - draws
// nothing and takes no room.
struct Swatch {
	const gfx::Texture* icon = nullptr;
	Vec4 color{0.0f, 0.0f, 0.0f, 0.0f};
	bool Empty() const { return !icon && color.w <= 0.0f; }
};

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
	// The background's opacity, read live every draw from whoever owns it (a
	// HUD plate's Settings slider). Null = opaque.
	const float* opacity = nullptr;
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
	// A Label draws its whole string from its top-left corner, however narrow
	// its bounds — so what it paints is measured, not bounded (Widget::InkRect).
	gfx::Rect InkRect() const override;

	std::string text;
	bool dim = false;
	// Draws in the theme's accent instead — for a line that has to be NOTICED
	// (a dialog's refusal or warning). Wins over `dim`; both take the color from
	// the live theme, never a captured copy, so the user's palette applies.
	bool accent = false;
	// Center the text vertically in the bounds instead of drawing from the top.
	// For a label that shares a row with a control: the control fills the row's
	// height, and top-aligned text beside it sits high.
	bool centerV = false;
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
	// The tooltip, when hovered (see `tooltip`).
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The face is bounded but the label is CENTRED on it and measured, so a
	// label wider than the button (or a font taller than it) runs out of both
	// sides at once. See Label.
	gfx::Rect InkRect() const override;

	std::string text;
	std::function<void()> onClick;
	// Shown in a small box while the pointer is over the button - above it, or
	// below when there is no room - in the overlay pass, so it covers whatever
	// is next to the button. For an ICON button it is the button's name (the
	// face shows none); empty = no tooltip.
	std::string tooltip;
	// Draw as selected (controlActive fill) regardless of hover — for a row that
	// represents the current selection in a list (the config dialog's state/clip rows).
	bool active = false;
	// A disabled button still PAINTS its pixels, so it still claims the mouse
	// there (nothing underneath may take a click through it) — it just does
	// nothing with it, and draws flattened. For an action waiting on a
	// condition the user can watch being met, like a delete confirmation's name.
	bool enabled = true;
	// Optional icon face drawn centered INSTEAD of the label (the text stays
	// the fallback when the texture is missing). `iconTurns` rotates it in
	// quarter turns clockwise, so one chevron asset serves every direction
	// (the HUD movement pad).
	const gfx::Texture* icon = nullptr;
	int iconTurns = 0;
	// An icon drawn ON the button's face in place of its label (ui-updates):
	// the face keeps its stone, bevel and push, the icon - a white glyph,
	// assets/ui/glyph_<name>.png from tools/BuildToolIcons.py - is tinted with
	// the label's colour (dimmed when disabled) and sinks with it. Put the words
	// in `tooltip`; `text` stays the fallback when the texture is missing.
	const gfx::Texture* faceIcon = nullptr;
	// CUT STONE (more-ui-updates): with `etch` set and a skin that has the block
	// part, the button IS a cut-stone block with that symbol etched into it
	// (ui::DrawCutStone; assets/ui/etch_<name>.png). `etchLit` - the gold lit -
	// replaces it while `active`, which also holds the block down: a current
	// tab is both sunk and lit. The icon / label paths are the fallback (no
	// skin, or the flat debug look).
	const gfx::Texture* etch = nullptr;
	const gfx::Texture* etchLit = nullptr;
	// Fire the action on the PRESS instead of at the bottom of the sink
	// (Michael: the movement stones and the tabs act at once). The push still
	// plays, all of it, and never delays the action; a press dragged off before
	// release has already acted, so it completes rather than cancelling.
	bool fireOnPress = false;
	// CARVED WORDS on a cut stone (more-ui-updates: the save, load and world
	// pages - Michael: "that needs the same treatment"): with a skin that has
	// the block part, a text button draws as the menus' entries do, its label
	// carved in gold, lit while the pointer is over it. No skin = the plain face.
	bool carved = false;
	// Plays the push with no click and no action - the movement pad presses a
	// stone when the KEYBOARD moves the party. Ignored while the mouse holds it.
	void PressVisual();
	// Under the pointer (and the pointer not taken) as of the last Update - for
	// a subclass drawing its own fallback face.
	bool Hot() const { return m_hot; }

	// THE PUSH (Michael, ui-updates: "animate as pushed, execute the action,
	// then animate back"). A click does not fire the moment the button is
	// released: the face SINKS into the pushed look (the label a pixel lower; an
	// icon face shrinks a touch), the action runs at the BOTTOM of the press,
	// and the face RISES back. So even a flick of a click is seen to land, and
	// every button in the game reads alike. Timed on the steady clock - real
	// time, allocation-free - and stepped from Update, so the callback still
	// runs where callbacks always have (a page rebuild still defers itself).
	static constexpr float kSinkSeconds = 0.07f;  // up -> fully pushed
	static constexpr float kHoldSeconds = 0.05f;  // held at the bottom, then fire
	static constexpr float kRiseSeconds = 0.10f;  // pushed -> up, after the action

private:
	using Clock = std::chrono::steady_clock;
	enum class Push { None, Sinking, Rising };
	// How far down the face is: 0 up, 1 fully pushed.
	float Depth() const;

	bool m_hot = false;
	bool m_held = false;
	// The push in flight has ALREADY acted (fireOnPress, or a PressVisual), so
	// the bottom of its sink must not act again.
	bool m_fired = false;
	Push m_push = Push::None;
	Clock::time_point m_pressAt{}; // the sink's start (the press)
	Clock::time_point m_riseAt{};  // the rise's start (just after the action)
};

// A labeled on/off box: a small square at the left with the label to its right;
// clicking anywhere in the row toggles it and fires onChange with the new state.
// `highlight` draws the row selected (independent of the check) so it can double
// as a list row. The owner reads Checked()/SetChecked() to sync external state.
// A non-empty `swatch` draws between the box and the label, the height of the
// row, for a list of things that have a look (textures).
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
	// The box is bounded but the label beside it is measured — see Label.
	gfx::Rect InkRect() const override;

	std::string label;
	std::function<void(bool)> onChange;
	bool highlight = false; // draw the row highlighted (e.g. selected/previewed)
	Swatch swatch;          // empty = none

private:
	// Where the label starts, past the box and any swatch (DrawSelf and
	// InkRect both ask, so the measured ink matches the drawn row).
	float TextX(const gfx::Rect& px) const;
	// The box's side: big enough that a sunken field face still shows its well
	// round the tick (at 0.65rem the frame and the tick filled it solid).
	float BoxSide(const gfx::Rect& px) const { return std::min(px.h * 0.75f, Rem(0.9f)); }

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
	// Moves the thumb to a value changed ELSEWHERE (a HUD panel resized by its
	// corner grip) without firing onChange. A no-op mid-drag, where the
	// player's hand is the authority. Rebuilds the readout - not per frame.
	void SetValue(float v) {
		if (m_dragging) return;
		v = v < m_min ? m_min : (v > m_max ? m_max : v);
		if (v == m_value) return;
		m_value = v;
		RefreshDisplay();
	}
	// Places shown after the point in the readout (default 2). 0 for a slider
	// over whole numbers, which otherwise reads "Rooms: 8.00".
	void SetDecimals(int places) {
		m_decimals = places;
		RefreshDisplay();
	}
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
	int m_decimals = 2;
	std::string m_display;
	bool m_dragging = false;
};

// Labeled selector whose open list is drawn as an overlay, so it covers the
// widgets laid out after it. The list is CLAMPED to the window: it opens below
// the control, flips above when there is more room there, and scrolls (wheel or
// thumb drag, like SlotList) when it still doesn't fit — an installed-asset list
// is as long as the pool, and it used to run off the bottom of the screen.
//
// The FACE never paints outside the control: a selection wider than the room
// left of the expander is trimmed with ".." (FitText), says itself in full in a
// tooltip while hovered, and the open list widens to its longest item (within
// the window), so nothing is ever unreadable. The trim is still a layout defect
// - the control was not given room for what it shows - so it reports itself to
// the overlap audit through TextOverrun.
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
	// The open list, or the hovered face's tooltip when its text was trimmed.
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The face is bounded sideways (the text is trimmed) but the line is CENTRED
	// on it at the font's height, so a row shorter than the font spills the text
	// out of the top and bottom - a Button's rule.
	gfx::Rect InkRect() const override;
	float TextOverrun() const override;

	std::vector<std::string> items;
	std::function<void(int)> onSelect;
	// Optional PICTURES, parallel to `items` (empty = a plain text list; a null
	// entry = no picture on that row). Each draws as a square before its text,
	// on the face and in the open list - whose rows grow to `iconRowScale` x
	// the face's height, so the pictures are big enough to tell apart (the
	// Level dialog's UI material thumbnails). Not owned.
	std::vector<const gfx::Texture*> icons;
	float iconRowScale = 2.2f;
	// Optional CATEGORY buttons pinned along the top of the open list (Michael,
	// for the Level dialog's materials: All / Light / Dark / Stone / Wood ...).
	// `filterLabels` names them; `itemFilters`, parallel to `items`, says which
	// rows each passes - bit f set = shown under button f. A row past the end
	// of itemFilters passes them all. The pick lasts while the control does.
	std::vector<std::string> filterLabels;
	std::vector<unsigned> itemFilters;
	// Each button's COLOUR CHIP, parallel to filterLabels: a small swatch
	// before its label that hints at the category (light grey for Light, green
	// for Forest). Alpha 0, or past the end, = no chip.
	std::vector<Vec4> filterColors;
	// The buttons' text size against the list's: they are captions, and at the
	// list's own size eight of them took three lines of a dialog-sized list.
	float filterScale = 0.68f;

protected:
	void LayoutSelf(UIContext& ctx) override;

private:
	// The selected item's text ("" for none), a reference into `items`.
	const std::string& Current() const;
	// An open-list row's height: the face's, or iconRowScale x it with icons.
	float RowH() const;
	// The picture's square in a row `rowH` tall, and how far it pushes the text
	// right (0 with no icons). Face and list both ask, so they cannot disagree.
	float IconSide(float rowH) const;
	float IconLead(float rowH) const;
	const gfx::Texture* IconAt(int index) const;
	void DrawIcon(gfx::SpriteBatch& batch, const Theme& theme, int index,
				  const gfx::Rect& row) const;
	// The category filter: whether row `item` shows under the current button,
	// and how many rows do. With no buttons every row shows.
	bool Passes(size_t item) const;
	size_t ShownCount() const;
	// The category buttons, laid left to right across the top of a popup and
	// wrapping: calls f(index, rect) for each, returns the band's height (0
	// with no buttons). One walk serves the layout, the hit test and the draw.
	template <class F>
	float ForEachChip(const gfx::Rect& popup, F&& f) const;
	// The popup below the button band - where the rows scroll.
	gfx::Rect ListRect(const gfx::Rect& popup) const;
	// Where the face text starts, and how wide it may run before the expander.
	// DrawSelf, InkRect and TextOverrun all ask, so the measure is the draw.
	float TextX() const;
	float TextRoom() const;

	// The open list's box, clamped to the window (below the control, or above it
	// when that side has more room), and at least as wide as its longest item
	// (m_popupTextW) where the window allows. Everything else resolves against it.
	gfx::Rect PopupRect(const UIContext& ctx) const;
	// The `slot`-th SHOWN row (the category filter hides some; slot counts
	// only the ones that pass), scrolled, in the list area.
	gfx::Rect ItemRect(const gfx::Rect& popup, size_t slot) const;
	float MaxScroll(const gfx::Rect& popup) const;
	gfx::Rect ScrollTrackRect(const gfx::Rect& popup) const;
	gfx::Rect ScrollThumbRect(const gfx::Rect& popup, float maxScroll) const;

	int m_selected = 0;
	int m_hoverItem = -1;
	int m_filter = 0;     // the category button in force
	int m_hoverChip = -1; // the category button under the pointer
	// The buttons' font (filterScale x this control's), resolved at Layout so
	// the const layout walk can measure in it. Null until then / with no buttons.
	const Font* m_chipFont = nullptr;
	const Font& ChipFont() const { return m_chipFont ? *m_chipFont : TextFont(); }
	bool HasChip(size_t i) const;    // button i has a colour chip
	float ChipWidth(size_t i) const; // button i's whole width
	bool m_open = false;
	bool m_hot = false;
	float m_scroll = 0.0f; // pixels scrolled down the open list
	bool m_scrollHot = false;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f; // pointer offset within the thumb while dragging
	// The widest item's text, measured when the list opens (never per frame -
	// a pool-length list is hundreds of rows).
	float m_popupTextW = 0.0f;
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
// characters (WM_CHAR via Input::TypedChars, UTF-8), Backspace deletes the last
// whole character, and Enter fires onSubmit. Clicking outside the box unfocuses
// it. A solid caret marks focus (no blink - widget Update has no time step). The
// owner reads/sets `text` directly; onChange fires whenever it changes by input.
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
	size_t maxLength = 32;            // in CHARACTERS, not bytes (utf8::Length)
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
// reuses: Begin() at the click point, Add()/AddGroup() the rows for whatever
// was right-clicked, then Show() (a menu with no rows does not open).
//
// ALLOCATION-FREE. A context menu opens mid-game from a click, in exactly the
// settled frames the steady-state guard watches, so it owns a FIXED pool of
// rows with inline labels, and a row carries an int id rather than a closure.
// The owner sets `onPick` once and keeps what the ids mean itself. The old
// shape - a vector of entries, each a std::string label plus a std::function
// capturing strings - allocated dozens of times per open and again per pick.
// Rows past kMaxRows are dropped and labels past kLabelCapacity are clipped,
// rather than growing.
//
// SCREEN-ANCHORED, not parent-relative: it opens at an absolute pixel point and
// draws in the OVERLAY pass, so `bounds` stays zero — a context menu must not be
// clipped or placed by whatever happens to own it. The tree inspector therefore
// shows it as 0x0, which is correct rather than a missing rect.
class ContextMenu : public Widget {
public:
	static constexpr size_t kMaxRows = 40;       // top level + submenus together
	static constexpr size_t kLabelCapacity = 63; // bytes of UTF-8 per label
	static constexpr int kTopLevel = -1;         // Add()'s "in no group"

	ContextMenu() = default;

	// Starts a new menu at (x,y) device pixels (clamped on screen in Update),
	// discarding the previous rows; it stays closed until Show().
	void Begin(float x, float y);
	// A leaf row reporting `id` through onPick. `group` is a handle from
	// AddGroup, placing the row in that group's submenu. False when full.
	bool Add(std::string_view label, int id, int group = kTopLevel);
	// A top-level group row; returns its handle, or kTopLevel when full.
	int AddGroup(std::string_view label);
	// Opens what Begin/Add built. No-op when nothing was added.
	void Show();
	void Close() {
		m_open = false;
		m_openChild = -1;
	}
	bool IsOpen() const { return m_open; }

	// Fired with the picked leaf's id, after the menu has closed. Set once by
	// the owner. Capture `this` alone so it fits std::function's small buffer:
	// a pick COPIES it (the callback may rebuild the menu's owner).
	std::function<void(int id)> onPick;

	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext&, gfx::SpriteBatch&) override {} // overlay-only
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	struct Row {
		char text[kLabelCapacity + 1] = {};
		size_t len = 0;
		int id = 0;
		int group = kTopLevel; // the owning group's ROW index, or top level
		bool isGroup = false;
		std::string_view Label() const { return {text, len}; }
	};

	int Append(std::string_view label, int id, int group, bool isGroup);
	// Visual position of top-level row `row` among the top-level rows.
	size_t TopPosition(int row) const;
	size_t TopCount() const;
	size_t ChildCount(int group) const;
	gfx::Rect EntryRect(size_t i) const;
	gfx::Rect ChildRect(size_t i) const; // row i of the open group's submenu
	void Pick(UIContext& ctx, int id);

	bool m_open = false;
	float m_x = 0.0f, m_y = 0.0f; // top-left, device pixels (clamped in Update)
	float m_w = 0.0f, m_rowH = 0.0f; // sized from the font in Update
	std::array<Row, kMaxRows> m_rows;
	size_t m_count = 0;
	int m_hover = -1; // a ROW index into m_rows, not a visual position
	// Cascading submenu state: which group row's children are showing (-1 =
	// none) and the submenu box, laid out beside the parent in Update.
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
	using Clock = std::chrono::steady_clock;
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;
	gfx::Rect DeleteRect() const; // square icon button at the row's right end
	float Depth() const;          // the push: 0 up .. 1 down

	std::string m_primary, m_secondary;
	std::function<void()> m_onActivate;
	std::function<void()> m_onDeleteClick;
	const gfx::Texture* const* m_icon;
	bool m_deletable;
	bool m_hot = false;
	bool m_hotDelete = false;
	// THE PUSH, as a menu entry has it (more-ui-updates): a press sinks the row,
	// the RELEASE over it completes the push and the row acts at the bottom of
	// the sink (Button's clock); released elsewhere it only rises. The delete
	// icon is not pushed - it opens its confirm on the press, as it did.
	bool m_held = false;
	bool m_sinking = false;
	bool m_rising = false;
	Clock::time_point m_pressAt{};
	Clock::time_point m_riseAt{};
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

	float rowHeight = 1.75f;                  // rem (UI/Units.h): a line plus air
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
	size_t Count() const { return m_items.size(); }
	const std::string& Label(size_t index) const { return m_items[index].label; }
	void UpdateSelf(UIContext& ctx) override;
	void DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

	// The space between two entries, in rem.
	float gapRem = 0.3f;

	// THE PUSH (more-ui-updates Phase 4: the pause and title menus, Michael):
	// an entry is pressed like any button - it sinks, ACTS ON RELEASE over it
	// (drag off to cancel; Enter / Space press the selected one), and rises,
	// on Button's clock (kSinkSeconds / kHoldSeconds / kRiseSeconds). Skinned,
	// each entry is a CUT-STONE block with its word carved in (DrawCarvedText),
	// the selected one's gold lit; the flat look keeps the accent bar.

private:
	struct Item {
		std::string label;
		std::function<void()> onActivate;
	};
	using Clock = std::chrono::steady_clock;

	gfx::Rect ItemRect(size_t index) const;
	void MoveSelection(int delta);
	void Activate(int index);
	// How far entry `index` is pushed: 0 up .. 1 down.
	float Depth(int index) const;

	std::vector<Item> m_items;
	float m_itemHeight;
	int m_selected = 0;
	int m_pushItem = -1;     // the entry being pressed, or -1
	bool m_held = false;     // the mouse still holds it
	bool m_sinking = false;  // released / Enter: completing, then it acts
	bool m_rising = false;   // acted (or cancelled): coming back up
	Clock::time_point m_pressAt{};
	Clock::time_point m_riseAt{};
};

// WORDS CARVED INTO STONE (more-ui-updates): `text` at (x, y) as an incised
// cut lit from the top-left - its near edge in shadow, its far edge catching
// the light - with `fill` (gold, for the menus) lying in it. The context's text
// outline is suspended around it, since a ring round each of the three layers
// reads as paint, not a cut. Allocation-free.
void DrawCarvedText(gfx::SpriteBatch& batch, const Font& font, std::string_view text,
					float x, float y, const Vec4& fill);
// The gold in a carved word, and the same gold lit (the selected / hovered
// stone) - one set, so every carved face in the game agrees. They follow the
// MATERIAL: each is the skin's ink, solved by ResolveInks against the stone's
// mean colour. A null skin gets the dark-stone colours.
Vec4 CarvedGold(const Skin* skin);
Vec4 CarvedLit(const Skin* skin);
// A card's title, a shade brighter than the words under it.
Vec4 CarvedTitle(const Skin* skin);
// Carved but unpainted: the cut alone, quieter than the gold - for the
// secondary words on a stone (a save's date, a world's folder).
Vec4 CarvedPlain(const Skin* skin);
// A disabled carved word: the gold faded toward the stone it is cut in.
Vec4 CarvedDisabled(const Skin* skin);
// Solves `skin`'s inks against its `stoneMean`: each authored ink is kept if its
// WCAG contrast ratio against the mean already reaches kInkContrast (the plain
// one kInkContrastPlain), else moved toward pale gold or dark bronze - whichever
// gets there with the smaller change, or the end that reads best when neither
// does (a mid-grey stone caps every colour near 5:1). The lit ink is kept
// brighter than the gold so a hover still shows. Call when the material changes.
inline constexpr float kInkContrast = 4.5f;
inline constexpr float kInkContrastPlain = 3.0f;
void ResolveInks(Skin& skin);
// The WCAG contrast ratio of two sRGB colours (1 = identical .. 21 = black on
// white); alpha is ignored. For the ink solve and `uimaterial`'s report.
float ContrastRatio(const Vec4& a, const Vec4& b);

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
	// Restores a scroll position — for a list that REBUILDS its rows and would
	// otherwise jump to the top every time its model changed. Clamped by the
	// next layout, so a position past a now-shorter list is safe to hand back.
	void SetScroll(float pixels) { m_scroll = pixels; }
	// Scrolls the least distance that brings `child` fully into the view. Reads
	// PIXEL rects, so call it after a layout has run — which is what makes it
	// safe for a caller that knows a widget but not where the layout put it (a
	// list opening on its current selection).
	void ScrollIntoView(const Widget& child);

	// The children's container: the view box, shifted up by the scroll.
	gfx::Rect ContentRect() const override;
	// The view box itself (unscrolled) — what the scroll maths and clip use.
	gfx::Rect ViewRect() const;

	// Both in REM (UI/Units.h), so the chrome tracks the text it wraps.
	float padding = 0.45f; // inset from this widget's own edge
	float gutter = 0.5f;   // scrollbar track + margin, always reserved

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

	// Grows the pool to `n` children now (hidden until counted), so the first
	// frame that needs them builds nothing. For a repeater that first shows in
	// the middle of play, where growing is an allocation in a guarded frame.
	void Warm(size_t n);

private:
	void LayoutSelf(UIContext& ctx) override;

	Factory m_factory;
	Counter m_count;
	Placer m_place;
	size_t m_live = 0;
};

// Draws a 1px border around a rectangle.
void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color);

// A soft GLOW round a rectangle: `radius` px of rings outside it, `color` at
// `strength` alpha against the edge falling away to nothing (a quadratic
// falloff, so it reads as light rather than as a second border). Makes a
// coloured mark - a member's identity border, a lit button - stand off stone
// that is close to its own value. Draw it BEFORE the thing it surrounds. The
// glow paints outside `rect`, so a caller must leave it that much room.
void DrawGlow(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color,
			  float radius, float strength);

// A GROOVE CUT INTO THE STONE round a rectangle: a channel `width` px wide,
// lying just inside `rect`, whose floor is `base` and whose walls are lit from
// the top-left like every bevel in the skin - the upper and left walls in
// shadow, the lower and right ones catching the light. So the OUTER edge is dark
// on top/left and light on bottom/right, and the INNER edge the other way round.
// Marks something as belonging to a colour without lighting it up (Michael: the
// glowing member borders were "far too bright").
void DrawCarvedGroove(gfx::SpriteBatch& batch, const gfx::Rect& rect, float width,
					  const Vec4& base);

// WHERE A TOOLTIP GOES - every tooltip in the game asks here, so none can run
// off the screen (an effect's tip on the rightmost portrait used to clip off the
// window's edge, because each tip did its own sums and most only checked one
// side). The tip sits on its `prefer` side of `anchor`, `gap` px off it; when it
// would leave `bounds` there and the opposite side has more room, it flips; then
// it is CLAMPED wholly inside `bounds`, `margin` px in, on both axes. Below and
// Above line up with the anchor by `align` (centred, its left edge, or its
// right edge), Right centres beside it. It covers the anchor only when nothing
// else fits. `bounds` is the surface the tip is drawn on - the window for a
// UIContext (ctx.Width()/Height()), the panel for the map editor.
enum class TipSide { Below, Above, Right };
enum class TipAlign { Center, Start, End };
gfx::Rect PlaceTooltip(const gfx::Rect& anchor, float w, float h, const gfx::Rect& bounds,
					   TipSide prefer, float gap, float margin = 2.0f,
					   TipAlign align = TipAlign::Center);

// TEXT FITTED TO A WIDTH. The whole of `text` when it fits in `room` pixels,
// else its longest prefix that leaves room for kTrimMark after it - cut back to
// a whole UTF-8 character, never part-way through one. `trimmed` (optional) says
// which; an empty prefix with `trimmed` set means not even the mark fits. A VIEW
// into `text`, so fitting allocates nothing: a drop-down's face fits its text
// every frame, inside the frames the steady-state guard watches.
inline constexpr std::string_view kTrimMark = "..";
std::string_view FitText(const Font& font, std::string_view text, float room,
						 bool* trimmed = nullptr);
// Draws `text` at (x, y) fitted to `room`, the mark after a trimmed prefix (and
// nothing at all when even the mark would not fit). Allocation-free, as above.
void DrawFittedText(gfx::SpriteBatch& batch, const Font& font, std::string_view text,
					float x, float y, float room, const Vec4& color);

// Draws a Swatch filling the rect: the texture, else the flat colour. The
// editor palette's rows and a Checkbox's swatch both draw through this, so a
// type looks the same in the palette and in a dialog listing it.
void DrawSwatch(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Swatch& swatch);

// Draws the shared framed-background look: the context's skinned panel face
// (stone + bevel + sheen, UI/Skin.h) when a skin is set (the theme's panel
// alpha rides the tint so the background-opacity preference applies to both
// looks), else the flat theme fill + 1px border. Panel/TextOutput/popups route
// through it, and so does the game-layer chrome (PartyHud's sheet/inventory/
// tooltip surfaces). `opacity` fades the background on top of that (the HUD
// docks' own slider); the flat look's border stays, as the party bar's slots
// keep theirs.
void DrawPanelFace(UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
				   float opacity = 1.0f);

// Draws an ITEM SOCKET - every one, so a backpack cell, a doll slot, a bag row
// and a HUD hand box read as the same kind of hole: skinned, the stone sunk
// into a dark well (Face::Slot); flat, `flatFill` with the theme's 1px border
// (a caller wanting an accent border draws it after). Returns the WELL - the rect
// inside the frame, where the item and anything laid on the socket belong.
// `lift` (0..1) brightens the well for hover / selection.
gfx::Rect DrawSlotFace(const UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
					   const Vec4& flatFill, float lift = 0.0f);

// Draws the face of a control you put a VALUE into - a drop-down, a text
// field, a check box, a slider's groove - so all four read as one kind of
// thing. Skinned: the SELECTED SETTINGS TAB's sunken stone (Face::ButtonDown)
// under a darker veil, lifted a little when Hot, and Active (focused / open)
// edged with a soft accent glow instead of a flat yellow border. Flat (the
// debug look): `flatFill` with a 1px `flatBorder`, exactly as these controls
// drew before. Returns the rect inside the frame, where content belongs.
enum class FieldState { Normal, Hot, Active };
gfx::Rect DrawFieldFace(const UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
						FieldState state, const Vec4& flatFill, const Vec4& flatBorder);

// Draws a button FACE — the one button look (state fill, border, centered
// label). ui::Button routes through it, and so does every hand-drawn chrome
// button (the map editor's header/dock buttons), so hover reads the same
// everywhere. `held` (or an active row) fills controlActive, hover controlHot;
// a disabled button flattens to the panel fill with dim text and ignores `hot`.
// With a `skin` (UI/Skin.h) the face is the skin's button part instead —
// hot/held wash the theme's control colors over it, disabled dims the tint —
// so state still reads through the user's theme. Null skin = the flat look
// (kept as debug mode); hand-drawn chrome callers pass their owner's skin.
// `sink` (px) lowers the label, for a face caught mid-push (Button's animation).
void DrawButtonFace(gfx::SpriteBatch& batch, const Font& font,
					const gfx::Rect& rect,
					const std::string& label, const Theme& theme, bool hot,
					bool held = false, bool enabled = true,
					const Skin* skin = nullptr, float sink = 0.0f);

// Draws a drop-down's EXPANDER at the right end of `rect`: the authored box
// (ui::ControlIcons::dropDown, or its dropDownOpen twin while `open`), brightened
// while open or hovered — or the text arrow when no icon is installed. The look
// belongs to the drop-down, not to any one drawing site: DropDown routes
// through it, and so does hand-drawn chrome that presents a drop-down outside
// the widget tree (the map editor's level picker), exactly as they already
// share DrawButtonFace. `rect` is the whole CONTROL; the expander sizes and
// insets itself off `font` so it clears the border at any text size.
void DrawDropDownExpander(gfx::SpriteBatch& batch, const Font& font,
						  const gfx::Rect& rect, const Theme& theme, bool open,
						  bool hot);
// The x where that expander begins, less the gap text must keep from it: the
// right end of the room a drop-down's face text may use. Same `font` and `rect`
// as the draw, which is what keeps the two in step.
float DropDownTextRight(const Font& font, const gfx::Rect& rect);

// The standard close affordance every dialog uses: a small square button in the
// top-right CORNER of `panel` (window-fraction space, like the widgets it joins).
// `icon` is the shared close box (assets/ui/icon_close); a null icon falls back
// to a text "x". Returns the button (owned by `ui`). The rule is one place so
// every dialog closes the same way — top-right, never a footer button.
gfx::Rect CloseButtonRect(const gfx::Rect& panel);
Button* AddCloseButton(UIContext& ui, const gfx::Rect& panel,
					   const gfx::Texture* icon, std::function<void()> onClose);

// The same affordance, placed INSIDE a slot a layout reserved for it (a Stack's
// Space — UI/Layout.h). Prefer this wherever the dialog's chrome is stacked: the
// panel-fraction form floats the button over whatever happens to be beneath it,
// which is a collision waiting for a longer title, while a slot is an area the
// layout has already kept clear. The icon self-squares inside the slot.
Button* AddCloseButton(Widget& slot, const gfx::Texture* icon,
					   std::function<void()> onClose);

// How much larger than its context an editor dialog sets its two kinds of text.
// Constants rather than a size each dialog picks, for the same reason
// AddCloseButton is one helper: every dialog then reads at the same two sizes.
// Widgets take these through Widget::fontScale; raw draws take them through the
// two font helpers below. The dialogs' numeric readouts deliberately take
// NEITHER — they are sized to their digits and stay at the document size.
inline constexpr float kDialogTitleScale = 2.9f;
inline constexpr float kDialogTextScale = 2.0f;

// A dialog's TITLE face: the context's own text, enlarged. One helper for the
// same reason AddCloseButton is one helper — every dialog's title is then the
// same size — and because a title is sometimes a HIT TARGET (the level and type
// dialogs make their name a click-to-rename affordance). Measuring that in one
// size and drawing it in another puts the click somewhere the text is not, so
// the rect and the draw must ask the same function.
const Font& DialogTitleFont(const UIContext& ctx);

// A dialog's FORM face — setting names and footer buttons. The same size a
// widget gets from `fontScale = kDialogTextScale`, for the dialogs that draw
// their rows straight to the batch instead of through Label widgets.
const Font& DialogTextFont(const UIContext& ctx);

} // namespace dungeon::ui
