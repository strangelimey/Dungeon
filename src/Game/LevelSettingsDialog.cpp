// ============================================================================
// Game/LevelSettingsDialog.cpp — see LevelSettingsDialog.h.
// ============================================================================
#include "Game/LevelSettingsDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// Panel + region geometry, as fractions (0..1) of the window — a compact
// centered card (three rows + footer), the BalanceDialog proportions shrunk.
// Half again as wide as it was (0.28), which the title and the setting names
// both wanted: "Level settings — <stem>" is a long line for a narrow card, and
// the label column was tight enough at kDialogTextScale to be worth checking.
// Still centered, so only x and the column fractions move.
constexpr gfx::Rect kPanel{0.29f, 0.30f, 0.42f, 0.36f};
constexpr gfx::Rect kTitle{0.305f, 0.315f, 0.25f, 0.04f};
constexpr float kRowX = 0.305f, kRowW = 0.21f;   // label column
constexpr float kFieldX = 0.545f, kFieldW = 0.13f; // value column
// The rows start below the TITLE BAND (ui::kDialogTitleBandH) rather than at a
// hand-picked gap: at 0.385 the band was 0.070, short of the 0.0725 a title's
// line advance needs, and the title's descenders reached into the first label.
constexpr float kRowY0 = kTitle.y + ui::kDialogTitleBandH, kRowH = 0.055f;
// Save centered in the footer; Close is the top-right corner box now.
constexpr gfx::Rect kSave{0.455f, 0.585f, 0.09f, 0.045f};

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (the live-apply pattern the Balance
// dialog uses; an in-progress "" / "-" / "0." just waits).
void AddNumericField(ui::UIContext& ui, const gfx::Rect& r, float value,
					 std::function<void(float)> commit) {
	auto* field = ui.Add<ui::TextField>(r, std::format("{:g}", value));
	field->fontRole = ui::FontRole::Mono; // a numeric readout, like Balance's
	field->fontScale = 1.0f; // ...and sized to its digits, so it stays put while
							 // the labels around it take kDialogTextScale
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

LevelSettingsDialog::LevelSettingsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	// The whole form reads at the dialog text size. Set on the ROOT because
	// fontScale inherits (Widget.h), so one line covers every row this dialog
	// grows later; the numeric readouts pin themselves back in AddNumericField.
	// It survives Clear(), which only drops the root's children.
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

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

std::string LevelSettingsDialog::TitlePrefix() const {
	return std::format("{} — ", loc::Tr("map.level.title"));
}

// The band the title may occupy: the title line, one band tall, stopping short
// of the close box in the corner it runs toward.
gfx::Rect LevelSettingsDialog::TitleBand(float w, float h) const {
	const gfx::Rect band = ui::DialogTitleBand(kPanel, kTitle.x, kTitle.y);
	return {band.x * w, band.y * h, band.w * w, band.h * h};
}

// Fitted to that band. This panel is a narrow card (0.28 of the window) and the
// title carries the level's name, so the standard title size runs off the edge
// for all but the shortest stems — FitDialogTitle shrinks it instead of cutting
// the stem off, which is the part being renamed.
const ui::Font& LevelSettingsDialog::TitleFont(float w, float h) const {
	return *ui::FitDialogTitle(m_ui, TitlePrefix() + m_stem, TitleBand(w, h)).font;
}

// The stem's pixel rect within the title line — the click target that opens
// the inline rename (Update hit-tests it, Render styles the hover).
gfx::Rect LevelSettingsDialog::StemRect(float w, float h) {
	// The fitted TITLE face, not the body one — Render draws the stem with the
	// same, and this rect is the click target for it.
	const ui::Font& title = TitleFont(w, h);
	const float x = kTitle.x * w + title.MeasureWidth(TitlePrefix());
	return {x, kTitle.y * h, title.MeasureWidth(m_stem) + 6.0f, title.Height()};
}

void LevelSettingsDialog::BuildUI() {
	m_ui.Clear();
	m_nameField = nullptr;
	if (m_editName) {
		// The title row becomes the rename field (Render skips the drawn
		// title meanwhile). Enter commits; losing focus or Esc cancels.
		m_nameField = m_ui.Add<ui::TextField>(
			ui::DialogTitleBand(kPanel, kTitle.x, kTitle.y), m_stem);
		// It stands in for the title, so it is sized as the title, not as a row —
		// and its BOX is the title band for the same reason, or the face it is
		// about to draw does not fit the field holding it.
		m_nameField->fontScale = ui::kDialogTitleScale;
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
	ui::AddCloseButton(m_ui, kPanel, m_closeIcon, [this] {
		if (onApply) onApply(m_oDust, m_oHaze, m_oAmbient); // revert the preview
		Close();
	});
}

void LevelSettingsDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	// One font now: the title text used to be a second Font at the very same
	// size as this context's. GameUI::UpdateFonts commits every library font
	// once per frame, so there is nothing to flush here either.
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

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
		// (accent on hover + a hint underline so it reads clickable). Drawn in
		// two pieces because they are styled differently, so this takes the
		// fitted FONT rather than FitDialogTitle's joined string.
		const std::string prefix = TitlePrefix();
		const ui::Font& title = TitleFont(w, h); // same as StemRect
		title.Draw(batch, prefix, kTitle.x * w, kTitle.y * h, th.text);
		const gfx::Rect stem{kTitle.x * w + title.MeasureWidth(prefix),
							 kTitle.y * h, title.MeasureWidth(m_stem),
							 title.Height()};
		title.Draw(batch, m_stem, stem.x, stem.y,
				   m_nameHover ? th.accent : th.text);
		batch.DrawRect({stem.x, stem.y + stem.h + 1.0f, stem.w, 1.0f},
					   m_nameHover ? th.accent : th.textDim);
	}

	m_ui.Render(batch, w, h); // rows + footer buttons (+ the rename field)
}

} // namespace dungeon::game
