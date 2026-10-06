// ============================================================================
// Game/TypeEditorDialog.cpp — see TypeEditorDialog.h.
// ============================================================================
#include "Game/TypeEditorDialog.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "Game/Style.h" // the WeightedRefs list format
#include "UI/Controls.h"
#include "UI/TreeInspector.h" // widget class names, for BuiltRows

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

// A field row's label column against its control column.
constexpr float kLabelFill = 1.0f, kFieldFill = 1.5f;

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

// What a schema row added to its page, read off the tree (BuiltRow): each new
// child by class, a Stack row as its own children joined by '+', rows by ','.
std::string DescribeAdded(const ui::Widget& page, size_t from) {
	std::string out;
	const auto& rows = page.Children();
	for (size_t i = from; i < rows.size(); ++i) {
		if (!out.empty()) out += ',';
		const ui::Widget& row = *rows[i];
		if (row.Children().empty()) {
			out += ui::inspect::Name(row);
			continue;
		}
		for (size_t c = 0; c < row.Children().size(); ++c)
			out += (c ? "+" : "") + ui::inspect::Name(*row.Children()[c]);
	}
	return out.empty() ? "none" : out;
}

// A derived number to the field's own granularity: a 0.005 step reads "0.060"
// (a relief), a 0.5 step "12.5" (a power). One decimal hid a relief entirely.
std::string DerivedText(const FieldSpec& spec, float value) {
	int places = 1;
	if (spec.step > 0.0f && spec.step < 1.0f)
		places = std::clamp(static_cast<int>(std::ceil(-std::log10(spec.step) - 1e-4f)), 1, 4);
	return std::format("{:.{}f}", value, places);
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
	m_confirming = false;
	m_deleteWhat.clear();
	m_typed.clear();
	m_clashNote.clear();
	// A fresh open starts on the first tab: null the (now stale) control so
	// BuildUI's tab-preservation reads 0, not the previously-closed dialog's tab.
	m_tabs = nullptr;
	m_lastTab = 0;

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
	SetField(spec.key, std::move(value));
}

void TypeEditorDialog::SetField(std::string_view key, std::string value) {
	serialize::Set(m_cfg.fields, std::string(key), std::move(value));
	if (!Touched(key)) m_touched.emplace_back(key);
}

void TypeEditorDialog::BuildStageRows(ui::Stack& page, const FieldSpec& spec) {
	const FieldSpec* s = &spec;
	// Each row addresses its stage by INDEX and re-reads the list on every
	// change: an id is being typed one key at a time, so a captured id would be
	// stale by the second keystroke.
	auto stages = [this, s] { return SplitOptions(ValueOf(*s)); };
	auto join = [](const std::vector<std::string>& v) {
		std::string out;
		for (const std::string& w : v) out += (out.empty() ? "" : " ") + w;
		return out;
	};
	auto textOf = [this](const std::string& id) {
		const std::string* v = serialize::Find(m_cfg.fields, "text_" + id);
		return v ? *v : std::string();
	};
	page.Row<ui::Label>(FormRow(), loc::Tr("map.type.stages.head"))->dim = true;
	const std::vector<std::string> now = stages();
	for (size_t i = 0; i < now.size(); ++i) {
		ui::Stack* row = page.Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.4f;
		// The stage's id: record-safe, since the save and the items name it.
		ui::TextField* id = row->Row<ui::TextField>(ui::Len::Fill(0.6f), now[i]);
		id->maxLength = 24;
		m_stageIds.push_back(id);
		id->onChange = [this, s, i, id, stages, join, textOf] {
			std::erase_if(id->text, [](char ch) {
				const unsigned char u = static_cast<unsigned char>(ch);
				return !(std::isalnum(u) || ch == '_' || ch == '-');
			});
			std::vector<std::string> list = stages();
			if (i >= list.size()) return;
			// Applied only as a NEW id no stage holds. An emptied id waits for its
			// new name rather than dropping the stage (the list is space-split, so
			// an empty id would shift every index). SO DOES AN ID ANOTHER STAGE
			// HOLDS (code-review C100). An id is typed a key at a time, so renaming
			// stage2 to stage10 passes through stage1 - and applying that keystroke
			// moved this stage's line over stage1's, then the next carried it off
			// to stage10 and left stage1 with nothing, which a save then removed.
			// The typed text stays in the field; the stage keeps its last unique
			// id, and its line.
			const bool held = std::find(list.begin(), list.end(), id->text) != list.end();
			if (!id->text.empty() && !held) {
				// The line moves with the stage.
				const std::string text = textOf(list[i]);
				SetField("text_" + list[i], std::string());
				list[i] = id->text;
				if (!text.empty()) SetField("text_" + list[i], text);
				SetValue(*s, join(list));
			}
			// Applied or not, the notice says what the fields now show - an id
			// typed back to its own, or emptied, takes a "taken" notice down too.
			RefreshStageClash(list, i);
		};
		// What the log says on reaching it (`text_<id>`).
		ui::TextField* line = row->Row<ui::TextField>(ui::Len::Fill(1.4f), textOf(now[i]));
		line->maxLength = 96;
		line->placeholder = loc::Tr("map.type.stages.hint");
		line->onChange = [this, i, line, stages] {
			const std::vector<std::string> list = stages();
			if (i < list.size()) SetField("text_" + list[i], line->text);
		};
		RowIcon(*row, m_device, "clear", loc::Tr("map.type.stages.remove"),
				[this, s, i, stages, join] {
					std::vector<std::string> list = stages();
					if (i >= list.size()) return;
					SetField("text_" + list[i], std::string()); // the writer removes it
					list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
					SetValue(*s, join(list));
					m_uiRebuild = true; // deferred: inside a callback
				});
	}
	auto add = [this, s, stages, join] {
		std::vector<std::string> list = stages();
		// A fresh id nothing else uses: stage<N>.
		std::string fresh;
		for (int n = static_cast<int>(list.size()) + 1;; ++n) {
			fresh = "stage" + std::to_string(n);
			if (std::find(list.begin(), list.end(), fresh) == list.end()) break;
		}
		list.push_back(fresh);
		SetValue(*s, join(list));
		m_uiRebuild = true;
	};
	m_stageAdd = page.Row<ui::Button>(FormRow(), loc::Tr("map.type.stages.add"), add);
}

void TypeEditorDialog::RefreshStageClash(const std::vector<std::string>& list,
										 size_t typed) {
	// A row CLASHES while its field shows an id another stage holds: not empty,
	// not its own stage's id, yet in the list. DERIVED from every row each time
	// rather than latched by the keystroke that raised it (code-review C100's
	// follow-up): a latch outlived the field it described - typed back to its
	// own id, emptied, or rebuilt - and then took down notices it never put up.
	const auto clashes = [&](size_t j) {
		if (j >= m_stageIds.size() || j >= list.size()) return false;
		const std::string& t = m_stageIds[j]->text;
		return !t.empty() && t != list[j] &&
			   std::find(list.begin(), list.end(), t) != list.end();
	};
	size_t row = clashes(typed) ? typed : list.size();
	for (size_t j = 0; row == list.size() && j < list.size(); ++j)
		if (clashes(j)) row = j;
	if (row < list.size()) {
		// The row just typed always speaks (the newest event takes the line);
		// another row still clashing only keeps a line that was already the
		// clash's, never one another owner has put up since.
		if (row == typed || m_notice == m_clashNote) {
			m_clashNote = loc::Format("map.type.stages.taken", m_stageIds[row]->text);
			SetNoteInPlace(m_clashNote);
		} else {
			m_clashNote.clear();
		}
		return;
	}
	// Nothing clashes: the notice comes down only if it is still the clash's.
	if (!m_clashNote.empty() && m_notice == m_clashNote) SetNoteInPlace(std::string());
	m_clashNote.clear();
}

bool TypeEditorDialog::ClickAddStage() {
	if (!m_stageAdd || !m_stageAdd->onClick) return false;
	m_stageAdd->onClick();
	return true;
}

void TypeEditorDialog::BuildWeightedRows(ui::Stack& page, const FieldSpec& spec) {
	const FieldSpec* s = &spec;
	// Re-read on every change, by index - the stage rows' rule.
	auto list = [this, s] { return style::ParseMonsters(ValueOf(*s)); };
	auto write = [this, s](const std::vector<style::Pick>& picks) {
		SetValue(*s, style::FormatMonsters(picks));
	};
	// The candidates, each named the way faceFor names it (a monster with its
	// power); an id the catalog has lost stays listed by its own name.
	std::vector<std::string> ids = optionsFor ? optionsFor(spec) : std::vector<std::string>{};
	const std::vector<style::Pick> now = list();
	for (const style::Pick& p : now)
		if (std::find(ids.begin(), ids.end(), p.id) == ids.end()) ids.push_back(p.id);
	std::vector<std::string> names;
	for (const std::string& id : ids) {
		std::string label = faceFor ? faceFor(spec, id).label : std::string();
		names.push_back(label.empty() ? id : label);
	}
	page.Row<ui::Label>(FormRow(), loc::Tr("map.type.weighted.head"))->dim = true;
	for (size_t i = 0; i < now.size(); ++i) {
		ui::Stack* row = page.Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.4f;
		const int sel = static_cast<int>(std::find(ids.begin(), ids.end(), now[i].id) - ids.begin());
		row->Row<ui::DropDown>(ui::Len::Fill(1.6f), names, sel, [list, write, ids, i](int pick) {
			std::vector<style::Pick> picks = list();
			if (i >= picks.size() || pick < 0 || pick >= static_cast<int>(ids.size())) return;
			picks[i].id = ids[static_cast<size_t>(pick)];
			write(picks);
		});
		ui::TextField* weight =
			row->Row<ui::TextField>(ui::Len::Fill(0.4f), std::format("{:g}", now[i].weight));
		weight->maxLength = 6;
		weight->onChange = [weight, list, write, i] {
			std::erase_if(weight->text, [](char ch) {
				return !(std::isdigit(static_cast<unsigned char>(ch)) || ch == '.');
			});
			float w = 0.0f;
			const auto [end, ec] = std::from_chars(weight->text.data(),
												   weight->text.data() + weight->text.size(), w);
			// A weight being typed ("0.", "") waits; only a usable one is kept.
			if (ec != std::errc{} || w <= 0.0f) return;
			std::vector<style::Pick> picks = list();
			if (i >= picks.size()) return;
			picks[i].weight = w;
			write(picks);
		};
		RowIcon(*row, m_device, "clear", loc::Tr("map.type.weighted.remove"),
				[this, list, write, i] {
					std::vector<style::Pick> picks = list();
					if (i >= picks.size()) return;
					picks.erase(picks.begin() + static_cast<std::ptrdiff_t>(i));
					write(picks);
					m_uiRebuild = true; // deferred: inside a callback
				});
	}
	page.Row<ui::Button>(FormRow(), loc::Tr("map.type.weighted.add"), [this, list, write, ids] {
		std::vector<style::Pick> picks = list();
		// The first candidate not already listed.
		for (const std::string& id : ids)
			if (std::none_of(picks.begin(), picks.end(),
							 [&](const style::Pick& p) { return p.id == id; })) {
				picks.push_back({id, 1.0f});
				write(picks);
				m_uiRebuild = true;
				return;
			}
	});
}

void TypeEditorDialog::BuildUI() {
	// Read the open tab BEFORE Clear frees the control (a rebuild keeps the tab,
	// and so does a trip through the confirmation, which has no tabs).
	const int activeTab = m_tabs ? m_tabs->ActiveTab() : m_lastTab;
	m_lastTab = activeTab;
	m_ui.Clear();
	m_tabs = nullptr; // the Clear just freed it; don't read it again below
	m_nameField = nullptr;
	m_noticeLabel = nullptr;
	m_deleteBtn = nullptr;
	m_stageIds.clear();
	m_stageAdd = nullptr;
	m_built.clear();
	// A rebuild puts every stage's id field back to its stage's id, so nothing
	// typed clashes any more: a "taken" notice comes down with the fields it
	// described. One another owner has put up since stays.
	if (!m_clashNote.empty() && m_notice == m_clashNote) m_notice.clear();
	m_clashNote.clear();
	if (m_confirming) {
		BuildConfirm();
		return;
	}
	// An id the code owns is shown, not offered for renaming: the chrome draws it
	// as an ordinary title and the slot stays empty.
	DialogChrome chrome = BuildDialogChrome(
		m_ui, kPanel,
		fixedIdentity ? loc::Format("map.type.title", m_cfg.categoryLabel, m_cfg.id) : "",
		m_closeIcon, [this] { Close(); });
	if (m_editName && !fixedIdentity) {
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
	} else if (!fixedIdentity) {
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

	// One row per field, stacked within its section's page — a content-sized
	// stack per tab (Game/DialogLayout.h TabStack), so a section is as long as
	// its schema and the page scrolls. It used to step a y cursor by a guessed
	// pitch, which every kind of row then had to agree with; a Slider needs two
	// lines and laid its track across the row below.
	std::vector<ui::Stack*> pages;
	for (size_t i = 0; i < m_sections.size(); ++i)
		pages.push_back(TabStack(*m_tabs, i));

	// The widget callbacks capture the spec BY POINTER: the schema is a static
	// table (SchemaFor returns a span over one), so the row outlives every
	// widget it built.
	for (const FieldSpec& spec : m_schema) {
		const FieldSpec* s = &spec;
		const auto it = std::find_if(m_sections.begin(), m_sections.end(),
									 [&](const char* s) {
										 return std::string_view(s) == spec.sectionKey;
									 });
		ui::Stack& page = *pages[static_cast<size_t>(it - m_sections.begin())];
		const std::string label = PrettyFieldName(spec.key);
		const std::string value = ValueOf(spec);
		// A labelled control: the name on the left, the control on the right.
		auto labelled = [&](ui::Len rowLen) -> ui::Stack* {
			ui::Stack* row = page.Row<ui::Stack>(rowLen, true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill), label)->centerV = true;
			return row;
		};
		// Where this row's widgets start on its page, for BuiltRows.
		const size_t pageRowsBefore = page.Children().size();

		// EVERY FieldKind, and no `default:` - the Game lib builds with C4062 as
		// an error (src/Game/CMakeLists.txt), so a new kind fails the build here
		// instead of building no widget (code-review C101).
		switch (spec.kind) {
		case FieldKind::Bool: {
			const bool on = value == "1" || value == "true";
			page.Row<ui::Checkbox>(FormRow(), label, on, [this, s](bool checked) {
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
			// A DERIVED field (derivedFor answers) names the value the game uses
			// in its place, unset or overridden alike.
			const std::optional<float> derived = DerivedValue(spec);
			const std::string shown =
				derived ? label + " " + loc::Format("map.type.derived", DerivedText(spec, *derived))
						: label;
			if (optional && value.empty()) {
				page.Row<ui::Checkbox>(FormRow(), derived ? shown : label + loc::Tr("map.type.frommap"),
									   true, [this, s, derived](bool on) {
										   if (on) return; // already unset
										   // An override starts where the derived value is.
										   const float step = s->step > 0.0f ? s->step : 0.001f;
										   SetValue(*s, derived ? std::format("{:g}",
																			   std::max(s->lo, std::round(*derived / step) * step))
																: *s->neutral ? s->neutral : "0");
										   m_uiRebuild = true; // becomes a slider
									   });
				break;
			}
			float v = spec.lo;
			std::from_chars(value.data(), value.data() + value.size(), v);
			// A Slider stacks its label OVER its track, so it asks for two lines.
			ui::Stack* row = page.Row<ui::Stack>(FormRow(1.9f), true);
			row->gapRem = 0.4f;
			row->Row<ui::Slider>(ui::Len::Fill(), shown, spec.lo, spec.hi, v,
								 [this, s](float f) {
									 // Snap to the field's granularity so the
									 // catalog keeps authored-looking numbers.
									 const float step = s->step > 0.0f ? s->step : 0.001f;
									 const float snapped = std::round(f / step) * step;
									 SetValue(*s, std::format("{:g}", snapped));
								 });
			if (optional)
				RowIcon(*row, m_device, "clear", loc::Tr("map.btn.clear"), [this, s] {
					SetValue(*s, ""); // empty = the writer REMOVES the field
					m_uiRebuild = true;
				});
			break;
		}
		case FieldKind::Text: {
			auto* field =
				labelled(FormRow())->Row<ui::TextField>(ui::Len::Fill(kFieldFill), value);
			field->maxLength = spec.maxLen > 0 ? spec.maxLen : 64;
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
			const bool textures = spec.kind == FieldKind::TextureSet;
			labelled(FormRow())->Row<ui::Button>(
				ui::Len::Fill(kFieldFill),
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
		case FieldKind::DamageType:
		case FieldKind::CatalogRef: {
			// "(none)" is index 0 for a CatalogRef alone, so a reference can be
			// left unset (an absent catalog field is meaningful - it means "the
			// loader's default"). An Enum and a damage type always name one of
			// their values: every blow and every DoT is resisted as SOME type.
			std::vector<std::string> items;
			const bool nullable = spec.kind == FieldKind::CatalogRef;
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
			labelled(FormRow())->Row<ui::DropDown>(
				ui::Len::Fill(kFieldFill), items, sel,
				[this, s, values, nullable](int i) {
					const int v = nullable ? i - 1 : i;
					SetValue(*s, v >= 0 && v < static_cast<int>(values.size())
									 ? values[static_cast<size_t>(v)]
									 : std::string());
				});
			break;
		}
		case FieldKind::QuestStages:
			BuildStageRows(page, spec);
			break;
		case FieldKind::WeightedRefs:
			BuildWeightedRows(page, spec);
			break;
		case FieldKind::CatalogRefPick: {
			// ONE id, picked from a list that shows every candidate the way the
			// palette does (faceFor: a surface type's name and swatch; else the
			// bare id) - a dropdown would hide exactly the swatches you choose
			// by. "(none)" leads, so the field can be left unset. Ticking a row
			// takes its id and rebuilds the page so the others clear; unticking
			// the held one clears the field. An id the catalog no longer has
			// still lists, ticked, rather than being dropped behind the
			// author's back.
			std::vector<std::string> offered =
				optionsFor ? optionsFor(spec) : std::vector<std::string>{};
			if (!value.empty() && std::find(offered.begin(), offered.end(), value) == offered.end())
				offered.push_back(value);
			page.Row<ui::Label>(FormRow(), label)->accent = true;
			page.Row<ui::Checkbox>(FormRow(), loc::Tr("map.type.none"), value.empty(),
								   [this, s](bool) {
									   SetValue(*s, std::string()); // the writer REMOVES it
									   m_uiRebuild = true; // deferred: inside a callback
								   });
			for (const std::string& id : offered) {
				RefFace face = faceFor ? faceFor(*s, id) : RefFace{};
				if (face.label.empty()) face.label = id;
				ui::Checkbox* row = page.Row<ui::Checkbox>(
					FormRow(), face.label, id == value, [this, s, id](bool checked) {
						SetValue(*s, checked ? id : std::string());
						m_uiRebuild = true;
					});
				// The swatch is the ROW'S QUESTION, asked each time it draws -
				// never an answer kept from now (code-review C235; swatchFor).
				if (swatchFor && !swatchFor(*s, id).Empty())
					row->swatch = [this, s, id] { return swatchFor(*s, id); };
			}
			break;
		}
		}
		m_built.push_back({s, DescribeAdded(page, pageRowsBefore)});
	}

	// A refusal (a rename collision, a type still in use) or the delete arming
	// note, in a band of its own between the form and the footer — it used to be
	// drawn at a hand-picked y, which is a row nothing else knew about.
	m_noticeLabel = chrome.body->Row<ui::Label>(FormRow(0.9f), m_notice);
	m_noticeLabel->accent = true;

	// The footer's actions are icon discs named by their tooltips (FooterIcon).
	FooterIcon(*chrome.footer, m_device, "save", loc::Tr("map.cfg.save"),
			   [this] { ClickSave(); });
	// Duplicate hands off to the create dialog, so this one closes first — the
	// same handoff the extra button makes (copy the config, close, then call:
	// the callback may not touch this dialog's widgets after Close).
	if (!duplicateLabel.empty())
		FooterIcon(*chrome.footer, m_device, "duplicate", duplicateLabel, [this] {
			Config cfg = m_cfg;
			Close();
			if (onDuplicate) onDuplicate(cfg);
		});
	if (!extraLabel.empty())
		FooterIcon(*chrome.footer, m_device, extraIcon.c_str(), extraLabel, [this] {
			Config cfg = m_cfg;
			Close();
			if (onExtra) onExtra(cfg);
		});
	// Armed, the disc lights and its name becomes the confirmation (the notice
	// row above says what a second click does). None for an id the code owns.
	if (!fixedIdentity)
		FooterIcon(*chrome.footer, m_device, "delete",
				   loc::Tr(m_deleteArmed ? "map.type.delete.confirm" : "map.type.delete"),
				   [this] { ClickDelete(); })
			->active = m_deleteArmed;
	chrome.footer->Space(ui::Len::Fill()); // help sits at the far edge
	FooterIcon(*chrome.footer, m_device, "help", loc::Tr("map.btn.help"),
			   [this] { m_helpOpen = true; });
}

bool TypeEditorDialog::ClickSave() {
	// A touched field that invalidates baked geometry tells the owner to
	// re-run AssetBaker (it keeps the dialog up, busy, meanwhile).
	m_cfg.rebake = false;
	for (const FieldSpec& spec : m_schema)
		if (spec.rebakes && Touched(spec.key)) m_cfg.rebake = true;
	if (onSave) {
		if (std::string refused = onSave(m_cfg); !refused.empty()) {
			// Nothing was written: the form stays up saying why, to be fixed.
			m_notice = std::move(refused);
			m_uiRebuild = true; // deferred - this fires inside the tree walk
			return false;
		}
	}
	if (!m_busy) Close(); // a launched re-bake closes us on completion
	return true;
}

bool TypeEditorDialog::PickAsset(std::string_view key, const std::string& value) {
	for (const FieldSpec& spec : m_schema)
		if (std::string_view(spec.key) == key &&
			(spec.kind == FieldKind::TextureSet || spec.kind == FieldKind::Model)) {
			SetValue(spec, value);
			m_uiRebuild = true; // the button's face is its value
			return true;
		}
	return false;
}

void TypeEditorDialog::SelectTab(int tab) {
	if (m_tabs) m_tabs->SetActiveTab(tab);
}

bool TypeEditorDialog::TypeStageId(size_t stage, const std::string& text) {
	if (stage >= m_stageIds.size()) return false;
	ui::TextField* field = m_stageIds[stage];
	field->text = text;
	if (field->onChange) field->onChange();
	return true;
}

// --- deleting ----------------------------------------------------------------

void TypeEditorDialog::ClickDelete() {
	if (fixedIdentity) {
		// No button offers this; the console's step can still ask. The owner's
		// refusal would say the same, but arming first would promise a delete.
		m_notice = loc::Tr("map.type.classbacked");
		m_uiRebuild = true;
		return;
	}
	if (typedDelete) {
		// ASKED BEFORE THE CONFIRMATION OPENS (the Worlds dialog's rule): a
		// refusal after you have typed the id out wastes a deliberate act.
		const std::string why = canDelete ? canDelete(m_cfg.id) : std::string();
		if (!why.empty()) {
			m_notice = why;
			m_uiRebuild = true;
			return;
		}
		m_confirming = true;
		m_deleteWhat = onDescribe ? onDescribe(m_cfg.id) : std::vector<std::string>{};
		// Said in the LOG, because a sweep of this view has to be able to show
		// it audited the confirmation and not the form before the click.
		log::Info("type editor: confirming the delete of {} '{}'", m_cfg.catalogKey,
				  m_cfg.id);
		m_typed.clear();
		m_notice = loc::Tr("map.worlds.delete.casenote");
		m_uiRebuild = true; // deferred — this fires inside the tree walk
		return;
	}
	// Otherwise two clicks: the first arms it (the label switches to the
	// confirm), so a destructive action never fires on a stray click.
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
}

void TypeEditorDialog::ConfirmDelete(const std::string& typed) {
	if (!m_confirming) return;
	m_typed = typed;
	if (m_deleteBtn) m_deleteBtn->enabled = m_typed == m_cfg.id;
	// EXACT, CASE AND ALL — the same rule, from the same builder, as a world.
	if (m_typed != m_cfg.id) {
		SetNoteInPlace(loc::Tr("map.worlds.delete.mismatch"));
		return;
	}
	std::string problem;
	if (onDelete && onDelete(m_cfg.id, problem)) {
		Close(); // the entry is gone; there is nothing left to edit
		return;
	}
	// Refused at the last moment (the world changed under the confirmation) or
	// failed part-way: back to the form, saying which.
	m_confirming = false;
	m_notice = problem;
	m_uiRebuild = true;
}

void TypeEditorDialog::ApplyPending() {
	if (!m_uiRebuild) return;
	m_uiRebuild = false;
	BuildUI();
}

void TypeEditorDialog::LeaveConfirm() {
	m_confirming = false;
	m_typed.clear();
	m_notice.clear();
	m_uiRebuild = true;
}

void TypeEditorDialog::SetNoteInPlace(std::string text) {
	m_notice = std::move(text);
	if (m_noticeLabel) m_noticeLabel->text = m_notice;
}

void TypeEditorDialog::BuildConfirm() {
	// The title says what is being asked; the id is not a rename affordance
	// here — renaming the thing you are deleting is not a question this view asks.
	DialogChrome chrome = BuildDialogChrome(
		m_ui, kPanel, loc::Format("map.worlds.delete.head", m_cfg.id), m_closeIcon,
		[this] { Close(); }, /*withFooter*/ false);
	// The world delete's wording, shared on purpose: the undo line, the
	// prompt and the case note say nothing world-specific.
	m_deleteBtn = BuildTypedConfirm(
		*chrome.body, m_deleteWhat,
		{loc::Tr("map.worlds.delete.undo"),
		 loc::Format("map.worlds.delete.type", m_cfg.id),
		 loc::Tr("map.worlds.cancel"), typedDeleteLabel},
		m_cfg.id, m_typed,
		[this] { SetNoteInPlace(loc::Tr("map.worlds.delete.casenote")); },
		[this] { LeaveConfirm(); }, [this] { ConfirmDelete(m_typed); });
	ui::Label* note = chrome.body->Row<ui::Label>(FormRow(0.9f), m_notice);
	note->dim = true;
	m_noticeLabel = note;
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
	// An open list or colour picker takes the Esc first and closes itself in
	// the walk below (code-review C81).
	if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.PopupOpen()) {
		if (m_confirming) { // Esc is "no": back to the form, not out of the dialog
			LeaveConfirm();
			return;
		}
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
