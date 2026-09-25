// ============================================================================
// Game/GenerateDialog.cpp — see GenerateDialog.h.
// ============================================================================
#include "Game/GenerateDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "Game/GenerateKnobs.h"
#include "UI/Controls.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <random>

namespace dungeon::game {

namespace {

constexpr gfx::Rect kPanel{0.30f, 0.14f, 0.40f, 0.72f};
constexpr float kLabelFill = 1.3f, kFieldFill = 1.0f;

} // namespace

GenerateDialog::GenerateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void GenerateDialog::OpenCreate(const std::string& dungeonId,
								const std::string& where) {
	m_open = true;
	m_mode = Mode::Create;
	m_dungeon = dungeonId;
	m_where = where;
	m_uiRebuild = false;
	BuildUI();
}

void GenerateDialog::OpenRegenerate(const std::string& levelStem) {
	m_open = true;
	m_mode = Mode::Regenerate;
	m_level = levelStem;
	m_uiRebuild = false;
	BuildUI();
}

void GenerateDialog::BuildUI() {
	if (m_tabs) m_activeTab = m_tabs->ActiveTab();
	m_ui.Clear();
	m_tabs = nullptr;
	m_seedField = nullptr;
	// The title stays SHORT and the where-it-lands line goes in the body: a
	// dungeon's display name has no length limit, and the title slot does (the
	// uioverlap sweep caught "New level in The Crypt" running 18px past it).
	const std::string title =
		m_mode == Mode::Create ? loc::Tr("map.gen.newtitle")
							   : std::format("{} — {}", loc::Tr("map.gen.title"), m_level);
	DialogChrome chrome =
		BuildDialogChrome(m_ui, kPanel, title, m_closeIcon, [this] { Close(); });
	if (m_mode == Mode::Create)
		chrome.body->Row<ui::Label>(FormRow(), m_where)->centerV = true;

	// One tab per knob group, one row per knob — both walked off the table, so
	// nothing here names a knob.
	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(), 0.07f);
	const auto tabs = generate::KnobTabs();
	for (const char* tab : tabs) m_tabs->AddTab(loc::Tr(tab));
	m_tabs->SetActiveTab(m_activeTab);
	std::vector<ui::Stack*> pages;
	for (size_t i = 0; i < tabs.size(); ++i) pages.push_back(TabStack(*m_tabs, i));

	// Callbacks capture the knob BY POINTER: the table is a static constant, so
	// it outlives every widget built from it.
	for (const generate::Knob& knob : generate::Knobs()) {
		const generate::Knob* k = &knob;
		const auto it = std::find_if(tabs.begin(), tabs.end(), [&](const char* t) {
			return std::string_view(t) == knob.tab;
		});
		ui::Stack& page = *pages[static_cast<size_t>(it - tabs.begin())];
		const std::string label = loc::Tr(knob.label);
		const double value = knob.get(m_params);

		if (knob.kind == generate::KnobKind::Seed) {
			// Typed, not slid: a seed is an identity, not a quantity. Every
			// PARSEABLE state is written back as you type (an in-progress ""
			// simply waits), and Roll sits beside it.
			ui::Stack* row = page.Row<ui::Stack>(FormRow(), true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill), label)->centerV = true;
			auto* field = row->Row<ui::TextField>(
				ui::Len::Fill(kFieldFill),
				std::to_string(static_cast<u32>(value)));
			field->fontRole = ui::FontRole::Mono;
			field->fontScale = 1.0f; // sized to its digits, like every readout
			field->maxLength = 10;
			m_seedField = field;
			field->onChange = [this, k, field] {
				const std::string& t = field->text;
				double v = 0.0;
				const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
				if (ec == std::errc() && p == t.data() + t.size())
					generate::SetKnob(*k, m_params, v);
			};
			// A new seed, so "give me a different one" does not mean typing a
			// number. Written straight into the field rather than rebuilding —
			// the field is this tree's, and nothing is being cleared.
			row->Row<ui::Button>(FooterButton(0.8f), loc::Tr("map.gen.roll"),
								 [this, k] {
									 std::random_device rd;
									 generate::SetKnob(*k, m_params, rd());
									 if (m_seedField)
										 m_seedField->text = std::to_string(m_params.seed);
								 });
			continue;
		}

		// A Slider stacks its label OVER its track, so it asks for two lines.
		ui::Stack* row = page.Row<ui::Stack>(FormRow(1.9f), true);
		auto* slider = row->Row<ui::Slider>(
			ui::Len::Fill(), label, static_cast<float>(knob.lo),
			static_cast<float>(knob.hi), static_cast<float>(value),
			[this, k](float v) { generate::SetKnob(*k, m_params, v); });
		if (knob.kind == generate::KnobKind::Int) slider->SetDecimals(0);
	}

	chrome.footer->Space(ui::Len::Fill());
	if (m_mode == Mode::Create) {
		// The old [+] behaviour, kept: sometimes you want a blank canvas.
		chrome.footer->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.gen.empty"),
									   [this] {
										   if (onCreate) onCreate(m_dungeon, nullptr);
										   Close();
									   });
		chrome.footer->Row<ui::Button>(
			FooterButton(1.4f), loc::Tr("map.gen.create"), [this] {
				if (!onCreate) return;
				if (onKnobsUsed) onKnobsUsed(m_params);
				const std::string stem = onCreate(m_dungeon, &m_params);
				if (stem.empty()) return;
				// Straight into the reroll loop on what was just made. DEFERRED:
				// this fires from inside the tree the rebuild would clear.
				m_mode = Mode::Regenerate;
				m_level = stem;
				m_uiRebuild = true;
			});
	} else {
		chrome.footer->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.gen.go"),
									   [this] {
										   if (onKnobsUsed) onKnobsUsed(m_params);
										   if (onGenerate) onGenerate(m_params);
									   });
	}
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
