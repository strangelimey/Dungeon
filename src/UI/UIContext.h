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
#include "UI/FontLibrary.h"
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
	// Owns its own Font, loaded from `fontPath` (empty = the system fallback).
	// The pre-FontLibrary form, kept while the remaining owners migrate.
	UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
			  float fontHeight);
	// Draws in `role`, resolved through the shared library — the form to use.
	// `library` must outlive the context.
	UIContext(FontLibrary& library, FontRole role, float fontHeight);

	// Re-resolves this context's font: a new size (window resize) or, once the
	// audition can swap faces live, a new face for the same role. Cheap enough
	// to call every frame; a no-op in the owned-Font form, which keeps its size
	// through Font::SetHeight instead.
	void UseFont(FontRole role, float pixelHeight);

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

	// This context's ROOT font — the document's 1rem (UI/Units.h). A widget
	// drawing in a different role asks for TextFont() instead; rem never moves.
	Font& GetFont() { return *m_font; }
	// Const path, for the rem/em units (UI/Units.h) and other const rect maths.
	const Font& GetFont() const { return *m_font; }

	// The font for `role` at THIS context's authored size — how a widget with a
	// font role resolves (Widget::fontRole). The role's optical scale is applied
	// by the library, so a Script label sits on the same baseline grid as the
	// Body text around it. In the owned-Font form there is no library and every
	// role resolves to the one font.
	const Font& FontFor(FontRole role) const;

	// A role at an EXPLICIT size — for text that is deliberately larger than the
	// document, like a heading or a portrait's initial. Roles carry a face, not
	// a size, so anything bigger than the body has to say how much bigger; say
	// it in rem (Widget::Rem) and it keeps tracking the window like everything
	// else. The role's optical scale still applies.
	const Font& FontAt(FontRole role, float pixelHeight) const;

	// The size this context was AUTHORED at, before any role's optical scale.
	// This — not GetFont().Height() — is what to multiply when asking FontAt for
	// a deliberately larger face: Get applies the optical scale itself, so
	// scaling an already-scaled height would apply it twice, and a Script or
	// Display root would drift further off the grid the bigger it got.
	float DesignHeight() const { return m_designHeight; }

	FontRole RootRole() const { return m_role; }
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

	// The POINTER — position and buttons. A widget claims it when the cursor is
	// over it, so a click cannot also land on whatever is behind.
	bool IsMouseConsumed() const { return m_mouseConsumed; }
	void ConsumeMouse() { m_mouseConsumed = true; }
	// Puts the claim back. ONLY for the update walk's clip handling, which
	// suppresses the pointer through a subtree the cursor is outside of and has
	// to un-suppress it afterwards — a widget must never hand back a claim it
	// made, or the thing behind it gets the same click.
	void SetMouseConsumed(bool consumed) { m_mouseConsumed = consumed; }

	// The WHEEL, claimed separately — and that separation is the point. Nearly
	// every ConsumeMouse call is a hover claim by a control that has no use for
	// the wheel at all (a Slider, a Button, a Checkbox). While the two shared
	// one flag, resting the cursor on any of them stopped the page underneath
	// scrolling: the settings page is mostly sliders, and the wheel did nothing
	// over most of it. A widget that SCROLLS claims this one; a widget that
	// merely wants the click does not, and the wheel falls through to whatever
	// can use it. A modal (an open popup) claims both.
	bool IsWheelConsumed() const { return m_wheelConsumed; }
	void ConsumeWheel() { m_wheelConsumed = true; }

private:
	// Exactly one of these backs m_font: an owned Font (legacy form) or one
	// borrowed from the library. Library fonts live as long as the library, so
	// the borrowed pointer is stable (see FontLibrary.h "LIFETIME").
	std::unique_ptr<Font> m_ownedFont;
	FontLibrary* m_library = nullptr;
	FontRole m_role = FontRole::Body;
	Font* m_font = nullptr;
	// The size this context was AUTHORED at, before any role's optical scale.
	// Kept so FontFor can resolve a second role at the same authored size —
	// asking with m_font->Height() would apply the root role's scale twice.
	float m_designHeight = 16.0f;
	Theme m_theme;
	const Skin* m_skin = nullptr;
	// Covers the window; every top-level widget is one of its children.
	Widget m_root;
	const Input* m_input = nullptr;
	bool m_mouseConsumed = false;
	bool m_wheelConsumed = false;
	float m_width = 1.0f;
	float m_height = 1.0f;
	float m_mouseX = 0.0f;
	float m_mouseY = 0.0f;
};

} // namespace dungeon::ui
