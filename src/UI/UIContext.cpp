#include "UI/UIContext.h"

#include "UI/TreeInspector.h"
#include "UI/Widget.h"

namespace dungeon::ui {

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

void UIContext::Update(const Input& input, float width, float height) {
	m_input = &input;
	m_mouseConsumed = false;
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
	m_width = width;
	m_height = height;
	const gfx::Rect window{0, 0, width, height};
	m_root.Layout(window, *this);
	m_root.Draw(*this, batch);
	m_root.DrawOverlay(*this, batch);
	// Debug view of the tree, above everything (no-op unless `uitree` is on).
	inspect::Draw(*this, batch);
}

} // namespace dungeon::ui
