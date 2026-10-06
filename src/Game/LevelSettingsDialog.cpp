// ============================================================================
// Game/LevelSettingsDialog.cpp — see LevelSettingsDialog.h.
// ============================================================================
#include "Game/LevelSettingsDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"
#include "UI/Layout.h"
#include "UI/Skin.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace dungeon::game {

namespace {
// A compact centered card (four rows + footer), the BalanceDialog proportions
// shrunk. The panel is the only rect now; the card inside it is stacked
// (Game/DialogLayout.h).
// Wide enough for the TITLE: "Level settings — <stem>" is drawn at
// kDialogTitleScale, and the old 0.28-wide card could not hold it — the stem
// ran out past the panel edge and under the close box long before any of this
// was stacked. A card has to be sized for the text it carries.
// Grown 0.48 -> 0.54 when the tags row landed. The Stack would have absorbed
// the row into its trailing Fill without complaint, which is exactly why the
// height is adjusted deliberately: a card that silently swallows its own slack
// looks fine until the row after next has nowhere to go. 0.54 -> 0.60 for the
// UI material row, and -> 0.68 for its sample strip, for the same reason.
constexpr gfx::Rect kPanel{0.30f, 0.16f, 0.40f, 0.68f};
// A settings row's label column against its value column.
constexpr float kLabelFill = 1.4f, kFieldFill = 1.0f;

// A numeric text field: shows `value` ({:g}), and while edited writes every
// PARSEABLE state back through `commit` (the live-apply pattern the Balance
// dialog uses; an in-progress "" / "-" / "0." just waits). Returns the field.
ui::TextField* AddNumericField(ui::Stack& row, ui::Len len, float value,
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
	return field;
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
							   float ambient, const std::string& tags,
							   const std::string& uiStone,
							   const std::string& dungeonStone) {
	m_open = true;
	m_stem = stem;
	m_dust = m_oDust = dust;
	m_haze = m_oHaze = haze;
	m_ambient = m_oAmbient = ambient;
	m_tags = tags;
	m_uiStone = uiStone;
	m_dungeonStone = dungeonStone;
	m_stones = stoneOrder ? stoneOrder() : InstalledUiStones();
	// The chrome wears this level's material for as long as the dialog is up.
	if (onPreviewStone) onPreviewStone(PreviewName());
	m_editName = false;
	m_uiRebuild = false;
	BuildUI();
}

bool LevelSettingsDialog::TypeNumber(size_t row, const std::string& text) {
	if (!m_open || row >= kNumberRows || !m_numberFields[row]) return false;
	ui::TextField* field = m_numberFields[row];
	field->text = text;
	if (field->onChange) field->onChange();
	return true;
}

void LevelSettingsDialog::BuildUI() {
	m_ui.Clear();
	m_nameField = nullptr;
	m_sampleBox = nullptr;
	m_numberFields = {};
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
	const Row rows[kNumberRows] = {{"map.level.dust", &m_dust},
								   {"map.level.haze", &m_haze},
								   {"map.level.ambient", &m_ambient}};
	for (size_t i = 0; i < kNumberRows; ++i) {
		const Row& r = rows[i];
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr(r.labelKey))->centerV =
			true;
		m_numberFields[i] = AddNumericField(*row, ui::Len::Fill(kFieldFill), *r.value,
											[this, v = r.value](float f) {
												*v = f;
												Apply();
											});
	}
	// The tags row. A text field, not a dropdown: tags are free-form words that
	// content joins by being tagged, so there is no closed list to offer — and
	// typing one no catalog carries yet is how a tag gets started, not an error.
	{
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		// A wider field than the numeric rows get: those hold "0.075", this holds
		// a list of words. The clip in ui::TextField keeps a long list inside its
		// box either way, but a value you cannot read without focusing the field
		// is still a worse row than one you can.
		row->Row<ui::Label>(ui::Len::Fill(1.0f), loc::Tr("map.level.tags"))
			->centerV = true;
		auto* field = row->Row<ui::TextField>(ui::Len::Fill(1.4f), m_tags);
		field->maxLength = 48;
		ui::TextField* raw = field;
		raw->onChange = [this, raw] {
			// Records are whitespace-tokenised, so SPACE is the separator here
			// and everything a tag may not contain is stripped as typed — the
			// DoorInspector name-filter idiom, one character class wider.
			std::erase_if(raw->text, [](char ch) {
				const unsigned char u = static_cast<unsigned char>(ch);
				return !(std::isalnum(u) || ch == '_' || ch == '-' || ch == ' ');
			});
			m_tags = raw->text;
		};
	}
	// The UI material row: "the dungeon's" first (no override), then every
	// installed material by its player-facing name. A stem the level names but
	// the folder no longer holds is kept as a choice, so opening the dialog never
	// silently changes it.
	{
		ui::Stack* row = chrome.body->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(1.0f), loc::Tr("map.level.uistone"))->centerV =
			true;
		if (!m_uiStone.empty() &&
			std::find(m_stones.begin(), m_stones.end(), m_uiStone) == m_stones.end())
			m_stones.push_back(m_uiStone);
		// Each row shows its material's thumbnail; "the dungeon's" shows the
		// dungeon's, so the choice reads as what it will look like.
		std::vector<std::string> labels{loc::Tr("map.level.uistone.dungeon")};
		std::vector<const gfx::Texture*> icons{
			thumbFor && !m_dungeonStone.empty() ? thumbFor(m_dungeonStone) : nullptr};
		int selected = 0;
		for (size_t i = 0; i < m_stones.size(); ++i) {
			labels.push_back(loc::Tr("stone." + m_stones[i]));
			icons.push_back(thumbFor ? thumbFor(m_stones[i]) : nullptr);
			if (m_stones[i] == m_uiStone) selected = static_cast<int>(i) + 1;
		}
		auto* pick = row->Row<ui::DropDown>(
			ui::Len::Fill(1.4f), std::move(labels), selected, [this](int index) {
				m_uiStone = index > 0 && index <= static_cast<int>(m_stones.size())
								? m_stones[static_cast<size_t>(index - 1)]
								: std::string();
				if (onPreviewStone) onPreviewStone(PreviewName()); // shown at once
			});
		pick->icons = std::move(icons);
		// Category buttons over the open list: all / light / dark / each kind.
		// "The dungeon's" is a choice of who decides, so it shows under all.
		if (stoneFilterLabels && stoneFilterBits) {
			pick->filterLabels = stoneFilterLabels();
			if (stoneFilterColors) pick->filterColors = stoneFilterColors();
			pick->itemFilters.push_back(~0u);
			for (const std::string& s : m_stones) pick->itemFilters.push_back(stoneFilterBits(s));
		}
		// This dialog's rows are already tall; at the default 2.2x only four of
		// ~30 materials showed at once.
		pick->iconRowScale = 1.5f;
	}
	// The sample strip: a slab of game chrome in the material being previewed
	// (drawn in Render with the game's skin - this dialog's own look is flat),
	// under the column the dropdown sits in.
	{
		ui::Stack* row = chrome.body->Row<ui::Stack>(ui::Len::Fixed(3.2f), true);
		row->gapRem = 0.5f;
		row->Row<ui::Box>(ui::Len::Fill(1.0f));
		m_sampleBox = row->Row<ui::Box>(ui::Len::Fill(1.4f));
		m_sampleBox->debugName = "MaterialSample";
	}
	chrome.body->Space(ui::Len::Fill()); // the rows sit at the top

	chrome.footer->Space(ui::Len::Fill());
	FooterIcon(*chrome.footer, m_device, "save", loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_dust, m_haze, m_ambient, m_tags, m_uiStone);
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

	// An open material list takes the Esc first (it closes itself in the walk
	// below); the dialog, and the numbers it is previewing, stay (C81).
	if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.PopupOpen()) {
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
	DrawSample(batch);        // BEFORE the widgets, so an open list covers it
	m_ui.Render(batch, w, h); // title/rename, rows, footer — all widgets
}

// A panel face holding a button face and two item slots, in the game's skin -
// so whatever material is previewed is SEEN, inside an editor that draws its
// own chrome flat. Its area is an empty Box row the layout placed (last
// frame's rect, which only moves when the window does).
void LevelSettingsDialog::DrawSample(gfx::SpriteBatch& batch) {
	if (!sampleSkin || !m_sampleBox) return;
	const gfx::Rect r = m_sampleBox->Pixel();
	if (r.w <= 0.0f || r.h <= 0.0f) return;
	ui::DrawFace(batch, r, *sampleSkin, ui::Face::Panel, {1, 1, 1, 1});
	const float pad = r.h * 0.18f, side = r.h - 2.0f * pad;
	const gfx::Rect button{r.x + pad, r.y + pad, std::max(0.0f, r.w - 3.0f * pad - 2.0f * side - pad), side};
	ui::DrawFace(batch, button, *sampleSkin, ui::Face::Button, {1, 1, 1, 1});
	for (int i = 0; i < 2; ++i)
		ui::DrawFace(batch,
					 {r.x + r.w - pad - side - static_cast<float>(i) * (side + pad), r.y + pad,
					  side, side},
					 *sampleSkin, ui::Face::Slot, {1, 1, 1, 1});
}

} // namespace dungeon::game
