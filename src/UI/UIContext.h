#pragma once

#include "Platform/Input.h"
#include "UI/Font.h"

#include <memory>
#include <vector>

namespace dungeon::ui {

class Widget;

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

    template <typename T, typename... Args>
    T* Add(Args&&... args) {
        auto widget = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = widget.get();
        m_widgets.push_back(std::move(widget));
        return raw;
    }

    void Update(const Input& input);
    void Render(gfx::SpriteBatch& batch);

    Font& GetFont() { return m_font; }
    const Theme& GetTheme() const { return m_theme; }

    // Input routing state (used by widgets during Update).
    const Input* CurrentInput() const { return m_input; }
    bool IsMouseConsumed() const { return m_mouseConsumed; }
    void ConsumeMouse() { m_mouseConsumed = true; }

private:
    Font m_font;
    Theme m_theme;
    std::vector<std::unique_ptr<Widget>> m_widgets;
    const Input* m_input = nullptr;
    bool m_mouseConsumed = false;
};

} // namespace dungeon::ui
