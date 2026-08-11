// ============================================================================
// Game/BalanceDialog.cpp — see BalanceDialog.h.
// ============================================================================
#include "Game/BalanceDialog.h"
#include "Game/CurvePlot.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// The panel, as fractions (0..1) of the window (the monster-config dialog's
// proportions, minus its preview pane — the tabs get the width). The card
// inside it is stacked (Game/DialogLayout.h).
constexpr gfx::Rect kPanel{0.24f, 0.08f, 0.52f, 0.84f};

// A row's label column against its value column.
constexpr float kLabelFill = 1.3f, kFieldFill = 1.0f;

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (a live apply). An unparseable
// in-progress state ("", "-", "0.") just waits — the knob keeps its last
// valid value until the text reads as a number again.
void AddNumericField(ui::Stack& row, ui::Len len, float value,
					 std::function<void(float)> commit) {
	auto* field = row.Row<ui::TextField>(len, std::format("{:g}", value));
	// Mono: these are the tuning TABLES, read down a column. A proportional
	// face gives every digit a different width, so the numbers will not line up
	// with each other however the rows are placed.
	field->fontRole = ui::FontRole::Mono;
	field->fontScale = 1.0f; // ...and sized to its digits: a tuning table stays
							 // at the document size while its labels grow
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

BalanceDialog::BalanceDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void BalanceDialog::Open(const Balance& current) {
	m_open = true;
	m_cfg = current;
	m_original = current;
	m_activeTab = 0;
	BuildUI();
}

void BalanceDialog::BuildUI() {
	m_ui.Clear();
	DialogChrome chrome =
		BuildDialogChrome(m_ui, kPanel, loc::Tr("map.balance.title"), m_closeIcon,
						  [this] {
							  if (onApply) onApply(m_original); // revert live
							  Close();
						  });

	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(), 0.075f);
	const size_t tabFormula = m_tabs->AddTab(loc::Tr("map.balance.tab.formula"));
	const size_t tabAttacks = m_tabs->AddTab(loc::Tr("map.balance.tab.attacks"));
	BuildFormulaTab(tabFormula);
	BuildAttacksTab(tabAttacks);
	m_tabs->SetActiveTab(m_activeTab);

	chrome.footer->Space(ui::Len::Fill());
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_cfg);
		Close();
	});
	chrome.footer->Space(ui::Len::Fill());
}

void BalanceDialog::BuildFormulaTab(size_t tab) {
	// One row per knob, straight from the fields table (a new knob in Balance.h
	// shows up here with no dialog change). Labels are the raw balance.cat keys
	// — WYSIWYG with the file. The stack is content-sized, so the list is as
	// long as the table and the page scrolls; the pitch used to be a fraction
	// of the page (0.062 = 29px around 40px type, every row on the one below).
	ui::Stack* rows = TabStack(*m_tabs, tab);
	// THE CURVES, FIRST — they are the shape of the formula, and the knobs
	// below are only numbers until you can see what they do (Game/CurvePlot.h).
	// Live by construction: the plot borrows the same working copy the fields
	// edit, so dragging skill_bonus redraws it with no wiring between them.
	rows->Row<CurvePlot>(ui::Len::Fixed(7.0f), &m_cfg);
	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	for (const BalanceField& f : BalanceFields()) {
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), f.key)->centerV = true;
		AddNumericField(*row, ui::Len::Fill(kFieldFill), m_cfg.*(f.value),
						[this, &f](float v) {
							m_cfg.*(f.value) = v;
							Apply();
						});
	}
}

void BalanceDialog::BuildAttacksTab(size_t tab) {
	// A table: a header row (raw attacks.cat keys), then one row per attack —
	// id + damage type (identity, read-only) and the four numeric fields. Every
	// row is the same horizontal stack, so the columns line up by construction
	// instead of by four x constants agreeing with each other.
	constexpr float kIdFill = 1.1f, kTypeFill = 0.9f, kNumFill = 1.0f;
	ui::Stack* rows = TabStack(*m_tabs, tab);

	ui::Stack* header = rows->Row<ui::Stack>(FormRow(), true);
	header->gapRem = 0.4f;
	header->Space(ui::Len::Fill(kIdFill + kTypeFill));
	for (const char* h : {"damage", "accuracy", "speed", "stamina"})
		header->Row<ui::Label>(ui::Len::Fill(kNumFill), h)->centerV = true;
	// "?" — the column explainer overlay (what the four numbers do).
	header->Row<ui::Button>(FooterButton(0.35f), "?", [this] { m_helpOpen = true; });

	for (AttackSpec& a : m_cfg.attacks) {
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.4f;
		row->Row<ui::Label>(ui::Len::Fill(kIdFill), a.id)->centerV = true;
		ui::Label* type =
			row->Row<ui::Label>(ui::Len::Fill(kTypeFill), a.typeId);
		type->centerV = true;
		type->dim = true;
		AddNumericField(*row, ui::Len::Fill(kNumFill), a.dmg,
						[this, &a](float v) { a.dmg = v; Apply(); });
		AddNumericField(*row, ui::Len::Fill(kNumFill), a.acc,
						[this, &a](float v) { a.acc = v; Apply(); });
		AddNumericField(*row, ui::Len::Fill(kNumFill), a.pace,
						[this, &a](float v) { a.pace = v; Apply(); });
		AddNumericField(*row, ui::Len::Fill(kNumFill), a.stam,
						[this, &a](float v) { a.stam = v; Apply(); });
		// The header's "?" column keeps the numbers clear of the scrollbar.
		row->Space(FooterButton(0.35f));
	}
}

void BalanceDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	// One font now: the title text used to be a second Font at the very same
	// size as this context's. GameUI::UpdateFonts commits every library font
	// once per frame, so there is nothing to flush here either.
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	// The column-help overlay owns the input while up: any click or Esc
	// dismisses it, and the dialog beneath stays frozen.
	if (m_helpOpen) {
		if (input.WasKeyPressed(VK_ESCAPE) ||
			input.WasMousePressed(MouseButton::Left))
			m_helpOpen = false;
		return;
	}

	if (input.WasKeyPressed(VK_ESCAPE)) { // cancel: revert live to the snapshot
		if (onApply) onApply(m_original);
		Close();
		return;
	}

	m_ui.Update(input, w, h);
	if (!m_open) return; // a footer button closed us this frame
	if (m_tabs) m_activeTab = m_tabs->ActiveTab();
}

void BalanceDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
						   float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h); // title + tabs + fields + footer

	// The "?" overlay: title + word-wrapped paragraphs (one lang key each),
	// drawn over a second dim wash so the dialog visibly freezes beneath it.
	if (m_helpOpen) {
		batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
		const gfx::Rect help{0.30f * w, 0.14f * h, 0.40f * w, 0.72f * h};
		batch.DrawRect(help, th.panel);
		ui::DrawBorder(batch, help, th.panelBorder);
		const float pad = help.w * 0.05f;
		const float lineH = m_ui.GetFont().Height() * 1.25f;
		float y = help.y + pad;
		m_ui.GetFont().Draw(batch, loc::Tr("map.balance.help.title"), help.x + pad, y,
					th.text);
		y += lineH * 1.6f;
		// Word-wrap each paragraph to the panel width; a blank line between.
		for (const char* key :
			 {"map.balance.help.damage", "map.balance.help.accuracy",
			  "map.balance.help.speed", "map.balance.help.stamina",
			  "map.balance.help.notes"}) {
			const std::string text = loc::Tr(key);
			std::string line;
			size_t start = 0;
			while (start <= text.size()) {
				const size_t sp = text.find(' ', start);
				const std::string word =
					text.substr(start, sp == std::string::npos ? std::string::npos
															   : sp - start);
				const std::string tryLine =
					line.empty() ? word : line + " " + word;
				if (!line.empty() &&
					m_ui.GetFont().MeasureWidth(tryLine) > help.w - pad * 2) {
					m_ui.GetFont().Draw(batch, line, help.x + pad, y, th.textDim);
					y += lineH;
					line = word;
				} else {
					line = tryLine;
				}
				if (sp == std::string::npos) break;
				start = sp + 1;
			}
			if (!line.empty()) {
				m_ui.GetFont().Draw(batch, line, help.x + pad, y, th.textDim);
				y += lineH;
			}
			y += lineH * 0.5f; // paragraph gap
		}
	}
}

} // namespace dungeon::game
