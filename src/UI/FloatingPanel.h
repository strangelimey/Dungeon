// ============================================================================
// UI/FloatingPanel.h - panels the player moves and resizes (ui-panels P3a).
//
// A FloatingPanel is a top-level piece of HUD - the party bar, a plate, a
// dock - whose place is the PLAYER'S, not the layout's. Michael
// (docs/ui-panels-notes.md): "each bar/panel is a floating panel that can be
// independently moved and resized."
//
// HOLD CTRL TO ARRANGE (Michael, 2026-09-30, after the first feel pass: the
// grips appeared on every hover). Without Ctrl a panel is just its content.
// With Ctrl held over it, the panel is outlined, shows a move cross at its
// top-left, a resize wedge at its bottom-right and a RESET button at its
// top-right, and takes the whole pointer: a drag anywhere on it moves it, a
// drag on the wedge scales it, a click on reset puts EVERY panel back
// (FloatingLayer::onResetAll). A drag, once started, runs to the button's
// release whether or not Ctrl is still down.
//
// SNAPPING: a moved panel's edges snap to any other panel's edges (and the
// window's) that come within half a rem, and a resized panel's right or bottom
// edge does the same - the scale is solved for the edge. A thin accent guide
// shows the edge it caught.
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
// INPUT. Without Ctrl the content has the pointer, and the panel claims only
// what is left (UpdateSelf, after the children) - it is an opaque surface, and
// a click on a dock's padding used to fall through to the 3D view behind it.
// With Ctrl the panel claims it FIRST (UpdateBeforeChildren), over its whole
// rect, so no button inside takes an arranging click.
// ============================================================================
#pragma once

#include "UI/Widget.h"

#include <functional>
#include <string>

namespace dungeon::ui {

class FloatingLayer;

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
	// The scale becomes the subtree's fontScale. Off for a panel whose whole
	// CONTEXT scales with it instead (the character sheet has its own UIContext,
	// so its scale is the context's root font size - rem itself moves - and a
	// fontScale on top would apply it twice).
	bool scalesText = true;
	// A move or resize ended (the app saves).
	std::function<void()> onChanged;

	// What the pointer shape should be for this panel right now: 0 none,
	// 1 move, 2 resize. The app turns it into a window cursor. (Over the reset
	// button it is 0: that is a button, not a grip.)
	int CursorWanted() const { return m_cursor; }
	bool Dragging() const { return m_drag != Drag::None; }

	// Within this panel's own limits, whatever the stored value says (a Settings
	// slider spans 0.5..1.5 for every panel; the sheet stops short of filling
	// the window).
	float Scale() const {
		const float s = scale ? *scale : 1.0f;
		return s < minScale ? minScale : (s > maxScale ? maxScale : s);
	}
	// The em a panel's content measures in at `s` - the font its subtree will
	// resolve, asked of the library exactly as Widget::Layout will ask it.
	float EmAt(UIContext& ctx, float s) const;

protected:
	void UpdateBeforeChildren(UIContext& ctx) override;
	void UpdateSelf(UIContext& ctx) override;
	void DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	friend class FloatingLayer;
	enum class Drag { None, Move, Resize };
	bool Unlocked() const { return !locked || !*locked; }
	gfx::Rect GripRect(int corner) const; // 0 top-left, 1 top-right, 3 bottom-right
	void StartDrag(Drag kind, float mx, float my);
	// The snaps, in absolute pixels: shift a moved rect onto the nearest edge
	// in reach; solve a resized panel's scale for its right or bottom edge.
	void SnapMove(float& x, float& y, float w, float h, float reach);
	float SnapResize(UIContext& ctx, float s, float reach);
	// Every rect a panel can snap to: the other shown panels of this layer and
	// of the layer's peer, and the window. `fn(rect, isWindow)`.
	template <typename Fn> void ForEachSnapTarget(Fn&& fn) const;
	void DrawResetGlyph(gfx::SpriteBatch& batch, const gfx::Rect& r, const Vec4& ink) const;

	FloatingLayer* m_layer = nullptr; // the layer that placed it (set each layout)
	Drag m_drag = Drag::None;
	bool m_arranging = false;  // Ctrl held over this panel: outlined, grips up
	bool m_resetHot = false;   // ... and the pointer is on the reset button
	int m_cursor = 0;
	float m_grabX = 0.0f, m_grabY = 0.0f; // pointer at the press
	float m_startX = 0.0f, m_startY = 0.0f; // panel top-left at the press (px)
	float m_startW = 0.0f, m_startH = 0.0f; // panel size at the press (px)
	float m_startScale = 1.0f;
	// The edges the drag snapped to this frame, as hairline guides (w or h 0 =
	// none): a vertical line for an x snap, a horizontal one for a y snap.
	gfx::Rect m_guideX{}, m_guideY{};
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

	// The reset button on an arranging panel: every panel back to its default
	// spot and size. Null = no reset button.
	std::function<void()> onResetAll;
	// The reset button's tooltip (localized by the app).
	std::string resetTip;
	// Another layer whose panels this one's snap to - the character sheet has
	// its own UIContext, but it floats over the HUD's panels. Asked at each
	// drag, since the other context rebuilds its layer.
	std::function<const FloatingLayer*()> snapPeer;

protected:
	void LayoutSelf(UIContext& ctx) override;
};

} // namespace dungeon::ui
