#include "UI/UIContext.h"

#include "UI/Widget.h"

namespace dungeon::ui {

UIContext::UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
					 float fontHeight)
	: m_font(device, fontPath, fontHeight) {}

void UIContext::Update(const Input& input, float width, float height) {
	m_input = &input;
	m_mouseConsumed = false;
	const gfx::Rect window{0, 0, width, height};
	// Reverse order: widgets drawn on top get first claim on the mouse.
	for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it) {
		if (!(*it)->visible) continue;
		(*it)->Layout(window);
		(*it)->Update(*this);
	}
	m_input = nullptr;
}

void UIContext::Render(gfx::SpriteBatch& batch, float width, float height) {
	const gfx::Rect window{0, 0, width, height};
	for (auto& widget : m_widgets) {
		if (!widget->visible) continue;
		widget->Layout(window);
		widget->Draw(*this, batch);
	}
	for (auto& widget : m_widgets)
		if (widget->visible) widget->DrawOverlay(*this, batch);
}

} // namespace dungeon::ui
