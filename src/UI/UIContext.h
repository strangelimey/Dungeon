// ============================================================================
// UI/UIContext.h — the retained-mode widget tree.
//
// The context owns a ROOT widget covering the window; UIContext::Add creates a
// top-level widget as a child of it, and those widgets may in turn own children
// of their own (see Widget.h — a child's bounds are fractions of its PARENT).
// The game creates widgets once and keeps raw pointers to the ones it updates
// (labels, the message log). Both passes take the current window size and
// resolve the whole tree's pixel rects on the fly, so the UI scales with the
// screen. Per frame:
//   Update(input, w, h): the tree is laid out, then walked for input — deepest
//     and topmost first (children in REVERSE add order before their parent).
//     A widget that uses the mouse calls ConsumeMouse() so widgets visited
//     later ignore the same event. Keyboard input is not consumed — the party
//     always receives movement keys.
//   Render(batch, w, h): the tree draws parent-behind-children in add order,
//     then a second DrawOverlay pass lets popups (open drop-downs) paint above
//     everything.
// ============================================================================
#pragma once

#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/Widget.h"

#include <memory>
#include <vector>

namespace dungeon::ui {

struct Skin;

// Theme colors shared by all controls.
struct Theme {
	Vec4 panel{0.08f, 0.07f, 0.06f, 0.85f};
	Vec4 panelBorder{0.45f, 0.38f, 0.25f, 1.0f};
	Vec4 control{0.18f, 0.15f, 0.12f, 1.0f};
	Vec4 controlHot{0.30f, 0.25f, 0.18f, 1.0f};
	Vec4 controlActive{0.42f, 0.34f, 0.22f, 1.0f};
	Vec4 text{0.92f, 0.88f, 0.80f, 1.0f};
	Vec4 textDim{0.62f, 0.58f, 0.50f, 1.0f};
	Vec4 accent{0.85f, 0.65f, 0.25f, 1.0f};
};

// Owns the widget tree and routes input/drawing. Later-added widgets draw on
// top and receive input first.
class UIContext {
public:
	UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
			  float fontHeight);

	// Creates a TOP-LEVEL widget — a child of the root, so its bounds are
	// fractions of the window. Nest deeper with Widget::Add on the parent.
	template <typename T, typename... Args>
	T* Add(Args&&... args) {
		return m_root.Add<T>(std::forward<Args>(args)...);
	}

	// Destroys every widget (e.g. to rebuild a page in a new language). Any
	// raw pointers handed out by Add are dead; never call from inside a
	// widget callback — the widget is still on the Update stack.
	void Clear() { m_root.ClearChildren(); }

	// The window-sized root of the tree (the inspector walks it).
	Widget& Root() { return m_root; }
	const Widget& Root() const { return m_root; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, float width, float height);

	Font& GetFont() { return m_font; }
	const Theme& GetTheme() const { return m_theme; }
	void SetTheme(const Theme& theme) { m_theme = theme; }

	// Textured chrome (UI/Skin.h). Null = the flat theme-fill look, which is
	// kept as the DEBUG MODE (containment/extents read at a glance). The owner
	// keeps the Skin (and its textures) alive; widgets re-check every draw, so
	// the toggle is live.
	const Skin* GetSkin() const { return m_skin; }
	void SetSkin(const Skin* skin) { m_skin = skin; }

	// Window size from the most recent Update/Render — lets widgets clamp
	// popups to the screen.
	float Width() const { return m_width; }
	float Height() const { return m_height; }

	// Pointer position from the most recent Update. Render has no Input of its
	// own, so the tree inspector reads the cursor from here.
	float MouseX() const { return m_mouseX; }
	float MouseY() const { return m_mouseY; }

	// Input routing state (used by widgets during Update).
	const Input* CurrentInput() const { return m_input; }
	bool IsMouseConsumed() const { return m_mouseConsumed; }
	void ConsumeMouse() { m_mouseConsumed = true; }

private:
	Font m_font;
	Theme m_theme;
	const Skin* m_skin = nullptr;
	// Covers the window; every top-level widget is one of its children.
	Widget m_root;
	const Input* m_input = nullptr;
	bool m_mouseConsumed = false;
	float m_width = 1.0f;
	float m_height = 1.0f;
	float m_mouseX = 0.0f;
	float m_mouseY = 0.0f;
};

} // namespace dungeon::ui
