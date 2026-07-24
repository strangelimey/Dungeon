// ============================================================================
// Game/TypeEditorDialog.cpp — see TypeEditorDialog.h.
// ============================================================================
#include "Game/TypeEditorDialog.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {
// Panel geometry as window fractions — the BalanceDialog card, a little taller
// (a schema can run to a dozen rows; the tab pages scroll past that).
constexpr gfx::Rect kPanel{0.26f, 0.12f, 0.48f, 0.76f};
constexpr gfx::Rect kTitle{0.28f, 0.145f, 0.40f, 0.04f};
constexpr gfx::Rect kTabs{0.28f, 0.195f, 0.44f, 0.60f};
constexpr gfx::Rect kSave{0.28f, 0.815f, 0.09f, 0.045f};
constexpr gfx::Rect kExtra{0.39f, 0.815f, 0.15f, 0.045f};
constexpr gfx::Rect kHelp{0.615f, 0.815f, 0.035f, 0.045f};
constexpr gfx::Rect kClose{0.66f, 0.815f, 0.09f, 0.045f};

// Row metrics inside a tab page (fractions of the page). A row past 1.0 is what
// makes the page scroll, so a long section simply runs on.
constexpr float kRowX = 0.04f, kRowW = 0.92f;
constexpr float kRowY0 = 0.03f, kRowH = 0.115f;
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

TypeEditorDialog::TypeEditorDialog(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

void TypeEditorDialog::Open(Config cfg, std::span<const FieldSpec> schema) {
	m_open = true;
	m_busy = false;
	m_helpOpen = false;
	m_uiRebuild = false;
	m_cfg = std::move(cfg);
	m_cfg.rebake = false;
	m_schema = schema;
	m_touched.clear();

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
	m_ui.Clear();
	m_tabs = m_ui.Add<ui::TabControl>(kTabs, 0.07f);
	for (const char* section : m_sections) m_tabs->AddTab(loc::Tr(section));

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
			float v = spec.lo;
			std::from_chars(value.data(), value.data() + value.size(), v);
			m_tabs->AddChild<ui::Slider>(tab, gfx::Rect{kRowX, y, kRowW, kRowH * 0.92f},
										 label, spec.lo, spec.hi, v,
										 [this, s](float f) {
											 // Snap to the field's granularity so the
											 // catalog keeps authored-looking numbers.
											 const float step = s->step > 0.0f ? s->step : 0.001f;
											 const float snapped = std::round(f / step) * step;
											 SetValue(*s, std::format("{:g}", snapped));
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
		case FieldKind::Enum:
		case FieldKind::TextureSet:
		case FieldKind::Model:
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
		y += kRowH;
	}

	m_ui.Add<ui::Button>(kSave, loc::Tr("map.cfg.save"), [this] {
		// A touched field that invalidates baked geometry tells the owner to
		// re-run AssetBaker (it keeps the dialog up, busy, meanwhile).
		m_cfg.rebake = false;
		for (const FieldSpec& spec : m_schema)
			if (spec.rebakes && Touched(spec.key)) m_cfg.rebake = true;
		if (onSave) onSave(m_cfg);
		if (!m_busy) Close(); // a launched re-bake closes us on completion
	});
	if (!extraLabel.empty())
		m_ui.Add<ui::Button>(kExtra, extraLabel, [this] {
			Config cfg = m_cfg;
			Close();
			if (onExtra) onExtra(cfg);
		});
	m_ui.Add<ui::Button>(kHelp, "?", [this] { m_helpOpen = true; });
	m_ui.Add<ui::Button>(kClose, loc::Tr("map.cfg.close"), [this] { Close(); });
}

void TypeEditorDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

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
	if (input.WasKeyPressed(VK_ESCAPE)) { // discard: nothing was applied live
		Close();
		return;
	}
	m_ui.Update(input, w, h);
}

void TypeEditorDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							  float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	m_font.Draw(batch, loc::Format("map.type.title", m_cfg.categoryLabel, m_cfg.id),
				kTitle.x * w, kTitle.y * h, th.text);
	m_ui.Render(batch, w, h);

	// While re-baking, freeze the form behind a notice (the owner runs AssetBaker).
	if (m_busy) {
		batch.DrawRect(panel, {0.0f, 0.0f, 0.0f, 0.55f});
		const std::string msg = loc::Tr("newasset.baking");
		m_font.Draw(batch, msg, panel.x + (panel.w - m_font.MeasureWidth(msg)) * 0.5f,
					panel.y + panel.h * 0.5f - m_font.Height() * 0.5f, th.accent);
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
		const float lineH = m_font.Height() * 1.25f;
		float y = help.y + pad;
		const int active = m_tabs ? m_tabs->ActiveTab() : 0;
		const char* section =
			active >= 0 && active < static_cast<int>(m_sections.size())
				? m_sections[static_cast<size_t>(active)]
				: kSectionIdentity;
		m_font.Draw(batch, loc::Tr(section), help.x + pad, y, th.accent);
		y += lineH * 1.5f;
		for (const FieldSpec& spec : m_schema) {
			if (std::string_view(spec.sectionKey) != std::string_view(section)) continue;
			m_font.Draw(batch, PrettyFieldName(spec.key), help.x + pad, y, th.text);
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
					m_font.MeasureWidth(tryLine) > help.w - pad * 3) {
					m_font.Draw(batch, line, help.x + pad * 2, y, th.textDim);
					y += lineH;
					line = word;
				} else {
					line = tryLine;
				}
				if (space == std::string::npos) break;
				i = space + 1;
			}
			if (!line.empty()) {
				m_font.Draw(batch, line, help.x + pad * 2, y, th.textDim);
				y += lineH;
			}
			y += lineH * 0.35f;
			if (y > help.y + help.h - pad) break; // the card is full
		}
	}
}

} // namespace dungeon::game
