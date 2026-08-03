// ============================================================================
// Game/ProjectileInspector.cpp — see ProjectileInspector.h.
// ============================================================================
#include "Game/ProjectileInspector.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "UI/Controls.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace dungeon::game {

namespace {
// A compact centered card: title, six info rows, a two-button footer — the
// LevelSettingsDialog proportions.
constexpr gfx::Rect kPanel{0.36f, 0.28f, 0.28f, 0.40f};
constexpr gfx::Rect kTitle{0.375f, 0.30f, 0.25f, 0.04f};
constexpr float kRowX = 0.375f, kValX = 0.51f;
constexpr float kRowY0 = 0.365f, kRowH = 0.042f;
// Remove centered in the footer; Close is the top-right corner box now.
constexpr gfx::Rect kRemove{0.45f, 0.615f, 0.10f, 0.045f};
} // namespace

ProjectileInspector::ProjectileInspector(gfx::GraphicsDevice& device,
										 ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_closeIcon = TryLoadTextureFile(device, paths::Asset("ui\\icon_close"));
}

void ProjectileInspector::Open(const Config& cfg) {
	m_open = true;
	m_cfg = cfg;
	BuildUI();
}

void ProjectileInspector::BuildUI() {
	m_ui.Clear();
	m_ui.Add<ui::Button>(kRemove, loc::Tr("map.proj.remove"), [this] {
		if (onRemove) onRemove();
		Close();
	});
	ui::AddCloseButton(m_ui, kPanel, m_closeIcon.get(), [this] { Close(); });
}

void ProjectileInspector::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	// One font now (the dialog's title text used to be a second Font at the very
	// same size as this context's); GameUI::UpdateFonts commits every library
	// font once per frame, so there is nothing to flush here.
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));
	if (input.WasKeyPressed(VK_ESCAPE)) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void ProjectileInspector::Render(gfx::SpriteBatch& batch, const ui::Theme& th,
								 float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	m_ui.GetFont().Draw(batch, loc::Tr("map.proj.title"), kTitle.x * w, kTitle.y * h, th.text);

	// Read-only info rows: label (dim) + value (bright).
	const std::array<std::pair<std::string, std::string>, 6> rows = {{
		{loc::Tr("map.proj.side"), m_cfg.side},
		{loc::Tr("map.proj.dmgtype"), m_cfg.dmgType},
		{loc::Tr("map.proj.damage"), std::format("{:.1f}", m_cfg.damage)},
		{loc::Tr("map.proj.accuracy"), std::format("{:.0f}%", m_cfg.accuracy * 100.0f)},
		{loc::Tr("map.proj.speed"), std::format("{:.1f} m/s", m_cfg.speed)},
		{loc::Tr("map.proj.range"), std::format("{:.1f} m", m_cfg.rangeLeft)},
	}};
	for (size_t i = 0; i < rows.size(); ++i) {
		const float y = (kRowY0 + i * kRowH) * h;
		m_ui.GetFont().Draw(batch, rows[i].first, kRowX * w, y, th.textDim);
		m_ui.GetFont().Draw(batch, rows[i].second, kValX * w, y, th.text);
	}

	m_ui.Render(batch, w, h); // Remove / Close
}

} // namespace dungeon::game
