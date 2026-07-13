// ============================================================================
// Game/LevelSettingsDialog.cpp — see LevelSettingsDialog.h.
// ============================================================================
#include "Game/LevelSettingsDialog.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
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
	m_editName = false;
	m_uiRebuild = false;
	BuildUI();
}

// The stem's pixel rect within the title line — the click target that opens
// the inline rename (Update hit-tests it, Render styles the hover).
gfx::Rect LevelSettingsDialog::StemRect(float w, float h) {
	const std::string prefix =
		std::format("{} — ", loc::Tr("map.level.title"));
	const float x = kTitle.x * w + m_font.MeasureWidth(prefix);
	return {x, kTitle.y * h, m_font.MeasureWidth(m_stem) + 6.0f,
			m_font.Height()};
}

void LevelSettingsDialog::BuildUI() {
	m_ui.Clear();
	m_nameField = nullptr;
	if (m_editName) {
		// The title row becomes the rename field (Render skips the drawn
		// title meanwhile). Enter commits; losing focus or Esc cancels.
		m_nameField = m_ui.Add<ui::TextField>(
			gfx::Rect{kTitle.x, kTitle.y, kTitle.w, 0.035f}, m_stem);
		m_nameField->maxLength = 24;
		m_nameField->SetFocused(true);
		ui::TextField* raw = m_nameField;
		raw->onChange = [raw] {
			// Stems are filenames AND whitespace-tokenised record words (the
			// DoorInspector name filter) — strip anything else as typed.
			std::erase_if(raw->text, [](char ch) {
				const unsigned char u = static_cast<unsigned char>(ch);
				return !(std::isalnum(u) || ch == '_' || ch == '-');
			});
		};
		raw->onSubmit = [this, raw] {
			const std::string next = raw->text;
			if (next.empty() || next == m_stem ||
				(onRename && onRename(m_stem, next))) {
				if (!next.empty() && next != m_stem) m_stem = next;
				m_editName = false;
				m_uiRebuild = true; // deferred — we are inside a callback
			}
			// A refused rename (duplicate etc.) keeps the field open.
		};
	}
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

	if (m_uiRebuild) { // deferred from a widget callback (see BuildUI)
		m_uiRebuild = false;
		BuildUI();
	}

	if (input.WasKeyPressed(VK_ESCAPE)) {
		if (m_editName) { // first Esc only cancels the name edit
			m_editName = false;
			m_uiRebuild = true;
			return;
		}
		if (onApply) onApply(m_oDust, m_oHaze, m_oAmbient); // revert preview
		Close();
		return;
	}

	// The stem in the title is the rename affordance: hover styles it,
	// a click swaps the title row for the edit field (safe to rebuild
	// immediately — this is not a widget callback).
	m_nameHover = false;
	if (!m_editName) {
		const gfx::Rect stem = StemRect(w, h);
		m_nameHover = stem.Contains(input.MouseX(), input.MouseY());
		if (m_nameHover && input.WasMousePressed(MouseButton::Left)) {
			m_editName = true;
			BuildUI();
			return; // the press is the affordance's, not the new field's
		}
	}

	m_ui.Update(input, w, h);

	// Clicking away from the open name field drops its focus — treat that as
	// cancel (Enter is the commit; field death is deferred like every Clear).
	if (m_editName && m_nameField && !m_nameField->Focused()) {
		m_editName = false;
		m_uiRebuild = true;
	}
}

void LevelSettingsDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th,
								 float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	if (!m_editName) {
		// Title: prefix in plain text, the stem as the rename affordance
		// (accent on hover + a hint underline so it reads clickable).
		const std::string prefix =
			std::format("{} — ", loc::Tr("map.level.title"));
		m_font.Draw(batch, prefix, kTitle.x * w, kTitle.y * h, th.text);
		const gfx::Rect stem{kTitle.x * w + m_font.MeasureWidth(prefix),
							 kTitle.y * h, m_font.MeasureWidth(m_stem),
							 m_font.Height()};
		m_font.Draw(batch, m_stem, stem.x, stem.y,
					m_nameHover ? th.accent : th.text);
		batch.DrawRect({stem.x, stem.y + stem.h + 1.0f, stem.w, 1.0f},
					   m_nameHover ? th.accent : th.textDim);
	}

	m_ui.Render(batch, w, h); // rows + footer buttons (+ the rename field)
}

} // namespace dungeon::game
