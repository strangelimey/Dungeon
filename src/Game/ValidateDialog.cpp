// ============================================================================
// Game/ValidateDialog.cpp — see ValidateDialog.h.
// ============================================================================
#include "Game/ValidateDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"
#include "UI/Layout.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {
// A wide card: these rows carry a sentence, not a word. Fixed height rather
// than sized to the row count (the InspectPicker trick) because the list can be
// long — it scrolls instead, and a panel that changed size with the number of
// faults would jump under the pointer between runs.
constexpr gfx::Rect kPanel{0.16f, 0.16f, 0.68f, 0.66f};
constexpr float kRowH = 1.15f; // rem-ish row height through FormRow
} // namespace

ValidateDialog::ValidateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void ValidateDialog::Open(std::vector<validate::Issue> issues) {
	m_open = true;
	m_issues = std::move(issues);
	m_errors = 0;
	for (const validate::Issue& i : m_issues)
		if (i.severity == validate::Severity::Error) ++m_errors;
	BuildUI();
}

void ValidateDialog::BuildUI() {
	m_ui.Clear();
	const std::string title =
		m_issues.empty()
			? loc::Tr("map.check.title")
			: std::format("{} — {}", loc::Tr("map.check.title"),
						  loc::Format("map.check.summary", m_errors,
									  m_issues.size() - m_errors));
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, title, m_closeIcon,
											[this] { Close(); },
											/*withFooter*/ false);

	if (m_issues.empty()) {
		chrome.body->Row<ui::Label>(FormRow(), loc::Tr("map.check.clean"));
		chrome.body->Space(ui::Len::Fill());
		return;
	}

	// Rows live in a ScrollArea over a content-sized Stack: the shared control
	// owns the scrolling, clipping, wheel and thumb (UI/Controls.h), and
	// `fitContent` is what lets it know how far the rows actually run.
	ui::ScrollArea* scroll = chrome.body->Row<ui::ScrollArea>(ui::Len::Fill());
	ui::Stack* rows = scroll->Add<ui::Stack>(gfx::Rect{0, 0, 1, 1});
	rows->fitContent = true;

	for (const validate::Issue& is : m_issues) {
		// "ERR  showcase 13,19   This door can never be opened — ..."
		// The severity leads because it is what you triage on; the location
		// follows because it is what you act on.
		std::string where = is.level;
		if (is.x >= 0) where += std::format(" {},{}", is.x, is.z);
		const std::string label =
			std::format("{}  {}   {}",
						is.severity == validate::Severity::Error ? "!" : "?", where,
						loc::Format(is.messageKey, is.a, is.b));
		const std::string lvl = is.level;
		const int x = is.x, z = is.z;
		// ui::Button centres its label, which is right for a command and wrong
		// for a list of sentences of differing length — centred, the severity
		// marks fail to line up and the eye cannot scan the column. The rows are
		// PADDED into alignment instead of teaching the shared control a new
		// mode for one caller: cheap here, and no new state on a widget every
		// screen uses.
		rows->Row<ui::Button>(FormRow(kRowH), label, [this, lvl, x, z] {
			if (onJump) onJump(lvl, x, z);
		});
	}
}

void ValidateDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));
	if (input.WasKeyPressed(VK_ESCAPE)) {
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void ValidateDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
