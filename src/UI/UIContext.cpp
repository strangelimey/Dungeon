#include "UI/UIContext.h"

#include "Core/Log.h"
#include "UI/Skin.h"
#include "UI/TreeInspector.h"
#include "UI/Widget.h"

namespace dungeon::ui {

namespace {

// Every walk starts with no clip (code-review C208). Each push restores itself
// (ScopedClip), so a clip still in force here is a scope that never ended - a
// walk entered from inside another, say. It is dropped either way, and said
// ONCE, since a silent fallback is how a defect like this survives. True when
// one was dropped.
bool StartWalkUnclipped(const char* walk) {
	static bool s_said = false;
	if (!ResetClip()) return false;
	if (!s_said) {
		s_said = true;
		log::Warn("ui: a clip was still in force as {} walk began - dropped (a push "
				  "that was never restored?)",
				  walk);
	}
	return true;
}

} // namespace

UIContext::UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
					 float fontHeight)
	: m_ownedFont(std::make_unique<Font>(device, fontPath, fontHeight)) {
	m_font = m_ownedFont.get();
	m_designHeight = fontHeight;
	m_root.bounds = {0, 0, 1, 1};
	m_root.debugName = "root";
}

UIContext::UIContext(FontLibrary& library, FontRole role, float fontHeight)
	: m_library(&library), m_role(role), m_font(&library.Get(role, fontHeight)),
	  m_designHeight(fontHeight) {
	m_root.bounds = {0, 0, 1, 1};
	m_root.debugName = "root";
}

void UIContext::UseFont(FontRole role, float pixelHeight) {
	if (!m_library) return; // owned-Font form; the owner drives Font::SetHeight
	m_role = role;
	m_designHeight = pixelHeight;
	m_font = &m_library->Get(role, pixelHeight);
}

const Font& UIContext::FontFor(FontRole role) const {
	if (!m_library || role == m_role) return *m_font;
	return m_library->Get(role, m_designHeight);
}

const Font& UIContext::FontAt(FontRole role, float pixelHeight) const {
	if (!m_library) return *m_font; // owned-Font form: one font, one size
	return m_library->Get(role, pixelHeight);
}

void UIContext::Update(const Input& input, float width, float height) {
	StartWalkUnclipped("an update");
	m_input = &input;
	// A popup open last frame holds the pointer from the first widget on (see
	// ClaimPopup); it renews the claim during this walk if it is still open.
	m_mouseConsumed = m_popupClaimNext;
	m_wheelConsumed = m_popupClaimNext;
	m_popupClaimNext = false;
	m_width = width;
	m_height = height;
	m_mouseX = input.MouseX();
	m_mouseY = input.MouseY();
	const gfx::Rect window{0, 0, width, height};
	// Resolve the whole tree, then walk it for input (children before their
	// parent, in reverse add order — see Widget.h).
	m_root.Layout(window, *this);
	m_root.Update(*this);
	m_input = nullptr;
}

void UIContext::Render(gfx::SpriteBatch& batch, float width, float height) {
	// A stale clip's scissor goes with it; with none, the batch's is the
	// caller's, and left alone.
	if (StartWalkUnclipped("a draw")) batch.SetScissor(nullptr);
	m_width = width;
	m_height = height;
	const gfx::Rect window{0, 0, width, height};
	m_root.Layout(window, *this);
	// Armed by the dev console; checks THIS tree's siblings for overlapping
	// areas. Between Layout and Draw so it reads the rects that are about to be
	// drawn, and so every context is covered with no per-caller wiring.
	inspect::RunOverlapAudit(*this);
	// Text on stone carries the skin's outline - every glyph the tree draws,
	// whichever of the ~110 draw sites it comes from. Put back afterwards, so a
	// flat context (the editor dialogs, the console) drawn next is untouched.
	const Vec4 outline = batch.TextOutline();
	if (m_skin) batch.SetTextOutline(m_skin->textOutline);
	m_root.Draw(*this, batch);
	m_root.DrawOverlay(*this, batch);
	batch.SetTextOutline(outline);
	// Debug view of the tree, above everything (no-op unless `uitree` is on).
	inspect::Draw(*this, batch);
}

} // namespace dungeon::ui
