// ============================================================================
// Game/InspectPicker.cpp — see InspectPicker.h.
// ============================================================================
#include "Game/InspectPicker.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Layout, as fractions (0..1) of the window. The panel grows with the row count;
// everything else keys off these so title/rows/Close stack without overlap.
constexpr float kPanelW = 0.30f;
constexpr float kPad = 0.02f;    // panel inner margin
constexpr float kTitleH = 0.05f;
constexpr float kRowH = 0.058f;
constexpr float kGap = 0.01f;
} // namespace

InspectPicker::InspectPicker(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

void InspectPicker::Open(const std::string& title, const std::vector<std::string>& items) {
	m_open = true;
	m_title = title;
	m_items = items;

	// Size the panel to the content and centre it: title + one row per item + Close.
	const int n = static_cast<int>(items.size());
	const float bodyH = kPad + kTitleH + kGap + (n + 1) * (kRowH + kGap) + kPad;
	const float x = (1.0f - kPanelW) * 0.5f;
	const float y = std::clamp((1.0f - bodyH) * 0.5f, 0.05f, 0.5f);
	m_panel = {x, y, kPanelW, bodyH};
	m_titleRect = {x + kPad, y + kPad, kPanelW - 2 * kPad, kTitleH};

	BuildUI();
}

void InspectPicker::BuildUI() {
	m_ui.Clear();
	const float x = m_panel.x + kPad;
	const float w = m_panel.w - 2 * kPad;
	float y = m_panel.y + kPad + kTitleH + kGap;
	for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
		m_ui.Add<ui::Button>(gfx::Rect{x, y, w, kRowH}, m_items[i], [this, i] {
			Close();
			if (onPick) onPick(i);
		});
		y += kRowH + kGap;
	}
	m_ui.Add<ui::Button>(gfx::Rect{x, y, w, kRowH}, loc::Tr("map.cfg.close"),
						 [this] { Close(); });
}

void InspectPicker::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

	if (input.WasKeyPressed(VK_ESCAPE)) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void InspectPicker::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w, float h) {
	if (!m_open) return;
	auto px = [&](const gfx::Rect& r) {
		return gfx::Rect{r.x * w, r.y * h, r.w * w, r.h * h};
	};
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel = px(m_panel);
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	const gfx::Rect title = px(m_titleRect);
	m_font.Draw(batch, m_title, title.x, title.y, th.text);

	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
