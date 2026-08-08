// ============================================================================
// Game/InspectPicker.cpp — see InspectPicker.h.
// ============================================================================
#include "Game/InspectPicker.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// The panel's width and the height ONE row costs, as window fractions — the
// only two numbers left, and both are about sizing the card to its contents.
// Where the rows go inside it is the stack's business (Game/DialogLayout.h).
constexpr float kPanelW = 0.30f;
constexpr float kTitleH = 0.075f; // the title band, for the panel height sum
constexpr float kRowH = 0.058f;
constexpr float kChromeH = 0.05f; // padding above and below
} // namespace

InspectPicker::InspectPicker(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void InspectPicker::Open(const std::string& title, const std::vector<std::string>& items) {
	m_open = true;
	m_title = title;
	m_items = items;

	// Size the panel to the content and centre it: title + one row per item
	// (Close is the top-right corner box now, not a row).
	const float bodyH =
		kChromeH + kTitleH + static_cast<float>(items.size()) * kRowH;
	const float x = (1.0f - kPanelW) * 0.5f;
	const float y = std::clamp((1.0f - bodyH) * 0.5f, 0.05f, 0.5f);
	m_panel = {x, y, kPanelW, bodyH};

	BuildUI();
}

void InspectPicker::BuildUI() {
	m_ui.Clear();
	DialogChrome chrome = BuildDialogChrome(m_ui, m_panel, m_title, m_closeIcon,
											[this] { Close(); },
											/*withFooter*/ false);
	for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
		chrome.body->Row<ui::Button>(FormRow(1.2f), m_items[i], [this, i] {
			Close();
			if (onPick) onPick(i);
		});
}

void InspectPicker::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));

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
	m_ui.Render(batch, w, h); // title, rows, close — all widgets now
}

} // namespace dungeon::game
