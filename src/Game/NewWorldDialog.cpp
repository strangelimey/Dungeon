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
constexpr float kLevelW = 1.6f; // the level dropdown, in FooterButton widths

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
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, loc::Tr("map.newworld.title"),
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
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.newworld.create"),
								   [this] { Create(m_name); });
	if (!m_made.empty())
		chrome.footer->Row<ui::Button>(FooterButton(1.3f), loc::Tr("map.newworld.switch"),
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
	if (input.WasKeyPressed(VK_ESCAPE)) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void NewWorldDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim what is behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
