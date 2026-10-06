// ============================================================================
// Game/NewWorldDialog.cpp - see NewWorldDialog.h.
// ============================================================================
#include "Game/NewWorldDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>

namespace dungeon::game {

namespace {
constexpr gfx::Rect kPanel{0.25f, 0.18f, 0.50f, 0.64f};
// With the wizard picked its four rows join the card, so the card grows (up and
// down about the same centre) rather than squeezing every other row.
constexpr gfx::Rect kWizardPanel{0.25f, 0.06f, 0.50f, 0.88f};
constexpr float kLevelW = 1.6f; // the level dropdown, in FooterButton widths
// The wizard's sizes: the map's side in squares, small / medium / large.
constexpr int kSizes[] = {24, 32, 44};
constexpr const char* kSizeKeys[] = {"map.newworld.small", "map.newworld.medium",
									 "map.newworld.large"};

// A world's name is a FOLDER name: filtered as it is typed, so the field never
// shows a name that will not be the one written (CreateWorld filters again).
void FilterId(std::string& text) {
	std::erase_if(text, [](char ch) {
		const unsigned char u = static_cast<unsigned char>(ch);
		return !(std::isalnum(u) || ch == '_' || ch == '-');
	});
}
} // namespace

NewWorldDialog::NewWorldDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void NewWorldDialog::Open() {
	m_open = true;
	m_name.clear();
	m_made.clear();
	m_spec = {};
	m_levels = onLevels ? onLevels() : std::vector<std::string>{};
	m_tagChoices = onTags ? onTags() : std::vector<std::string>{};
	m_styleChoices = onStyles ? onStyles() : std::vector<std::pair<std::string, std::string>>{};
	m_note = loc::Tr("map.newworld.note");
	m_uiRebuild = false;
	BuildUI();
}

void NewWorldDialog::SetSource(NewWorldSpec::Source source, const std::string& level) {
	m_spec.source = source;
	if (source == NewWorldSpec::Source::CopyLevel)
		m_spec.level = !level.empty() ? level : m_levels.empty() ? std::string() : m_levels.front();
	m_uiRebuild = true; // the picked choice draws active
}

void NewWorldDialog::SetWizard(const std::string& tag, int size, float difficulty,
							   u32 seed) {
	m_spec.source = NewWorldSpec::Source::Wizard;
	m_spec.tag = tag;
	m_spec.size = size;
	m_spec.difficulty = std::clamp(difficulty, 0.0f, 1.0f);
	m_spec.seed = seed;
	m_uiRebuild = true;
}

void NewWorldDialog::SetStyle(const std::string& id) {
	m_spec.style = id;
	// The style decides the tags, and its row hides the wizard's tag row - so a
	// tag picked before cannot go on steering the floor from out of sight.
	if (!id.empty()) m_spec.tag.clear();
	m_uiRebuild = true;
}

void NewWorldDialog::Create(const std::string& typed) {
	std::string name = typed;
	FilterId(name);
	if (name.empty()) {
		m_note = loc::Tr("map.worlds.noname");
		m_uiRebuild = true;
		return;
	}
	const auto [made, problem] =
		onCreate ? onCreate(name, m_spec) : std::pair<std::string, std::string>{};
	if (made.empty()) {
		m_note = problem.empty() ? loc::Tr("map.worlds.failed") : problem;
		m_uiRebuild = true;
		return;
	}
	m_made = made;
	m_name.clear();
	m_note = loc::Format("map.newworld.done", made);
	m_uiRebuild = true; // Switch now appears
}

void NewWorldDialog::SwitchNow() {
	if (m_made.empty()) return;
	if (onSwitch && onSwitch(m_made)) {
		m_note = loc::Format("map.worlds.relaunching", m_made);
		Close();
		return;
	}
	m_note = loc::Format("map.worlds.missing", m_made);
	m_uiRebuild = true;
}

void NewWorldDialog::ApplyPending() {
	if (!m_uiRebuild) return;
	m_uiRebuild = false;
	BuildUI();
}

void NewWorldDialog::BuildUI() {
	m_ui.Clear();
	m_noteLabel = nullptr; // dies with the tree
	const bool wizard = m_spec.source == NewWorldSpec::Source::Wizard;
	m_panel = wizard ? kWizardPanel : kPanel;
	DialogChrome chrome = BuildDialogChrome(m_ui, m_panel, loc::Tr("map.newworld.title"),
											m_closeIcon, [this] { Close(); });

	// The name.
	{
		auto* field = chrome.body->Row<ui::TextField>(FormRow(), m_name);
		field->placeholder = loc::Tr("map.worlds.newname");
		field->maxLength = 32;
		ui::TextField* raw = field;
		raw->onChange = [this, raw] {
			FilterId(raw->text);
			m_name = raw->text;
		};
		raw->onSubmit = [this] { Create(m_name); };
	}
	chrome.body->Row<ui::Separator>(ui::Len::Fixed(0.5f));

	// How it starts: one button per way, the picked one drawn active - a set of
	// radio buttons made of the buttons every dialog already has.
	using S = NewWorldSpec::Source;
	const auto choice = [&](ui::Stack& row, S source, const char* key) {
		auto* b = row.Row<ui::Button>(ui::Len::Fill(), loc::Tr(key),
									  [this, source] { SetSource(source, m_spec.level); });
		b->active = m_spec.source == source;
	};
	choice(*chrome.body->Row<ui::Stack>(FormRow(), true), S::Blank, "map.newworld.blank");
	choice(*chrome.body->Row<ui::Stack>(FormRow(), true), S::CopyWorld, "map.newworld.copy");
	{
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		choice(*row, S::CopyLevel, "map.newworld.level");
		// Which level: this world's, in manifest order. Picking one also picks
		// the "Copy one level" way - choosing a level means you meant it.
		int sel = 0;
		for (size_t i = 0; i < m_levels.size(); ++i)
			if (m_levels[i] == m_spec.level) sel = static_cast<int>(i);
		row->Row<ui::DropDown>(FooterButton(kLevelW), m_levels, sel, [this](int i) {
			if (i >= 0 && i < static_cast<int>(m_levels.size()))
				SetSource(S::CopyLevel, m_levels[static_cast<size_t>(i)]);
		});
	}
	choice(*chrome.body->Row<ui::Stack>(FormRow(), true), S::Wizard, "map.newworld.wizard");
	const auto labelled = [&](const char* key) {
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(0.4f), loc::Tr(key))->centerV = true;
		return row;
	};
	// THE STYLE (Phase 7), for the two ways that start from the template: the
	// world receives it from the library and its first floor is built in it.
	// The copies bring this world's own styles, so the row is theirs to skip.
	if (m_spec.source == S::Blank || wizard) {
		std::vector<std::string> items{loc::Tr("map.newworld.nostyle")};
		int sel = 0;
		for (size_t i = 0; i < m_styleChoices.size(); ++i) {
			items.push_back(m_styleChoices[i].second);
			if (m_styleChoices[i].first == m_spec.style) sel = static_cast<int>(i) + 1;
		}
		labelled("map.newworld.style")
			->Row<ui::DropDown>(ui::Len::Fill(0.6f), items, sel, [this](int i) {
				SetStyle(i > 0 && i <= static_cast<int>(m_styleChoices.size())
							 ? m_styleChoices[static_cast<size_t>(i - 1)].first
							 : std::string());
			});
	}
	if (wizard) {
		// The wizard's knobs, only while it is the way picked: a first dungeon
		// generated from the template's content (docs/level-building.md).
		if (m_spec.style.empty()) { // TAGS, unless the style decides them
			// The content tag monsters, loot and surfaces are drawn by.
			std::vector<std::string> items{loc::Tr("map.newworld.anytag")};
			items.insert(items.end(), m_tagChoices.begin(), m_tagChoices.end());
			int sel = 0;
			for (size_t i = 0; i < m_tagChoices.size(); ++i)
				if (m_tagChoices[i] == m_spec.tag) sel = static_cast<int>(i) + 1;
			labelled("map.newworld.tag")
				->Row<ui::DropDown>(ui::Len::Fill(0.6f), items, sel, [this](int i) {
					m_spec.tag = i > 0 && i <= static_cast<int>(m_tagChoices.size())
									   ? m_tagChoices[static_cast<size_t>(i - 1)]
									   : std::string();
				});
		}
		{ // SIZE: three, in squares a side.
			std::vector<std::string> items;
			int sel = 1;
			for (size_t i = 0; i < std::size(kSizes); ++i) {
				items.push_back(loc::Format(kSizeKeys[i], kSizes[i]));
				if (kSizes[i] == m_spec.size) sel = static_cast<int>(i);
			}
			labelled("map.newworld.size")
				->Row<ui::DropDown>(ui::Len::Fill(0.6f), items, sel, [this](int i) {
					if (i >= 0 && i < static_cast<int>(std::size(kSizes)))
						m_spec.size = kSizes[static_cast<size_t>(i)];
				});
		}
		// DIFFICULTY: which monsters, and (the wizard's choice) how many.
		chrome.body->Row<ui::Slider>(FormRow(1.9f), loc::Tr("map.newworld.difficulty"), 0.0f,
									 1.0f, m_spec.difficulty,
									 [this](float f) { m_spec.difficulty = f; });
		{ // SEED: the same seed and knobs make the same floor; Reroll for another.
			ui::Stack* row = labelled("map.newworld.seed");
			auto* field = row->Row<ui::TextField>(ui::Len::Fill(0.35f), std::to_string(m_spec.seed));
			field->maxLength = 9;
			ui::TextField* raw = field;
			raw->onChange = [this, raw] {
				std::erase_if(raw->text, [](char ch) { return !std::isdigit(static_cast<unsigned char>(ch)); });
				m_spec.seed = raw->text.empty() ? 1u : static_cast<u32>(std::stoul(raw->text));
			};
			RowIcon(*row, m_device, "generate", loc::Tr("map.newworld.reroll"), [this] {
				// Deterministic from the current seed (the generator's rule: a
				// level is its knobs), so a reroll can itself be retraced.
				m_spec.seed = m_spec.seed * 1664525u + 1013904223u;
				m_spec.seed %= 1000000000u;
				if (m_spec.seed == 0) m_spec.seed = 1;
				m_uiRebuild = true;
			});
			row->Space(ui::Len::Fill(0.25f)); // the field keeps the width it had
		}
	}

	chrome.body->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	// What the last click did, or what the next one will - a FIXED band (the
	// WorldsDialog note's): as a Fill row it got whatever the choices left,
	// which at some window heights was nothing, and the text sat on Create.
	ui::Label* note = chrome.body->Row<ui::Label>(ui::Len::Fixed(2.0f), m_note);
	note->dim = true;
	note->centerV = true;
	note->fontScale = 1.1f;
	m_noteLabel = note;

	// Create, and once a world is made, the way there.
	FooterIcon(*chrome.footer, m_device, "newworld", loc::Tr("map.newworld.create"),
			   [this] { Create(m_name); });
	if (!m_made.empty())
		FooterIcon(*chrome.footer, m_device, "enter", loc::Tr("map.newworld.switch"),
				   [this] { SwitchNow(); })
			->active = true;
}

void NewWorldDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);
	if (m_uiRebuild) {
		m_uiRebuild = false;
		BuildUI();
	}
	// An open list takes the Esc first and closes itself in the walk (C81).
	if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.PopupOpen()) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void NewWorldDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim what is behind
	const gfx::Rect panel{m_panel.x * w, m_panel.y * h, m_panel.w * w, m_panel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
