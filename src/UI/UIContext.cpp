#include "UI/UIContext.h"

#include "UI/Widget.h"

namespace dungeon::ui {

UIContext::UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
					 float fontHeight)
	: m_font(device, fontPath, fontHeight) {
	m_root.bounds = {0, 0, 1, 1};
	m_root.debugName = "root";
}

void UIContext::Update(const Input& input, float width, float height) {
	m_input = &input;
	m_mouseConsumed = false;
	m_width = width;
	m_height = height;
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
}

} // namespace dungeon::ui
