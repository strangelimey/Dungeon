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

// Tall: the knob tabs scroll, but the report's three lines and the where-it-
// lands line all take from the same height, and P1's 0.72 left two sliders
// visible. Wide enough for the longest report line in every language - the
// uioverlap audit caught the complexity line 11px over at 0.44.
constexpr gfx::Rect kPanel{0.25f, 0.06f, 0.50f, 0.88f};
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
	m_report = {}; // nothing made yet: no report can be about it
	m_uiRebuild = false;
	BuildUI();
}

void GenerateDialog::OpenRegenerate(const std::string& levelStem) {
	m_open = true;
	m_mode = Mode::Regenerate;
	m_level = levelStem;
	if (levelStem != m_reportLevel) m_report = {};
	m_uiRebuild = false;
	BuildUI();
}

void GenerateDialog::BuildUI() {
	if (m_tabs) m_activeTab = m_tabs->ActiveTab();
	m_ui.Clear();
	m_tabs = nullptr;
	m_seedField = nullptr;
	m_reportLabels = {};
	// The title stays SHORT and the where-it-lands line goes in the body: a
	// dungeon's display name has no length limit, and the title slot does (the
	// uioverlap sweep caught "New level in The Crypt" running 18px past it).
	const std::string title =
		m_mode == Mode::Create ? loc::Tr("map.gen.newtitle")
							   : std::format("{} - {}", loc::Tr("map.gen.title"), m_level);
	DialogChrome chrome =
		BuildDialogChrome(m_ui, kPanel, title, m_closeIcon, [this] { Close(); });
	if (m_mode == Mode::Create)
		chrome.body->Row<ui::Label>(FormRow(), m_where)->centerV = true;

	// One tab per knob group, one row per knob - both walked off the table, so
	// nothing here names a knob.
	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(), 0.07f);
	const auto tabs = generate::KnobTabs();
	for (const char* tab : tabs) m_tabs->AddTab(loc::Tr(tab));
	// ...and one that is not a knob group: saved recipes (P4b).
	const size_t presetsTab = m_tabs->AddTab(loc::Tr("map.gen.tab.presets"));
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

		if (knob.kind == generate::KnobKind::Choice) {
			// A dropdown of what the OWNER offers (tags, levels), "as before"
			// first. A value no longer on offer - a level since renamed, kept in
			// a preset - stays listed as itself, so it is visible rather than
			// silently swapped for something else.
			std::vector<std::pair<std::string, std::string>> choices =
				choicesFor ? choicesFor(knob.key)
						   : std::vector<std::pair<std::string, std::string>>{};
			const std::string current = knob.getText(m_params);
			if (std::ranges::none_of(choices, [&](const auto& c) { return c.first == current; }))
				choices.push_back({current, current});
			std::vector<std::string> labels;
			int selected = 0;
			for (size_t i = 0; i < choices.size(); ++i) {
				labels.push_back(choices[i].second);
				if (choices[i].first == current) selected = static_cast<int>(i);
			}
			ui::Stack* row = page.Row<ui::Stack>(FormRow(), true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill), label)->centerV = true;
			row->Row<ui::DropDown>(ui::Len::Fill(kFieldFill), std::move(labels), selected,
								   [this, k, choices](int i) {
									   if (i >= 0 && i < static_cast<int>(choices.size()))
										   k->setText(m_params, choices[static_cast<size_t>(i)].first);
								   });
			continue;
		}
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
			// number. Written straight into the field rather than rebuilding -
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

		if (knob.kind == generate::KnobKind::Bool) {
			page.Row<ui::Checkbox>(FormRow(), label, value >= 0.5,
								   [this, k](bool on) {
									   generate::SetKnob(*k, m_params, on ? 1.0 : 0.0);
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

	BuildPresetsPage(*TabStack(*m_tabs, presetsTab));

	// What the last run BUILT, against what it was asked for. Empty until
	// something has been generated here (see SetReport).
	// Only the lines there ARE: four empty rows reserved before anything has
	// been generated squeezed the knob tabs down to a single visible slider
	// above a blank band (seen in the P5 screenshot). SetReport rebuilds the
	// form when a report arrives with no rows yet to hold it.
	for (size_t i = 0; i < m_report.size(); ++i) {
		if (m_report[i].empty()) continue;
		m_reportLabels[i] = chrome.body->Row<ui::Label>(FormRow(), m_report[i]);
		m_reportLabels[i]->centerV = true;
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
		// THE PLAY-TEST LOOP in one click (P5): make it, and walk into it.
		chrome.footer->Row<ui::Button>(
			FooterButton(1.6f), loc::Tr("map.gen.createplay"), [this] {
				if (!onCreate) return;
				if (onKnobsUsed) onKnobsUsed(m_params);
				const std::string stem = onCreate(m_dungeon, &m_params);
				if (!stem.empty() && onPlay) onPlay(stem);
			});
	} else {
		chrome.footer->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.gen.go"),
									   [this] {
										   if (onKnobsUsed) onKnobsUsed(m_params);
										   if (onGenerate) onGenerate(m_params);
									   });
		// ...and walk into what the rerolls made, when it looks right.
		chrome.footer->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.gen.play"),
									   [this] {
										   if (onPlay) onPlay(m_level);
									   });
	}
	chrome.footer->Space(ui::Len::Fill());
}

// The Presets tab: pick one and Load or Delete it; name the current knobs and
// Save them. Every change REBUILDS the form (deferred - these fire from inside
// the tree): a load moves every slider, and a save or delete changes the list.
void GenerateDialog::BuildPresetsPage(ui::Stack& page) {
	const std::vector<std::string> names =
		presetNames ? presetNames() : std::vector<std::string>{};
	m_presetPick = std::clamp(m_presetPick, 0, std::max(0, static_cast<int>(names.size()) - 1));
	auto labelled = [&](const char* key) {
		ui::Stack* row = page.Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr(key))->centerV = true;
		return row;
	};
	if (names.empty()) {
		page.Row<ui::Label>(FormRow(), loc::Tr("map.gen.preset.none"))->centerV = true;
	} else {
		labelled("map.gen.preset.pick")
			->Row<ui::DropDown>(ui::Len::Fill(kFieldFill), names, m_presetPick,
								[this](int i) { m_presetPick = i; });
		ui::Stack* buttons = page.Row<ui::Stack>(FormRow(1.4f), true);
		buttons->gapRem = 0.5f;
		buttons->Space(ui::Len::Fill());
		buttons->Row<ui::Button>(FooterButton(), loc::Tr("map.gen.preset.load"), [this, names] {
			const std::string& name = names[static_cast<size_t>(m_presetPick)];
			if (onPresetLoad && onPresetLoad(name, m_params))
				m_presetNote = loc::Format("map.gen.preset.loaded", name);
			m_uiRebuild = true;
		});
		buttons->Row<ui::Button>(FooterButton(), loc::Tr("map.gen.preset.delete"), [this, names] {
			const std::string& name = names[static_cast<size_t>(m_presetPick)];
			if (onPresetDelete && onPresetDelete(name))
				m_presetNote = loc::Format("map.gen.preset.deleted", name);
			m_uiRebuild = true;
		});
	}
	ui::TextField* field =
		labelled("map.gen.preset.name")->Row<ui::TextField>(ui::Len::Fill(kFieldFill), m_presetName);
	field->maxLength = 32;
	field->onChange = [this, field] { m_presetName = field->text; };
	ui::Stack* save = page.Row<ui::Stack>(FormRow(1.4f), true);
	save->Space(ui::Len::Fill());
	save->Row<ui::Button>(FooterButton(), loc::Tr("map.gen.preset.save"), [this] {
		if (!onPresetSave) return;
		const std::string saved = onPresetSave(m_presetName, m_params);
		m_presetNote = saved.empty() ? loc::Tr("map.gen.preset.failed")
									 : loc::Format("map.gen.preset.saved", saved);
		m_uiRebuild = true;
	});
	page.Row<ui::Label>(FormRow(), m_presetNote)->centerV = true;
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
