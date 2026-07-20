// ============================================================================
// Game/WallStyleDialog.cpp — see WallStyleDialog.h.
// ============================================================================
#include "Game/WallStyleDialog.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {
// A compact centered card: title, a wear slider, a columns checkbox, footer —
// the LevelSettingsDialog proportions.
constexpr gfx::Rect kPanel{0.36f, 0.30f, 0.28f, 0.34f};
constexpr gfx::Rect kTitle{0.375f, 0.320f, 0.25f, 0.04f};
constexpr gfx::Rect kWear{0.375f, 0.395f, 0.25f, 0.075f};   // slider (label + track)
constexpr gfx::Rect kColumns{0.375f, 0.490f, 0.25f, 0.045f}; // checkbox row
constexpr gfx::Rect kSave{0.40f, 0.575f, 0.09f, 0.045f};
constexpr gfx::Rect kClose{0.51f, 0.575f, 0.09f, 0.045f};
} // namespace

WallStyleDialog::WallStyleDialog(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

void WallStyleDialog::Open(const std::string& id, const std::string& display,
						   const std::string& texture, float wear, bool columns) {
	m_open = true;
	m_id = id;
	m_display = display;
	m_texture = texture;
	m_wear = std::clamp(wear, 0.0f, 1.0f);
	m_columns = columns;
	BuildUI();
}

void WallStyleDialog::BuildUI() {
	m_ui.Clear();
	m_ui.Add<ui::Slider>(kWear, loc::Tr("map.wallstyle.wear"), 0.0f, 1.0f, m_wear,
						 [this](float v) { m_wear = v; });
	m_ui.Add<ui::Checkbox>(kColumns, loc::Tr("map.wallstyle.columns"), m_columns,
						   [this](bool on) { m_columns = on; });
	m_ui.Add<ui::Button>(kSave, loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_id, m_texture, m_wear, m_columns);
		Close();
	});
	m_ui.Add<ui::Button>(kClose, loc::Tr("map.cfg.close"), [this] { Close(); });
}

void WallStyleDialog::Update(const Input& input, float w, float h) {
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

void WallStyleDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							 float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	const std::string title =
		std::format("{} — {}", loc::Tr("map.wallstyle.title"), m_display);
	m_font.Draw(batch, title, kTitle.x * w, kTitle.y * h, th.text);

	m_ui.Render(batch, w, h); // slider + checkbox + footer buttons
}

} // namespace dungeon::game
