#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {

// Simple framed background rectangle (draws first if added first).
class Panel : public Widget {
public:
    explicit Panel(const gfx::Rect& rect) { bounds = rect; }
    void Update(UIContext&) override {}
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
};

class Label : public Widget {
public:
    Label(const gfx::Rect& rect, std::string text) : text(std::move(text)) {
        bounds = rect;
    }
    void Update(UIContext&) override {}
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

    std::string text;
    bool dim = false;
};

// Scrolling multi-line text log (message window). New lines append at the
// bottom; the mouse wheel scrolls when hovered.
class TextOutput : public Widget {
public:
    explicit TextOutput(const gfx::Rect& rect, size_t maxLines = 200)
        : m_maxLines(maxLines) {
        bounds = rect;
    }

    void AddLine(std::string line);
    void Update(UIContext& ctx) override;
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
    std::deque<std::string> m_lines;
    size_t m_maxLines;
    float m_scroll = 0.0f; // 0 = pinned to latest
};

class Button : public Widget {
public:
    Button(const gfx::Rect& rect, std::string text, std::function<void()> onClick)
        : text(std::move(text)), onClick(std::move(onClick)) {
        bounds = rect;
    }

    void Update(UIContext& ctx) override;
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

    std::string text;
    std::function<void()> onClick;

private:
    bool m_hot = false;
    bool m_held = false;
};

// Horizontal slider, value in [min, max].
class Slider : public Widget {
public:
    Slider(const gfx::Rect& rect, std::string label, float min, float max, float value,
           std::function<void(float)> onChange)
        : label(std::move(label)), m_min(min), m_max(max), m_value(value),
          onChange(std::move(onChange)) {
        bounds = rect;
        RefreshDisplay();
    }

    float Value() const { return m_value; }
    void Update(UIContext& ctx) override;
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;

    std::string label;
    std::function<void(float)> onChange;

private:
    void RefreshDisplay(); // caches the "label: value" text (not per-frame)

    float m_min, m_max, m_value;
    std::string m_display;
    bool m_dragging = false;
};

class DropDown : public Widget {
public:
    DropDown(const gfx::Rect& rect, std::vector<std::string> items, int selected,
             std::function<void(int)> onSelect)
        : items(std::move(items)), m_selected(selected), onSelect(std::move(onSelect)) {
        bounds = rect;
    }

    int Selected() const { return m_selected; }
    void Update(UIContext& ctx) override;
    void Draw(UIContext& ctx, gfx::SpriteBatch& batch) override;
    void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) override;

    std::vector<std::string> items;
    std::function<void(int)> onSelect;

private:
    gfx::Rect ItemRect(size_t index) const;

    int m_selected = 0;
    int m_hoverItem = -1;
    bool m_open = false;
    bool m_hot = false;
};

// Draws a 1px border around a rectangle.
void DrawBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect, const Vec4& color);

} // namespace dungeon::ui
