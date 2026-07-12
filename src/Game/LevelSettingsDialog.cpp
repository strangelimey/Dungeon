// ============================================================================
// Game/LevelSettingsDialog.cpp — see LevelSettingsDialog.h.
// ============================================================================
#include "Game/LevelSettingsDialog.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// Panel + region geometry, as fractions (0..1) of the window — a compact
// centered card (three rows + footer), the BalanceDialog proportions shrunk.
constexpr gfx::Rect kPanel{0.36f, 0.30f, 0.28f, 0.36f};
constexpr gfx::Rect kTitle{0.375f, 0.315f, 0.25f, 0.04f};
constexpr float kRowX = 0.375f, kRowW = 0.14f;   // label column
constexpr float kFieldX = 0.52f, kFieldW = 0.10f; // value column
constexpr float kRowY0 = 0.385f, kRowH = 0.055f;
constexpr gfx::Rect kSave{0.40f, 0.585f, 0.09f, 0.045f};
constexpr gfx::Rect kClose{0.51f, 0.585f, 0.09f, 0.045f};

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (the live-apply pattern the Balance
// dialog uses; an in-progress "" / "-" / "0." just waits).
void AddNumericField(ui::UIContext& ui, const gfx::Rect& r, float value,
					 std::function<void(float)> commit) {
	auto* field = ui.Add<ui::TextField>(r, std::format("{:g}", value));
	field->maxLength = 10;
	ui::TextField* raw = field;
	field->onChange = [raw, commit = std::move(commit)] {
		const std::string& t = raw->text;
		float v = 0.0f;
		const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
		if (ec == std::errc() && p == t.data() + t.size()) commit(v);
	};
}
} // namespace

LevelSettingsDialog::LevelSettingsDialog(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

void LevelSettingsDialog::Open(const std::string& stem, float dust, float haze,
							   float ambient) {
	m_open = true;
	m_stem = stem;
	m_dust = m_oDust = dust;
	m_haze = m_oHaze = haze;
	m_ambient = m_oAmbient = ambient;
	BuildUI();
}

void LevelSettingsDialog::BuildUI() {
	m_ui.Clear();
	struct Row {
		const char* labelKey;
		float* value;
	};
	const Row rows[3] = {{"map.level.dust", &m_dust},
						 {"map.level.haze", &m_haze},
						 {"map.level.ambient", &m_ambient}};
	for (int i = 0; i < 3; ++i) {
		const float y = kRowY0 + i * kRowH;
		m_ui.Add<ui::Label>(gfx::Rect{kRowX, y, kRowW, kRowH * 0.8f},
							loc::Tr(rows[i].labelKey));
		AddNumericField(m_ui, gfx::Rect{kFieldX, y, kFieldW, kRowH * 0.8f},
						*rows[i].value, [this, v = rows[i].value](float f) {
							*v = f;
							Apply();
						});
	}
	m_ui.Add<ui::Button>(kSave, loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_dust, m_haze, m_ambient);
		Close();
	});
	m_ui.Add<ui::Button>(kClose, loc::Tr("map.cfg.close"), [this] {
		if (onApply) onApply(m_oDust, m_oHaze, m_oAmbient); // revert the preview
		Close();
	});
}

void LevelSettingsDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

	if (input.WasKeyPressed(VK_ESCAPE)) { // cancel: revert the live preview
		if (onApply) onApply(m_oDust, m_oHaze, m_oAmbient);
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void LevelSettingsDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th,
								 float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	m_font.Draw(batch,
				std::format("{} — {}", loc::Tr("map.level.title"), m_stem),
				kTitle.x * w, kTitle.y * h, th.text);

	m_ui.Render(batch, w, h); // rows + footer buttons
}

} // namespace dungeon::game
