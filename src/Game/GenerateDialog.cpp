// ============================================================================
// Game/GenerateDialog.cpp — see GenerateDialog.h.
// ============================================================================
#include "Game/GenerateDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <random>

namespace dungeon::game {

namespace {
constexpr gfx::Rect kPanel{0.30f, 0.14f, 0.40f, 0.72f};
constexpr float kLabelFill = 1.3f, kFieldFill = 1.0f;

// A numeric field that writes every PARSEABLE state back as you type (the
// Balance/Level dialog pattern — an in-progress "" or "-" simply waits).
void NumField(ui::Stack& row, float value, std::function<void(float)> commit) {
	auto* field = row.Row<ui::TextField>(ui::Len::Fill(kFieldFill),
										 std::format("{:g}", value));
	field->fontRole = ui::FontRole::Mono;
	field->fontScale = 1.0f; // sized to its digits, like every numeric readout
	field->maxLength = 12;
	ui::TextField* raw = field;
	field->onChange = [raw, commit = std::move(commit)] {
		const std::string& t = raw->text;
		float v = 0.0f;
		const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
		if (ec == std::errc() && p == t.data() + t.size()) commit(v);
	};
}
} // namespace

GenerateDialog::GenerateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void GenerateDialog::Open(const std::string& levelLabel) {
	m_open = true;
	m_level = levelLabel;
	m_uiRebuild = false;
	BuildUI();
}

void GenerateDialog::BuildUI() {
	m_ui.Clear();
	DialogChrome chrome = BuildDialogChrome(
		m_ui, kPanel,
		std::format("{} — {}", loc::Tr("map.gen.title"), m_level), m_closeIcon,
		[this] { Close(); });

	struct Row {
		const char* key;
		float value;
		std::function<void(float)> set;
	};
	// One table, so adding a knob is one row rather than a block of layout.
	const Row rows[] = {
		{"map.gen.width", static_cast<float>(m_params.width),
		 [this](float v) { m_params.width = std::clamp(static_cast<int>(v), 8, 128); }},
		{"map.gen.height", static_cast<float>(m_params.height),
		 [this](float v) { m_params.height = std::clamp(static_cast<int>(v), 8, 128); }},
		{"map.gen.rooms", static_cast<float>(m_params.rooms),
		 [this](float v) { m_params.rooms = std::clamp(static_cast<int>(v), 1, 40); }},
		{"map.gen.branching", m_params.branching,
		 [this](float v) { m_params.branching = std::clamp(v, 0.0f, 1.0f); }},
		{"map.gen.locks", static_cast<float>(m_params.locks),
		 [this](float v) { m_params.locks = std::clamp(static_cast<int>(v), 0, 8); }},
		{"map.gen.difficulty", m_params.difficulty,
		 [this](float v) { m_params.difficulty = std::clamp(v, 0.0f, 1.0f); }},
		{"map.gen.reward", m_params.reward,
		 [this](float v) { m_params.reward = std::clamp(v, 0.0f, 1.0f); }},
		{"map.gen.seed", static_cast<float>(m_params.seed),
		 [this](float v) { m_params.seed = static_cast<u32>(std::max(0.0f, v)); }},
	};
	for (const Row& r : rows) {
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr(r.key))->centerV = true;
		NumField(*row, r.value, r.set);
	}
	chrome.body->Space(ui::Len::Fill());

	chrome.footer->Space(ui::Len::Fill());
	// A new seed, so "give me a different one" does not mean typing a number.
	// Rebuilding is DEFERRED: this fires from inside the widget tree, and
	// Clear() from a callback dangles the caller (the m_pendingLanguage rule).
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.gen.roll"), [this] {
		std::random_device rd;
		m_params.seed = rd();
		m_uiRebuild = true;
	});
	chrome.footer->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.gen.go"), [this] {
		if (onGenerate) onGenerate(m_params);
	});
	chrome.footer->Space(ui::Len::Fill());
}

void GenerateDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));
	if (m_uiRebuild) {
		m_uiRebuild = false;
		BuildUI();
	}
	if (input.WasKeyPressed(VK_ESCAPE)) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void GenerateDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
