#include "UI/UIContext.h"

#include "UI/Widget.h"

namespace dungeon::ui {

UIContext::UIContext(gfx::GraphicsDevice& device, const std::string& fontPath,
                     float fontHeight)
    : m_font(device, fontPath, fontHeight) {}

void UIContext::Update(const Input& input) {
    m_input = &input;
    m_mouseConsumed = false;
    // Reverse order: widgets drawn on top get first claim on the mouse.
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it)
        if ((*it)->visible) (*it)->Update(*this);
    m_input = nullptr;
}

void UIContext::Render(gfx::SpriteBatch& batch) {
    for (auto& widget : m_widgets)
        if (widget->visible) widget->Draw(*this, batch);
    for (auto& widget : m_widgets)
        if (widget->visible) widget->DrawOverlay(*this, batch);
}

} // namespace dungeon::ui
