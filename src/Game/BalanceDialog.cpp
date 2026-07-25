// ============================================================================
// Game/BalanceDialog.cpp — see BalanceDialog.h.
// ============================================================================
#include "Game/BalanceDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "UI/Controls.h"

#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// Panel + region geometry, as fractions (0..1) of the window (the monster-
// config dialog's proportions, minus its preview pane — the tabs get the
// width).
constexpr gfx::Rect kPanel{0.24f, 0.08f, 0.52f, 0.84f};
constexpr gfx::Rect kTitle{0.255f, 0.095f, 0.45f, 0.045f};
constexpr gfx::Rect kTabs{0.255f, 0.165f, 0.49f, 0.67f};
// Save centered in the footer; Close is the top-right corner box now.
constexpr gfx::Rect kSave{0.4475f, 0.855f, 0.105f, 0.05f};

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (a live apply). An unparseable
// in-progress state ("", "-", "0.") just waits — the knob keeps its last
// valid value until the text reads as a number again.
void AddNumericField(ui::TabControl* tabs, size_t tab, const gfx::Rect& r,
					 float value, std::function<void(float)> commit) {
	auto* field =
		tabs->AddChild<ui::TextField>(tab, r, std::format("{:g}", value));
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

BalanceDialog::BalanceDialog(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {
	m_closeIcon = TryLoadTextureFile(device, paths::Asset("ui\\icon_close"));
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
	m_tabs = m_ui.Add<ui::TabControl>(kTabs, 0.075f);
	const size_t tabFormula = m_tabs->AddTab(loc::Tr("map.balance.tab.formula"));
	const size_t tabAttacks = m_tabs->AddTab(loc::Tr("map.balance.tab.attacks"));
	BuildFormulaTab(tabFormula);
	BuildAttacksTab(tabAttacks);
	m_tabs->SetActiveTab(m_activeTab);

	m_ui.Add<ui::Button>(kSave, loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_cfg);
		Close();
	});
	ui::AddCloseButton(m_ui, kPanel, m_closeIcon.get(), [this] {
		if (onApply) onApply(m_original); // revert the live tuning
		Close();
	});
}

void BalanceDialog::BuildFormulaTab(size_t tab) {
	// One row per knob, straight from the fields table (a new knob in
	// Balance.h shows up here with no dialog change). Labels are the raw
	// balance.cat keys — WYSIWYG with the file. Rows past the tab bottom
	// scroll (TabControl's per-tab scrollbar).
	constexpr float rowH = 0.062f, top = 0.03f;
	int row = 0;
	for (const BalanceField& f : BalanceFields()) {
		const float y = top + row * rowH;
		m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.05f, y, 0.5f, rowH * 0.85f},
									f.key);
		AddNumericField(m_tabs, tab, gfx::Rect{0.58f, y, 0.3f, rowH * 0.85f},
						m_cfg.*(f.value), [this, &f](float v) {
							m_cfg.*(f.value) = v;
							Apply();
						});
		++row;
	}
}

void BalanceDialog::BuildAttacksTab(size_t tab) {
	// Header row (raw attacks.cat keys), then one row per attack: id + its
	// damage type (identity, read-only) and the four numeric fields.
	constexpr float rowH = 0.068f, top = 0.03f;
	constexpr float colW = 0.155f;
	constexpr float colX[4] = {0.26f, 0.435f, 0.61f, 0.785f};
	// "?" — the column explainer overlay (what the four numbers do).
	m_tabs->AddChild<ui::Button>(tab, gfx::Rect{0.95f, top, 0.045f, rowH * 0.85f},
								 "?", [this] { m_helpOpen = true; });
	const char* headers[4] = {"damage", "accuracy", "speed", "stamina"};
	for (int c = 0; c < 4; ++c)
		m_tabs->AddChild<ui::Label>(
			tab, gfx::Rect{colX[c], top, colW, rowH * 0.8f}, headers[c]);
	for (size_t i = 0; i < m_cfg.attacks.size(); ++i) {
		AttackSpec& a = m_cfg.attacks[i];
		const float y = top + (1 + static_cast<float>(i)) * rowH;
		m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.02f, y, 0.13f, rowH * 0.85f},
									a.id);
		m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.15f, y, 0.10f, rowH * 0.85f},
									DamageTypeId(a.type));
		AddNumericField(m_tabs, tab, gfx::Rect{colX[0], y, colW, rowH * 0.85f},
						a.dmg, [this, &a](float v) { a.dmg = v; Apply(); });
		AddNumericField(m_tabs, tab, gfx::Rect{colX[1], y, colW, rowH * 0.85f},
						a.acc, [this, &a](float v) { a.acc = v; Apply(); });
		AddNumericField(m_tabs, tab, gfx::Rect{colX[2], y, colW, rowH * 0.85f},
						a.pace, [this, &a](float v) { a.pace = v; Apply(); });
		AddNumericField(m_tabs, tab, gfx::Rect{colX[3], y, colW, rowH * 0.85f},
						a.stam, [this, &a](float v) { a.stam = v; Apply(); });
	}
}

void BalanceDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

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

	m_font.Draw(batch, loc::Tr("map.balance.title"), kTitle.x * w, kTitle.y * h,
				th.text);

	m_ui.Render(batch, w, h); // tabs + fields + footer buttons

	// The "?" overlay: title + word-wrapped paragraphs (one lang key each),
	// drawn over a second dim wash so the dialog visibly freezes beneath it.
	if (m_helpOpen) {
		batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
		const gfx::Rect help{0.30f * w, 0.14f * h, 0.40f * w, 0.72f * h};
		batch.DrawRect(help, th.panel);
		ui::DrawBorder(batch, help, th.panelBorder);
		const float pad = help.w * 0.05f;
		const float lineH = m_font.Height() * 1.25f;
		float y = help.y + pad;
		m_font.Draw(batch, loc::Tr("map.balance.help.title"), help.x + pad, y,
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
					m_font.MeasureWidth(tryLine) > help.w - pad * 2) {
					m_font.Draw(batch, line, help.x + pad, y, th.textDim);
					y += lineH;
					line = word;
				} else {
					line = tryLine;
				}
				if (sp == std::string::npos) break;
				start = sp + 1;
			}
			if (!line.empty()) {
				m_font.Draw(batch, line, help.x + pad, y, th.textDim);
				y += lineH;
			}
			y += lineH * 0.5f; // paragraph gap
		}
	}
}

} // namespace dungeon::game
