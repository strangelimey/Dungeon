// ============================================================================
// Game/LevelSettingsDialog.cpp — see LevelSettingsDialog.h.
// ============================================================================
#include "Game/LevelSettingsDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// A compact centered card (three rows + footer), the BalanceDialog proportions
// shrunk. The panel is the only rect now; the card inside it is stacked
// (Game/DialogLayout.h).
// Wide enough for the TITLE: "Level settings — <stem>" is drawn at
// kDialogTitleScale, and the old 0.28-wide card could not hold it — the stem
// ran out past the panel edge and under the close box long before any of this
// was stacked. A card has to be sized for the text it carries.
constexpr gfx::Rect kPanel{0.30f, 0.26f, 0.40f, 0.48f};
// A settings row's label column against its value column.
constexpr float kLabelFill = 1.4f, kFieldFill = 1.0f;

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (the live-apply pattern the Balance
// dialog uses; an in-progress "" / "-" / "0." just waits).
void AddNumericField(ui::Stack& row, ui::Len len, float value,
					 std::function<void(float)> commit) {
	auto* field = row.Row<ui::TextField>(len, std::format("{:g}", value));
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

void LevelSettingsDialog::BuildUI() {
	m_ui.Clear();
	m_nameField = nullptr;
	// The title band is the dialog's own: either the prefix + rename affordance,
	// or the rename field standing in for it. Either way it is a WIDGET in the
	// band the chrome reserved, so it cannot land on the rows beneath.
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, /*title*/ "", m_closeIcon,
											[this] {
												if (onApply)
													onApply(m_oDust, m_oHaze, m_oAmbient);
												Close();
											});
	if (m_editName) {
		// Enter commits; losing focus or Esc cancels.
		m_nameField =
			chrome.titleSlot->Add<ui::TextField>(gfx::Rect{0, 0, 1, 1}, m_stem);
		// It stands in for the title, so it is sized as the title, not as a row.
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
	} else {
		// Clicking the stem opens the rename. DEFERRED: this fires from inside
		// the tree walk, and rebuilding destroys the widget that called it.
		chrome.titleSlot->Add<EditableTitle>(
			gfx::Rect{0, 0, 1, 1}, std::format("{} — ", loc::Tr("map.level.title")),
			m_stem, [this] {
				m_editName = true;
				m_uiRebuild = true;
			});
	}

	struct Row {
		const char* labelKey;
		float* value;
	};
	const Row rows[3] = {{"map.level.dust", &m_dust},
						 {"map.level.haze", &m_haze},
						 {"map.level.ambient", &m_ambient}};
	for (const Row& r : rows) {
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr(r.labelKey))->centerV =
			true;
		AddNumericField(*row, ui::Len::Fill(kFieldFill), *r.value,
						[this, v = r.value](float f) {
							*v = f;
							Apply();
						});
	}
	chrome.body->Space(ui::Len::Fill()); // the rows sit at the top

	chrome.footer->Space(ui::Len::Fill());
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_dust, m_haze, m_ambient);
		Close();
	});
	chrome.footer->Space(ui::Len::Fill());
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

	// (The stem's hover and click are the StemTitle widget's own business now —
	// it owns the rect it draws, so there is no second copy of the geometry
	// here to drift out of step with the drawing.)
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
	m_ui.Render(batch, w, h); // title/rename, rows, footer — all widgets
}

} // namespace dungeon::game
