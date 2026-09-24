// ============================================================================
// Game/WorldsDialog.cpp — see WorldsDialog.h.
// ============================================================================
#include "Game/WorldsDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>

namespace dungeon::game {

namespace {
// A short list and one form row: narrower and shorter than the settings card.
// The list scrolls, so the card is sized for a handful of worlds, not all.
constexpr gfx::Rect kPanel{0.28f, 0.22f, 0.44f, 0.54f};

// A world's name is a FOLDER name — the id filter every authored name gets,
// applied as it is typed so the field never shows a name that will not be the
// one written. (CreateWorld filters again: the console reaches it too.)
void FilterId(std::string& text) {
	std::erase_if(text, [](char ch) {
		const unsigned char u = static_cast<unsigned char>(ch);
		return !(std::isalnum(u) || ch == '_' || ch == '-');
	});
}
} // namespace

WorldsDialog::WorldsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void WorldsDialog::Open(std::string openName) {
	m_open = true;
	m_openName = std::move(openName);
	m_worlds = onList ? onList() : std::vector<std::string>{};
	m_armed.clear();
	m_newName.clear();
	m_note = loc::Tr("map.worlds.note");
	m_uiRebuild = false;
	BuildUI();
}

void WorldsDialog::SetNote(std::string text) {
	// Written into the label rather than rebuilt, so typing in the name field
	// is never interrupted by a message about something else.
	m_note = std::move(text);
	if (m_noteLabel) m_noteLabel->text = m_note;
}

void WorldsDialog::ClickOpen(const std::string& name) {
	if (name == m_openName) return; // no button is offered for it
	// ARMED: the first click says what is about to happen, the second does it.
	// A different row's click moves the arm rather than switching, so a stray
	// click on the wrong row costs nothing.
	if (m_armed != name) {
		m_armed = name;
		m_note = loc::Format("map.worlds.confirm", name);
		m_uiRebuild = true; // the armed row's button changes its words
		return;
	}
	m_armed.clear();
	if (onSwitch && onSwitch(name)) {
		m_note = loc::Format("map.worlds.relaunching", name);
	} else {
		m_note = loc::Format("map.worlds.missing", name);
		// The row named something that is not there any more — re-read, so the
		// list stops offering it.
		if (onList) m_worlds = onList();
	}
	m_uiRebuild = true;
}

void WorldsDialog::Create(const std::string& typed) {
	std::string name = typed;
	FilterId(name);
	if (name.empty()) {
		SetNote(loc::Tr("map.worlds.noname"));
		return;
	}
	// Refused up front with its OWN reason rather than left to the owner's
	// single "" — a duplicate and a failed write are two different mistakes,
	// and one sentence for both hides one of them (W4's lesson).
	if (std::find(m_worlds.begin(), m_worlds.end(), name) != m_worlds.end()) {
		SetNote(loc::Format("map.worlds.exists", name));
		return;
	}
	const std::string made = onCreate ? onCreate(name) : std::string();
	if (made.empty()) {
		SetNote(loc::Tr("map.worlds.failed"));
		return;
	}
	if (onList) m_worlds = onList();
	m_newName.clear();
	// The new row is ARMED already: making a world is nearly always the
	// first half of going there, so the next click on its Open is the one
	// that relaunches — and the note says so.
	m_armed = made;
	m_note = loc::Format("map.worlds.created", made);
	m_uiRebuild = true;
}

void WorldsDialog::BuildUI() {
	m_ui.Clear();
	m_noteLabel = nullptr; // dies with the tree
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, loc::Tr("map.worlds.title"),
											m_closeIcon, [this] { Close(); });

	// The worlds, one row each: the name, then either the way there or a word
	// saying you are already in it. The running world has NO button — opening
	// the world you are in would relaunch into exactly where you are.
	ui::ScrollArea* scroll = chrome.body->Row<ui::ScrollArea>(ui::Len::Fill());
	ui::Stack* rows = scroll->Add<ui::Stack>(gfx::Rect{0, 0, 1, 1});
	rows->fitContent = true;
	rows->padRem = 0.5f;
	rows->gapRem = 0.5f;
	for (const std::string& name : m_worlds) {
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(), name)->centerV = true;
		if (name == m_openName) {
			ui::Label* here = row->Row<ui::Label>(FooterButton(1.6f),
												  loc::Tr("map.worlds.here"));
			here->centerV = true;
			here->dim = true;
			continue;
		}
		const bool armed = name == m_armed;
		auto* btn = row->Row<ui::Button>(
			FooterButton(1.6f),
			loc::Tr(armed ? "map.worlds.relaunch" : "map.worlds.open"),
			[this, name] { ClickOpen(name); });
		btn->active = armed;
	}

	chrome.body->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	{
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		auto* field = row->Row<ui::TextField>(ui::Len::Fill(), m_newName);
		field->placeholder = loc::Tr("map.worlds.newname");
		field->maxLength = 32;
		ui::TextField* raw = field;
		raw->onChange = [this, raw] {
			FilterId(raw->text);
			m_newName = raw->text;
		};
		raw->onSubmit = [this] { Create(m_newName); };
		row->Row<ui::Button>(FooterButton(1.6f), loc::Tr("map.worlds.create"),
							 [this] { Create(m_newName); });
	}
	// What the last click did, or what the next one will. One line, fine print
	// (the WorldSettingsDialog note), and held so it can change in place.
	{
		ui::Label* note = chrome.body->Row<ui::Label>(ui::Len::Fixed(2.0f), m_note);
		note->dim = true;
		note->centerV = true;
		note->fontScale = 1.1f;
		m_noteLabel = note;
	}
}

void WorldsDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	if (m_uiRebuild) {
		m_uiRebuild = false;
		BuildUI();
	}
	if (input.WasKeyPressed(VK_ESCAPE)) {
		// Esc DISARMS before it closes: an armed Open is a question the dialog
		// has asked, and Esc is the way to say no without leaving.
		if (!m_armed.empty()) {
			m_armed.clear();
			m_note = loc::Tr("map.worlds.note");
			BuildUI();
			return;
		}
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void WorldsDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
						  float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the world behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
