// ============================================================================
// UI/FloatingPanel.h - panels the player moves and resizes (ui-panels P3a).
//
// A FloatingPanel is a top-level piece of HUD - the party bar, a plate, a
// dock - whose place is the PLAYER'S, not the layout's: dragged by its own
// background (or the move grip at its top-left) and scaled by the grip at its
// bottom-right. Michael (docs/ui-panels-notes.md): "each bar/panel is a
// floating panel that can be independently moved and resized."
//
// Two classes, because a widget's pixel rect comes from its PARENT's layout:
//   FloatingLayer   a window-sized container; its LayoutSelf places every
//                   FloatingPanel child from the panel's saved spot, or its
//                   default, at the size its content asks for at its scale,
//                   kept inside the window.
//   FloatingPanel   one panel. Its content is an ordinary child filling it
//                   (bounds 0,0,1,1); the panel adds only placement, the
//                   grips and the drags.
//
// THE STATE IS THE APP'S. A panel holds POINTERS to its position (top-left as
// window fractions, negative = not moved yet: use the default) and its scale,
// so the settings own them and a Settings slider and a corner drag edit the
// same number. `onChanged` fires once when a drag ends, which is when the app
// saves.
//
// SIZE IS THE CONTENT'S. `size(ctx, scale)` answers in pixels - a HUD dock's
// height follows from its width (square cells), so only the content can say.
// The scale also becomes the panel's inherited fontScale, so text and every
// em-sized detail inside grow with it.
//
// INPUT. Real controls inside a panel still win: the grips claim the pointer
// BEFORE the children (UpdateBeforeChildren) but only over their own small
// squares, and a background drag starts in UpdateSelf, AFTER the children, so
// a press a button took never moves the panel. A panel claims the pointer over
// its whole rect - it is an opaque surface, and a click on a dock's padding
// used to fall through to the 3D view behind it.
// ============================================================================
#pragma once

#include "UI/Widget.h"

#include <functional>

namespace dungeon::ui {

class FloatingPanel : public Widget {
public:
	// Panels may sit over one another wherever the player drags them, so their
	// overlap is deliberate; `uioverlap` still audits everything INSIDE each.
	FloatingPanel() { overlapOk = true; }

	// The panel's size in PIXELS at `scale`. Required.
	std::function<Vec2(UIContext&, float scale)> size;
	// Where the top-left goes (pixels) while the panel has never been moved.
	std::function<Vec2(UIContext&)> defaultPos;
	// Shown at all? Asked every layout (the Magic dock appears only once a
	// member knows a symbol). Null = always.
	std::function<bool()> shownWhen;

	// The saved placement (owned by the app's settings). posX/posY < 0 = use
	// defaultPos. scale null = 1, and the resize grip is not offered.
	float* posX = nullptr;
	float* posY = nullptr;
	float* scale = nullptr;
	float minScale = 0.5f;
	float maxScale = 1.5f;
	// Locked = no grips, no drags (Settings -> UI "Lock HUD layout").
	const bool* locked = nullptr;
	// A move or resize ended (the app saves).
	std::function<void()> onChanged;

	// What the pointer shape should be for this panel right now: 0 none,
	// 1 move, 2 resize. The app turns it into a window cursor.
	int CursorWanted() const { return m_cursor; }
	bool Dragging() const { return m_drag != Drag::None; }

	float Scale() const { return scale ? *scale : 1.0f; }
	// The em a panel's content measures in at `s` - the font its subtree will
	// resolve, asked of the library exactly as Widget::Layout will ask it.
	float EmAt(UIContext& ctx, float s) const;

protected:
	void UpdateBeforeChildren(UIContext& ctx) override;
	void UpdateSelf(UIContext& ctx) override;
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	enum class Drag { None, Move, Resize };
	bool Unlocked() const { return !locked || !*locked; }
	gfx::Rect MoveGrip() const;
	gfx::Rect ResizeGrip() const;
	void StartDrag(Drag kind, float mx, float my);

	Drag m_drag = Drag::None;
	bool m_hover = false;
	int m_cursor = 0;
	float m_grabX = 0.0f, m_grabY = 0.0f; // pointer at the press
	float m_startX = 0.0f, m_startY = 0.0f; // panel top-left at the press (px)
	float m_startW = 0.0f, m_startH = 0.0f; // panel size at the press (px)
	float m_startScale = 1.0f;
};

class FloatingLayer : public Widget {
public:
	// Window-sized and invisible: it paints nothing, so sitting "over" the
	// message log or anything else beside it is not an overlap. What is inside
	// each panel is still audited.
	FloatingLayer() {
		debugName = "FloatingLayer";
		overlapOk = true;
	}

protected:
	void LayoutSelf(UIContext& ctx) override;
};

} // namespace dungeon::ui
