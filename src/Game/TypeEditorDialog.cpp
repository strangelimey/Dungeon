// ============================================================================
// Game/TypeEditorDialog.cpp — see TypeEditorDialog.h.
// ============================================================================
#include "Game/TypeEditorDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {
// Panel geometry as window fractions — the BalanceDialog card, a little taller
// (a schema can run to a dozen rows; the tab pages scroll past that).
constexpr gfx::Rect kPanel{0.26f, 0.12f, 0.48f, 0.76f};

// Row metrics inside a tab page (fractions of the page). A row past 1.0 is what
// makes the page scroll, so a long section simply runs on.
constexpr float kRowX = 0.04f, kRowW = 0.92f;
constexpr float kRowY0 = 0.03f, kRowH = 0.115f;
// A Slider row: its label sits OVER its track, so it is about two rows tall.
constexpr float kSliderRowH = 0.20f;
constexpr float kLabelW = 0.34f, kFieldX = 0.40f, kFieldW = 0.56f;

// "1"/"0" the way the catalogs write booleans.
const char* BoolText(bool on) { return on ? "1" : "0"; }

// Splits a space-separated option list ("floor wall").
std::vector<std::string> SplitOptions(std::string_view text) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && text[i] == ' ') ++i;
		const size_t start = i;
		while (i < text.size() && text[i] != ' ') ++i;
		if (i > start) out.emplace_back(text.substr(start, i - start));
	}
	return out;
}
} // namespace

TypeEditorDialog::TypeEditorDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void TypeEditorDialog::Open(Config cfg, std::span<const FieldSpec> schema) {
	m_open = true;
	m_busy = false;
	m_helpOpen = false;
	m_uiRebuild = false;
	m_cfg = std::move(cfg);
	m_cfg.rebake = false;
	m_schema = schema;
	m_touched.clear();
	m_notice.clear();
	m_editName = false;
	m_deleteArmed = false;
	// A fresh open starts on the first tab: null the (now stale) control so
	// BuildUI's tab-preservation reads 0, not the previously-closed dialog's tab.
	m_tabs = nullptr;

	// Tab order = the order the sections first appear in the schema table.
	m_sections.clear();
	for (const FieldSpec& spec : m_schema)
		if (std::find_if(m_sections.begin(), m_sections.end(), [&](const char* s) {
				return std::string_view(s) == spec.sectionKey;
			}) == m_sections.end())
			m_sections.push_back(spec.sectionKey);
	BuildUI();
}

std::string TypeEditorDialog::ValueOf(const FieldSpec& spec) const {
	if (const std::string* v = serialize::Find(m_cfg.fields, spec.key)) return *v;
	return spec.def;
}

bool TypeEditorDialog::Touched(std::string_view key) const {
	return std::find(m_touched.begin(), m_touched.end(), key) != m_touched.end();
}

void TypeEditorDialog::SetValue(const FieldSpec& spec, std::string value) {
	serialize::Set(m_cfg.fields, spec.key, std::move(value));
	if (!Touched(spec.key)) m_touched.emplace_back(spec.key);
}

void TypeEditorDialog::BuildUI() {
	// Read the open tab BEFORE Clear frees the control (a rebuild keeps the tab).
	const int activeTab = m_tabs ? m_tabs->ActiveTab() : 0;
	m_ui.Clear();
	m_tabs = nullptr; // the Clear just freed it; don't read it again below
	m_nameField = nullptr;
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, /*title*/ "", m_closeIcon,
											[this] { Close(); });
	if (m_editName) {
		// The title slot holds the rename field instead. Enter commits through
		// onRename; Esc or losing focus cancels.
		m_nameField =
			chrome.titleSlot->Add<ui::TextField>(gfx::Rect{0, 0, 1, 1}, m_cfg.id);
		// It stands in for the title, so it is sized as the title, not as a row.
		m_nameField->fontScale = ui::kDialogTitleScale;
		m_nameField->maxLength = 32;
		m_nameField->SetFocused(true);
		ui::TextField* raw = m_nameField;
		raw->onChange = [raw] {
			// A catalog id is a record word and an asset-safe name (the door /
			// level-stem rule) — strip anything else as it is typed.
			std::erase_if(raw->text, [](char ch) {
				const unsigned char u = static_cast<unsigned char>(ch);
				return !(std::isalnum(u) || ch == '_' || ch == '-');
			});
		};
		raw->onSubmit = [this, raw] {
			const std::string next = raw->text;
			std::string problem;
			if (next.empty() || next == m_cfg.id) {
				m_editName = false;
				m_uiRebuild = true;
				return;
			}
			if (onRename && onRename(m_cfg.id, next, problem)) {
				m_cfg.id = next;
				m_notice.clear();
				m_editName = false;
			} else {
				m_notice = problem; // refused: the field stays open to fix it
			}
			m_uiRebuild = true; // deferred — we are inside a widget callback
		};
	} else {
		// The category prefix, then the id as the RENAME affordance. Deferred
		// rebuild: this fires from inside the tree walk.
		chrome.titleSlot->Add<EditableTitle>(
			gfx::Rect{0, 0, 1, 1},
			loc::Format("map.type.title", m_cfg.categoryLabel, ""), m_cfg.id,
			[this] {
				m_editName = true;
				m_uiRebuild = true;
			});
	}
	// A rebuild (an optional field toggled between checkbox and slider) recreates
	// the tab control; activeTab (captured before Clear) keeps the open tab.
	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(), 0.07f);
	for (const char* section : m_sections) m_tabs->AddTab(loc::Tr(section));
	m_tabs->SetActiveTab(activeTab);

	// One row per field, stacked within its section's page. The widget callbacks
	// capture the spec BY POINTER: the schema is a static table (SchemaFor
	// returns a span over one), so the row outlives every widget it built.
	std::vector<float> nextY(m_sections.size(), kRowY0);
	for (const FieldSpec& spec : m_schema) {
		const FieldSpec* s = &spec;
		const auto it = std::find_if(m_sections.begin(), m_sections.end(),
									 [&](const char* s) {
										 return std::string_view(s) == spec.sectionKey;
									 });
		const size_t tab = static_cast<size_t>(it - m_sections.begin());
		float& y = nextY[tab];
		const std::string label = PrettyFieldName(spec.key);
		const std::string value = ValueOf(spec);
		// How far the cursor moves after this row. A Slider stacks its label
		// OVER its track, so it needs about twice the height of a one-line
		// control — pitching every row the same put each slider's track across
		// the row below it (the overlap audit's finding on the Look tab).
		float rowH = kRowH;

		switch (spec.kind) {
		case FieldKind::Bool: {
			const bool on = value == "1" || value == "true";
			m_tabs->AddChild<ui::Checkbox>(tab, gfx::Rect{kRowX, y, kRowW, kRowH * 0.62f},
										   label, on, [this, s](bool checked) {
											   SetValue(*s, BoolText(checked));
										   });
			break;
		}
		case FieldKind::Float: {
			// A field whose ABSENCE is meaningful (no schema default: the loader
			// falls back to the texture's own map) can't say so on a slider —
			// position 0 would read as an explicit zero. So an unset one shows as
			// a checkbox instead, and a set one gets an "x" to unset it again.
			const bool optional = !*spec.def;
			if (optional && value.empty()) {
				m_tabs->AddChild<ui::Checkbox>(
					tab, gfx::Rect{kRowX, y, kRowW, kRowH * 0.62f},
					label + loc::Tr("map.type.frommap"), true, [this, s](bool on) {
						if (on) return; // already unset
						SetValue(*s, *s->neutral ? s->neutral : "0");
						m_uiRebuild = true; // the row becomes a slider
					});
				break;
			}
			float v = spec.lo;
			std::from_chars(value.data(), value.data() + value.size(), v);
			const float sliderW = optional ? kRowW - 0.06f : kRowW;
			rowH = kSliderRowH; // label over track — see the note above
			m_tabs->AddChild<ui::Slider>(tab,
										 gfx::Rect{kRowX, y, sliderW, kSliderRowH * 0.9f},
										 label, spec.lo, spec.hi, v,
										 [this, s](float f) {
											 // Snap to the field's granularity so the
											 // catalog keeps authored-looking numbers.
											 const float step = s->step > 0.0f ? s->step : 0.001f;
											 const float snapped = std::round(f / step) * step;
											 SetValue(*s, std::format("{:g}", snapped));
										 });
			if (optional)
				m_tabs->AddChild<ui::Button>(
					tab, gfx::Rect{kRowX + sliderW + 0.01f, y, 0.05f, kRowH * 0.55f},
					"x", [this, s] {
						SetValue(*s, ""); // empty = the writer REMOVES the field
						m_uiRebuild = true;
					});
			break;
		}
		case FieldKind::Text: {
			m_tabs->AddChild<ui::Label>(tab, gfx::Rect{kRowX, y, kLabelW, kRowH * 0.55f},
										label);
			auto* field = m_tabs->AddChild<ui::TextField>(
				tab, gfx::Rect{kFieldX, y, kFieldW, kRowH * 0.55f}, value);
			field->maxLength = 64;
			ui::TextField* raw = field;
			field->onChange = [this, raw, s] { SetValue(*s, raw->text); };
			break;
		}
		case FieldKind::TextureSet:
		case FieldKind::Model: {
			// A POOL asset: too many to scroll and nothing to see in a list of
			// names, so the row is a button that opens the asset picker (its
			// grid shows the texture itself, its resolutions and its maps). The
			// button reads as the current value, "(none)" when unset.
			m_tabs->AddChild<ui::Label>(tab, gfx::Rect{kRowX, y, kLabelW, kRowH * 0.55f},
										label);
			const bool textures = spec.kind == FieldKind::TextureSet;
			m_tabs->AddChild<ui::Button>(
				tab, gfx::Rect{kFieldX, y, kFieldW, kRowH * 0.55f},
				value.empty() ? loc::Tr("map.type.none") : value,
				[this, s, textures] {
					// The picker is modal over this dialog; the owner routes it
					// and hands the pick back through onPickAsset's callback.
					if (onPickAsset)
						onPickAsset(textures, ValueOf(*s), [this, s](const std::string& picked) {
							SetValue(*s, picked);
							m_uiRebuild = true; // the button's face is its value
						});
				});
			break;
		}
		case FieldKind::Enum:
		case FieldKind::CatalogRef: {
			// "(none)" is index 0 for everything but a plain Enum, so a field can
			// be left unset (an absent catalog field is meaningful — it means
			// "the loader's default").
			std::vector<std::string> items;
			const bool nullable = spec.kind != FieldKind::Enum;
			if (nullable) items.push_back(loc::Tr("map.type.none"));
			std::vector<std::string> values = spec.kind == FieldKind::Enum
												  ? SplitOptions(spec.options)
												  : (optionsFor ? optionsFor(spec)
																: std::vector<std::string>{});
			// A value the pool no longer offers still has to be selectable, or
			// opening the dialog would silently retype the entry.
			if (!value.empty() &&
				std::find(values.begin(), values.end(), value) == values.end())
				values.push_back(value);
			items.insert(items.end(), values.begin(), values.end());

			int sel = 0;
			for (size_t i = 0; i < values.size(); ++i)
				if (values[i] == value) {
					sel = static_cast<int>(i) + (nullable ? 1 : 0);
					break;
				}
			m_tabs->AddChild<ui::Label>(tab, gfx::Rect{kRowX, y, kLabelW, kRowH * 0.55f},
										label);
			m_tabs->AddChild<ui::DropDown>(
				tab, gfx::Rect{kFieldX, y, kFieldW, kRowH * 0.55f}, items, sel,
				[this, s, values, nullable](int i) {
					const int v = nullable ? i - 1 : i;
					SetValue(*s, v >= 0 && v < static_cast<int>(values.size())
									 ? values[static_cast<size_t>(v)]
									 : std::string());
				});
			break;
		}
		}
		y += rowH;
	}

	// A refusal (a rename collision, a type still in use) or the delete arming
	// note, in a band of its own between the form and the footer — it used to be
	// drawn at a hand-picked y, which is a row nothing else knew about.
	chrome.body->Row<ui::Label>(FormRow(0.9f), m_notice)->accent = true;

	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.save"), [this] {
		// A touched field that invalidates baked geometry tells the owner to
		// re-run AssetBaker (it keeps the dialog up, busy, meanwhile).
		m_cfg.rebake = false;
		for (const FieldSpec& spec : m_schema)
			if (spec.rebakes && Touched(spec.key)) m_cfg.rebake = true;
		if (onSave) onSave(m_cfg);
		if (!m_busy) Close(); // a launched re-bake closes us on completion
	});
	// Duplicate hands off to the create dialog, so this one closes first — the
	// same handoff the extra button makes (copy the config, close, then call:
	// the callback may not touch this dialog's widgets after Close).
	if (!duplicateLabel.empty())
		chrome.footer->Row<ui::Button>(FooterButton(), duplicateLabel, [this] {
			Config cfg = m_cfg;
			Close();
			if (onDuplicate) onDuplicate(cfg);
		});
	if (!extraLabel.empty())
		chrome.footer->Row<ui::Button>(FooterButton(1.2f), extraLabel, [this] {
			Config cfg = m_cfg;
			Close();
			if (onExtra) onExtra(cfg);
		});
	// Delete is two clicks: the first arms it (the label switches to the
	// confirm), so a destructive action never fires on a stray click.
	chrome.footer->Row<ui::Button>(
		FooterButton(), loc::Tr(m_deleteArmed ? "map.type.delete.confirm"
											  : "map.type.delete"),
		[this] {
			if (!m_deleteArmed) {
				m_deleteArmed = true;
				m_notice = loc::Tr("map.type.delete.arm");
				m_uiRebuild = true; // the label changes
				return;
			}
			std::string problem;
			if (onDelete && onDelete(m_cfg.id, problem)) {
				Close();
				return;
			}
			m_deleteArmed = false;
			m_notice = problem; // refused: it says which levels still use it
			m_uiRebuild = true;
		});
	chrome.footer->Space(ui::Len::Fill()); // the "?" sits at the far edge
	chrome.footer->Row<ui::Button>(FooterButton(0.4f), "?",
								   [this] { m_helpOpen = true; });
}

void TypeEditorDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	// One font now: the title text used to be a second Font at the very same
	// size as this context's. GameUI::UpdateFonts commits every library font
	// once per frame, so there is nothing to flush here either.
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	if (m_uiRebuild) { // deferred from a widget callback
		m_uiRebuild = false;
		BuildUI();
	}
	if (m_busy) return; // a re-bake is running — the form is frozen

	// The help overlay owns the input while up: any click or Esc dismisses it.
	if (m_helpOpen) {
		if (input.WasKeyPressed(VK_ESCAPE) ||
			input.WasMousePressed(MouseButton::Left))
			m_helpOpen = false;
		return;
	}
	if (input.WasKeyPressed(VK_ESCAPE)) {
		if (m_editName) { // first Esc only cancels the rename
			m_editName = false;
			m_uiRebuild = true;
			return;
		}
		Close(); // discard: nothing was applied live
		return;
	}

	// (The id's hover and click belong to the EditableTitle widget now — it owns
	// the rect it draws, so no second copy of that geometry lives here.)
	m_ui.Update(input, w, h);

	// Clicking away from the open rename field cancels it (Enter is the commit).
	if (m_editName && m_nameField && !m_nameField->Focused()) {
		m_editName = false;
		m_uiRebuild = true;
	}
}

void TypeEditorDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							  float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	// Title (or the rename field), the form, the notice line and the footer are
	// all widgets in the card's stack now.
	m_ui.Render(batch, w, h);

	// While re-baking, freeze the form behind a notice (the owner runs AssetBaker).
	if (m_busy) {
		batch.DrawRect(panel, {0.0f, 0.0f, 0.0f, 0.55f});
		const std::string msg = loc::Tr("newasset.baking");
		m_ui.GetFont().Draw(batch, msg, panel.x + (panel.w - m_ui.GetFont().MeasureWidth(msg)) * 0.5f,
					panel.y + panel.h * 0.5f - m_ui.GetFont().Height() * 0.5f, th.accent);
		return;
	}

	// The "?" overlay: every field of the ACTIVE tab with its explanation,
	// word-wrapped (the BalanceDialog help pattern).
	if (m_helpOpen) {
		const gfx::Rect help{0.28f * w, 0.16f * h, 0.44f * w, 0.68f * h};
		// The theme panel is translucent, and the form underneath would read
		// through a wall of explanation text — black it out first.
		batch.DrawRect(help, {0.0f, 0.0f, 0.0f, 0.92f});
		batch.DrawRect(help, th.panel);
		ui::DrawBorder(batch, help, th.panelBorder);
		const float pad = help.w * 0.04f;
		const float lineH = m_ui.GetFont().Height() * 1.25f;
		float y = help.y + pad;
		const int active = m_tabs ? m_tabs->ActiveTab() : 0;
		const char* section =
			active >= 0 && active < static_cast<int>(m_sections.size())
				? m_sections[static_cast<size_t>(active)]
				: kSectionIdentity;
		m_ui.GetFont().Draw(batch, loc::Tr(section), help.x + pad, y, th.accent);
		y += lineH * 1.5f;
		for (const FieldSpec& spec : m_schema) {
			if (std::string_view(spec.sectionKey) != std::string_view(section)) continue;
			m_ui.GetFont().Draw(batch, PrettyFieldName(spec.key), help.x + pad, y, th.text);
			y += lineH;
			// Greedy word wrap into the card width.
			std::string line;
			const std::string text = spec.help;
			size_t i = 0;
			while (i <= text.size()) {
				const size_t space = text.find(' ', i);
				const std::string word =
					text.substr(i, space == std::string::npos ? std::string::npos : space - i);
				const std::string tryLine = line.empty() ? word : line + " " + word;
				if (!line.empty() &&
					m_ui.GetFont().MeasureWidth(tryLine) > help.w - pad * 3) {
					m_ui.GetFont().Draw(batch, line, help.x + pad * 2, y, th.textDim);
					y += lineH;
					line = word;
				} else {
					line = tryLine;
				}
				if (space == std::string::npos) break;
				i = space + 1;
			}
			if (!line.empty()) {
				m_ui.GetFont().Draw(batch, line, help.x + pad * 2, y, th.textDim);
				y += lineH;
			}
			y += lineH * 0.35f;
			if (y > help.y + help.h - pad) break; // the card is full
		}
	}
}

} // namespace dungeon::game
