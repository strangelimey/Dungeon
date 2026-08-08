// ============================================================================
// Game/ProjectileInspector.cpp — see ProjectileInspector.h.
// ============================================================================
#include "Game/ProjectileInspector.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace dungeon::game {

namespace {
// A compact centered card: title, six info rows, a Remove footer — the
// LevelSettingsDialog proportions. The panel is the only rect; the card inside
// it is stacked (Game/DialogLayout.h).
constexpr gfx::Rect kPanel{0.36f, 0.28f, 0.28f, 0.40f};
// The label column's share of an info row; the value takes the rest.
constexpr float kLabelFill = 1.0f, kValueFill = 1.1f;
} // namespace

ProjectileInspector::ProjectileInspector(gfx::GraphicsDevice& device,
										 ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void ProjectileInspector::Open(const Config& cfg) {
	m_open = true;
	m_cfg = cfg;
	BuildUI();
}

void ProjectileInspector::BuildUI() {
	m_ui.Clear();
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, loc::Tr("map.proj.title"),
											m_closeIcon, [this] { Close(); });

	// Read-only info rows: label (dim) + value (bright). They used to be drawn
	// straight to the batch from a hand-stepped y cursor, which is furniture the
	// layout could not see — as widgets they get areas like everything else.
	const std::array<std::pair<std::string, std::string>, 6> rows = {{
		{loc::Tr("map.proj.side"), m_cfg.side},
		{loc::Tr("map.proj.dmgtype"), m_cfg.dmgType},
		{loc::Tr("map.proj.damage"), std::format("{:.1f}", m_cfg.damage)},
		{loc::Tr("map.proj.accuracy"), std::format("{:.0f}%", m_cfg.accuracy * 100.0f)},
		{loc::Tr("map.proj.speed"), std::format("{:.1f} m/s", m_cfg.speed)},
		{loc::Tr("map.proj.range"), std::format("{:.1f} m", m_cfg.rangeLeft)},
	}};
	for (const auto& [label, value] : rows) {
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		ui::Label* l = row->Row<ui::Label>(ui::Len::Fill(kLabelFill), label);
		l->dim = true;
		l->centerV = true;
		row->Row<ui::Label>(ui::Len::Fill(kValueFill), value)->centerV = true;
	}
	chrome.body->Space(ui::Len::Fill()); // the rows sit at the top

	chrome.footer->Space(ui::Len::Fill());
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.proj.remove"),
								   [this] {
									   if (onRemove) onRemove();
									   Close();
								   });
	chrome.footer->Space(ui::Len::Fill());
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
	m_ui.Render(batch, w, h); // title, info rows, Remove, close — all widgets
}

} // namespace dungeon::game
