// ============================================================================
// Game/WorldSettingsDialog.cpp — see WorldSettingsDialog.h.
// ============================================================================
#include "Game/WorldSettingsDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <memory>

namespace dungeon::game {

namespace {
// Wider than the level dialog's card and shorter than the balance one's: the
// Areas tab is a SEVEN-column table, and a table narrower than its columns is
// not a table. (The panel is the only rect authored here; everything inside it
// is stacked — Game/DialogLayout.h.)
constexpr gfx::Rect kPanel{0.14f, 0.06f, 0.72f, 0.88f};

// A form row's label column against its value column (the level dialog's).
constexpr float kLabelFill = 1.4f, kFieldFill = 1.0f;
// The Areas table, column by column. ONE list, so the header and the rows are
// laid out from the same numbers rather than from two sets that have to agree.
constexpr float kAreaIdFill = 1.8f, kAreaNumFill = 0.85f;
constexpr float kRowBtn = 0.34f; // an ^ / v / x button, in FooterButton widths

// A numeric text field that writes back every PARSEABLE state (the live-apply
// pattern the Balance and Level dialogs use; an in-progress "", "-" or "0."
// just waits). Mono and at the document size: these are table columns, read
// down, and a proportional face gives every digit a different width.
ui::TextField* AddNumericField(ui::Stack& row, ui::Len len, float value,
							   std::function<void(float)> commit) {
	auto* field = row.Row<ui::TextField>(len, std::format("{:g}", value));
	field->fontRole = ui::FontRole::Mono;
	field->fontScale = 1.0f;
	field->maxLength = 8;
	ui::TextField* raw = field;
	field->onChange = [raw, commit = std::move(commit)] {
		const std::string& t = raw->text;
		float v = 0.0f;
		const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
		if (ec == std::errc() && p == t.data() + t.size()) commit(v);
	};
	return field;
}

ui::TextField* AddIntField(ui::Stack& row, ui::Len len, int value,
						   std::function<void(int)> commit) {
	return AddNumericField(
		row, len, static_cast<float>(value),
		[commit = std::move(commit)](float v) { commit(static_cast<int>(v)); });
}

// The index of `value` in `items`, or 0 when it is not there. A field naming
// something since deleted still has to show a row; the checker is what reports
// the dangling name, and quietly rewriting it here would hide that.
int IndexOf(const std::vector<std::string>& items, const std::string& value) {
	for (size_t i = 0; i < items.size(); ++i)
		if (items[i] == value) return static_cast<int>(i);
	return 0;
}

// Records are whitespace-tokenised, so an id may carry none — the DoorInspector
// name-filter idiom, applied as the field is typed in.
void FilterId(std::string& text) {
	std::erase_if(text, [](char ch) {
		const unsigned char u = static_cast<unsigned char>(ch);
		return !(std::isalnum(u) || ch == '_' || ch == '-');
	});
}

// A dim caption under a control: what a value means, or why the last edit was
// refused. At the DOCUMENT size rather than the dialog's, which is what makes
// it fine print — and what makes it FIT: a Label does not wrap, and a sentence
// set at kDialogTextScale runs past the panel and over whatever is beside it.
// (Anything that needs more than a line belongs in the "?" instead.)
ui::Label* AddNote(ui::Stack& body, const std::string& text) {
	ui::Label* note = body.Row<ui::Label>(ui::Len::Fixed(2.0f), text);
	note->dim = true;
	note->centerV = true;
	note->fontScale = 1.1f;
	return note;
}

// A section heading inside a tab.
void AddHeading(ui::Stack& body, std::string text) {
	body.Row<ui::Label>(FormRow(), std::move(text))->centerV = true;
}
} // namespace

WorldSettingsDialog::WorldSettingsDialog(gfx::GraphicsDevice& device,
										 ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	// The whole form reads at the dialog text size, set on the ROOT because
	// fontScale inherits; the numeric columns pin themselves back to the
	// document size in AddNumericField.
	m_ui.Root().fontScale = ui::kDialogTextScale;
	m_closeIcon = CloseIcon(device);
}

void WorldSettingsDialog::Open(const WorldMap* world,
							   std::vector<DungeonInfo> dungeons,
							   std::vector<std::string> levels, Manifest manifest,
							   const std::string& selectLocation) {
	m_open = true;
	m_world = world;
	m_dungeons = std::move(dungeons);
	m_levels = std::move(levels);
	m_manifest = std::move(manifest);
	m_selected = selectLocation;
	m_note.clear();
	m_helpOpen = false;
	m_uiRebuild = false;
	// A right-click on a doorway opens the dialog ON that doorway; anything
	// else opens where the dialog is about the world as a whole.
	m_activeTab = selectLocation.empty() ? 0 : 2;
	BuildUI();
}

const std::vector<std::string>& WorldSettingsDialog::LevelsOf(
	const std::string& dungeonId) const {
	for (const DungeonInfo& d : m_dungeons)
		if (d.id == dungeonId) return d.levels;
	return m_levels;
}

const WorldMap::Location* WorldSettingsDialog::Selected() const {
	if (!m_world || m_selected.empty()) return nullptr;
	for (const WorldMap::Location& l : m_world->Locations())
		if (l.id == m_selected) return &l;
	return nullptr;
}

std::string WorldSettingsDialog::StartNote() const {
	if (!m_world) return {};
	return loc::Format("map.world.start.on",
					   m_world->TerrainAt(m_world->StartX(), m_world->StartZ()).id);
}

void WorldSettingsDialog::SetNote(std::string text) {
	// WRITTEN INTO THE LABEL, NOT REBUILT. A rebuild destroys the field being
	// typed in and takes the focus with it, and the note is the ONLY part of
	// the form an edit changes — so rebuilding to show it made a two-digit
	// number impossible to type. Worse, the digits of one are judged
	// separately: typing 12 offers 1 first, and if 1 is water the refusal
	// bounced the field back to the old value before the 2 arrived. Nothing
	// mid-edit is corrected now; the note says why the world has not moved,
	// and the next parseable value that is allowed moves it.
	m_note = std::move(text);
	if (m_noteLabel) m_noteLabel->text = m_note;
}

void WorldSettingsDialog::BuildUI() {
	m_ui.Clear();
	m_noteLabel = nullptr; // dies with the tree; each tab re-seeds it
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, loc::Tr("map.world.title"),
											m_closeIcon, [this] { Close(); });

	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(), 0.075f);
	const size_t tabWorld = m_tabs->AddTab(loc::Tr("map.world.tab.world"));
	const size_t tabAreas = m_tabs->AddTab(loc::Tr("map.world.tab.areas"));
	const size_t tabDoors = m_tabs->AddTab(loc::Tr("map.world.tab.doorways"));
	BuildWorldTab(tabWorld);
	BuildAreasTab(tabAreas);
	BuildLocationsTab(tabDoors);
	m_tabs->SetActiveTab(m_activeTab);

	// "?" on the left, Save on the right: the explainer is not an action on the
	// world, and should not stand in the row of things that are.
	chrome.footer->Row<ui::Button>(FooterButton(0.4f), "?",
								   [this] { m_helpOpen = true; });
	chrome.footer->Space(ui::Len::Fill());
	chrome.footer->Row<ui::Button>(FooterButton(1.2f), loc::Tr("map.cfg.save"),
								   [this] {
									   if (onSave) onSave();
									   Close();
								   });
}

// ---------------------------------------------------------------------------
// The World tab: the world's start, the game's opening, the harness level.
// ---------------------------------------------------------------------------
void WorldSettingsDialog::BuildWorldTab(size_t tab) {
	ui::Stack* rows = TabStack(*m_tabs, tab);
	if (!m_world) return;

	// Seeded, not carried over: a note is about the edit that raised it, and
	// the tab it was raised on. Switching tabs is a fresh start.
	if (m_note.empty()) m_note = StartNote();
	AddHeading(*rows, loc::Tr("map.world.start.head"));
	{
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr("map.world.start"))
			->centerV = true;
		// X and Z commit TOGETHER, through one call, because the refusal is
		// about the CELL: an x that is only valid once z catches up must not be
		// judged on x alone.
		auto setStart = [this](int x, int z) {
			if (!onSetStart) return;
			if (onSetStart(x, z)) SetNote(StartNote());
			else if (!m_world->InBounds(x, z))
				SetNote(loc::Tr("map.world.start.offgrid"));
			else
				SetNote(loc::Format("map.world.start.impassable",
									m_world->TerrainAt(x, z).id));
		};
		AddIntField(*row, ui::Len::Fill(kFieldFill), m_world->StartX(),
					[this, setStart](int v) { setStart(v, m_world->StartZ()); });
		AddIntField(*row, ui::Len::Fill(kFieldFill), m_world->StartZ(),
					[this, setStart](int v) { setStart(m_world->StartX(), v); });
		row->Space(ui::Len::Fill(0.6f));
	}
	// THE STATUS NOTE — the one row an edit rewrites, held by pointer so it can
	// be rewritten without a rebuild (see SetNote).
	m_noteLabel = AddNote(*rows, m_note);
	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));

	AddHeading(*rows, loc::Tr("map.world.opening.head"));
	{
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr("map.world.opening"))
			->centerV = true;
		// TWO SHAPES, not a blank field meaning one of them. "No dungeon named"
		// is how the manifest ENCODES "on the world map", and an encoding is
		// not an interface.
		std::vector<std::string> shapes{loc::Tr("map.world.opening.onworld"),
										loc::Tr("map.world.opening.indungeon")};
		row->Row<ui::DropDown>(
			ui::Len::Fill(kFieldFill * 2.0f), std::move(shapes),
			m_manifest.startDungeon.empty() ? 0 : 1, [this](int i) {
				if (i == 0) {
					m_manifest.startDungeon.clear();
					m_manifest.startLevel.clear();
					m_manifest.startX = m_manifest.startZ = -1;
				} else if (!m_dungeons.empty()) {
					m_manifest.startDungeon = m_dungeons[0].id;
					m_manifest.startLevel = m_dungeons[0].levels.empty()
												? std::string()
												: m_dungeons[0].levels[0];
				}
				if (onManifest) onManifest(m_manifest);
				m_uiRebuild = true; // the rows below appear or go
			});
		row->Space(ui::Len::Fill(0.6f));
	}
	if (!m_manifest.startDungeon.empty()) {
		std::vector<std::string> ids;
		for (const DungeonInfo& d : m_dungeons) ids.push_back(d.id);
		{
			ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill),
								loc::Tr("map.world.opening.dungeon"))
				->centerV = true;
			row->Row<ui::DropDown>(
				ui::Len::Fill(kFieldFill * 2.0f), ids,
				IndexOf(ids, m_manifest.startDungeon), [this](int i) {
					if (i < 0 || i >= static_cast<int>(m_dungeons.size())) return;
					m_manifest.startDungeon = m_dungeons[i].id;
					// THE LEVEL FOLLOWS ITS DUNGEON, or the pair names a level
					// that dungeon does not have — a fault the checker would
					// report a moment later.
					m_manifest.startLevel = m_dungeons[i].levels.empty()
												? std::string()
												: m_dungeons[i].levels[0];
					if (onManifest) onManifest(m_manifest);
					m_uiRebuild = true;
				});
			row->Space(ui::Len::Fill(0.6f));
		}
		{
			const std::vector<std::string> lv = LevelsOf(m_manifest.startDungeon);
			ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill),
								loc::Tr("map.world.opening.level"))
				->centerV = true;
			row->Row<ui::DropDown>(ui::Len::Fill(kFieldFill * 2.0f), lv,
								   IndexOf(lv, m_manifest.startLevel),
								   [this, lv](int i) {
									   if (i < 0 || i >= static_cast<int>(lv.size()))
										   return;
									   m_manifest.startLevel = lv[i];
									   if (onManifest) onManifest(m_manifest);
								   });
			row->Space(ui::Len::Fill(0.6f));
		}
		{
			ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
			row->gapRem = 0.5f;
			row->Row<ui::Label>(ui::Len::Fill(kLabelFill),
								loc::Tr("map.world.opening.cell"))
				->centerV = true;
			AddIntField(*row, ui::Len::Fill(kFieldFill), m_manifest.startX,
						[this](int v) {
							m_manifest.startX = v;
							if (onManifest) onManifest(m_manifest);
						});
			AddIntField(*row, ui::Len::Fill(kFieldFill), m_manifest.startZ,
						[this](int v) {
							m_manifest.startZ = v;
							if (onManifest) onManifest(m_manifest);
						});
			row->Space(ui::Len::Fill(0.6f));
		}
		AddNote(*rows, loc::Tr("map.world.opening.startcell"));
	}
	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));

	AddHeading(*rows, loc::Tr("map.world.eval.head"));
	{
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr("map.world.eval"))
			->centerV = true;
		// "(unset)" is an ENTRY rather than an empty box, because unset means
		// something here — the harness falls back to the manifest's first level
		// — and a blank box does not say that.
		std::vector<std::string> items{loc::Tr("map.world.unset")};
		items.insert(items.end(), m_levels.begin(), m_levels.end());
		row->Row<ui::DropDown>(
			ui::Len::Fill(kFieldFill * 2.0f), items,
			m_manifest.evalLevel.empty() ? 0 : IndexOf(items, m_manifest.evalLevel),
			[this, items](int i) {
				m_manifest.evalLevel = i <= 0 ? std::string() : items[i];
				if (onManifest) onManifest(m_manifest);
			});
		row->Space(ui::Len::Fill(0.6f));
	}
	AddNote(*rows, loc::Tr("map.world.manifest"));
}

// ---------------------------------------------------------------------------
// The Areas tab: the difficulty rectangles, IN FILE ORDER, reorderable.
// ---------------------------------------------------------------------------
void WorldSettingsDialog::BuildAreasTab(size_t tab) {
	ui::Stack* rows = TabStack(*m_tabs, tab);
	if (!m_world) return;

	ui::Stack* header = rows->Row<ui::Stack>(FormRow(), true);
	header->gapRem = 0.4f;
	header->Row<ui::Label>(ui::Len::Fill(kAreaIdFill), loc::Tr("map.world.area.id"))
		->centerV = true;
	for (const char* key :
		 {"map.world.area.x", "map.world.area.z", "map.world.area.w",
		  "map.world.area.h", "map.world.area.danger"}) {
		ui::Label* h = header->Row<ui::Label>(ui::Len::Fill(kAreaNumFill),
											  loc::Tr(key));
		h->centerV = true;
		h->dim = true;
	}
	header->Space(FooterButton(kRowBtn * 3.0f));

	const std::vector<WorldMap::Area>& areas = m_world->Areas();
	for (size_t i = 0; i < areas.size(); ++i) {
		const WorldMap::Area& a = areas[i];
		// SHARED, and by value — the reference dies at the next rebuild, and a
		// plain copy would go stale the moment the id field renames the row it
		// names. Every one of this row's callbacks reads the same box, so a
		// rename keeps the rest of the row pointed at the right area (typing
		// "moors" one character at a time is five renames, and with a fixed
		// copy the second one looked up a name that no longer existed — the
		// rename stopped dead after the first keystroke).
		auto id = std::make_shared<std::string>(a.id);
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.4f;

		// Read the live row, change the one field, hand the whole thing back.
		// Built from the LIVE area each time, so two fields edited in turn
		// cannot write each other's stale values back.
		auto edit = [this, id](auto&& mutate) {
			if (!onEditArea || !m_world) return;
			for (const WorldMap::Area& cur : m_world->Areas())
				if (cur.id == *id) {
					WorldMap::Area next = cur;
					mutate(next);
					if (onEditArea(*id, next)) *id = next.id;
					return;
				}
		};

		// An area's id is an ordinary editable field: nothing but this dialog
		// addresses one by name, so renaming is not a reference sweep.
		auto* idField = row->Row<ui::TextField>(ui::Len::Fill(kAreaIdFill), a.id);
		idField->maxLength = 24;
		ui::TextField* rawId = idField;
		rawId->onChange = [edit, rawId] {
			FilterId(rawId->text);
			if (rawId->text.empty()) return; // mid-edit, not a rename to ""
			edit([rawId](WorldMap::Area& n) { n.id = rawId->text; });
		};

		AddIntField(*row, ui::Len::Fill(kAreaNumFill), a.x,
					[edit](int v) { edit([v](WorldMap::Area& n) { n.x = v; }); });
		AddIntField(*row, ui::Len::Fill(kAreaNumFill), a.z,
					[edit](int v) { edit([v](WorldMap::Area& n) { n.z = v; }); });
		AddIntField(*row, ui::Len::Fill(kAreaNumFill), a.w,
					[edit](int v) { edit([v](WorldMap::Area& n) { n.w = v; }); });
		AddIntField(*row, ui::Len::Fill(kAreaNumFill), a.h,
					[edit](int v) { edit([v](WorldMap::Area& n) { n.h = v; }); });
		{
			// DANGER IS OPTIONAL and an empty box is how it says so — the
			// terrain's own difficulty stands. Showing the sentinel -1 would be
			// showing the encoding instead of the fact.
			auto* dif = row->Row<ui::TextField>(
				ui::Len::Fill(kAreaNumFill),
				a.difficulty >= 0.0f ? std::format("{:g}", a.difficulty)
									 : std::string());
			dif->fontRole = ui::FontRole::Mono;
			dif->fontScale = 1.0f;
			dif->maxLength = 5;
			dif->placeholder = loc::Tr("map.world.area.terrain");
			ui::TextField* rawDif = dif;
			rawDif->onChange = [edit, rawDif] {
				const std::string& t = rawDif->text;
				if (t.empty()) {
					edit([](WorldMap::Area& n) { n.difficulty = -1.0f; });
					return;
				}
				float v = 0.0f;
				const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
				if (ec == std::errc() && p == t.data() + t.size())
					edit([v](WorldMap::Area& n) { n.difficulty = v; });
			};
		}

		// Reorder and delete. The arrows ARE the ordering rule made operable:
		// later rows win, so moving one down is how an exception is made to
		// beat a region it currently loses to. An arrow with nowhere to go is
		// a SPACE rather than a dead button — the columns still line up, and
		// nothing offers a move that cannot happen.
		const int index = static_cast<int>(i);
		if (index > 0)
			row->Row<ui::Button>(FooterButton(kRowBtn), "^", [this, id, index] {
				if (onOrderArea) onOrderArea(*id, index - 1);
				m_uiRebuild = true;
			});
		else
			row->Space(FooterButton(kRowBtn));
		if (i + 1 < areas.size())
			row->Row<ui::Button>(FooterButton(kRowBtn), "v", [this, id, index] {
				if (onOrderArea) onOrderArea(*id, index + 1);
				m_uiRebuild = true;
			});
		else
			row->Space(FooterButton(kRowBtn));
		row->Row<ui::Button>(FooterButton(kRowBtn), "x", [this, id] {
			if (onDeleteArea) onDeleteArea(*id);
			m_uiRebuild = true;
		});
	}

	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	{
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Button>(FooterButton(1.8f), loc::Tr("map.world.area.add"),
							 [this] { AddArea(); });
		row->Space(ui::Len::Fill());
	}
	AddNote(*rows, loc::Tr("map.world.area.note"));
}

void WorldSettingsDialog::AddArea() {
	if (!onAddArea || !m_world) return;
	WorldMap::Area a;
	// A fresh id that steps PAST collisions rather than replacing — the
	// "+ New..." idiom the type palette uses, and the reason AddArea refuses a
	// duplicate at all.
	for (int n = 1;; ++n) {
		a.id = std::format("area{}", n);
		bool taken = false;
		for (const WorldMap::Area& e : m_world->Areas())
			if (e.id == a.id) taken = true;
		if (!taken) break;
	}
	// One cell at the world's start: somewhere the author is already looking,
	// and a shape they will change immediately either way.
	a.x = m_world->StartX();
	a.z = m_world->StartZ();
	a.w = a.h = 1;
	onAddArea(a);
	m_uiRebuild = true;
}

// ---------------------------------------------------------------------------
// The Doorways tab: the locations, and what the selected one opens onto.
// ---------------------------------------------------------------------------
void WorldSettingsDialog::BuildLocationsTab(size_t tab) {
	ui::Stack* rows = TabStack(*m_tabs, tab);
	if (!m_world) return;

	const std::vector<WorldMap::Location>& locs = m_world->Locations();
	if (locs.empty()) AddNote(*rows, loc::Tr("map.world.loc.none"));
	for (const WorldMap::Location& l : locs) {
		// One button per doorway, drawn `active` when it is the one the form
		// below is about — a list row and its selection, the monster dialog's
		// clip-row idiom.
		const std::string id = l.id;
		ui::Button* row = rows->Row<ui::Button>(
			FormRow(),
			std::format("{}  ({})  {},{}  ->  {} / {}", l.id, l.kind, l.x, l.z,
						l.Dungeon(),
						l.level.empty() ? loc::Tr("map.world.loc.entrylevel")
										: l.level),
			[this, id] {
				m_selected = id;
				m_note.clear();
				m_uiRebuild = true;
			});
		row->active = (l.id == m_selected);
	}

	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	{
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Button>(FooterButton(1.8f), loc::Tr("map.world.loc.add"),
							 [this] { AddLocation(); });
		if (Selected())
			row->Row<ui::Button>(FooterButton(1.4f), loc::Tr("map.world.loc.del"),
								 [this] {
									 if (onDeleteLocation) onDeleteLocation(m_selected);
									 m_selected.clear();
									 m_uiRebuild = true;
								 });
		row->Space(ui::Len::Fill());
	}

	const WorldMap::Location* sel = Selected();
	if (!sel) return;
	rows->Row<ui::Separator>(ui::Len::Fixed(0.5f));
	AddHeading(*rows, loc::Format("map.world.loc.head", sel->id));
	const std::string id = sel->id;

	// Everything but the cell goes through one edit: read the live doorway,
	// change the one field, hand the whole thing back.
	auto edit = [this, id](auto&& mutate) {
		if (!onEditLocation || !m_world) return;
		for (const WorldMap::Location& cur : m_world->Locations())
			if (cur.id == id) {
				WorldMap::Location next = cur;
				mutate(next);
				onEditLocation(id, next);
				return;
			}
	};

	{ // where it stands — its own callback, because occupancy can refuse it
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr("map.world.loc.cell"))
			->centerV = true;
		auto move = [this, id](int x, int z) {
			if (!onMoveLocation) return;
			SetNote(onMoveLocation(id, x, z) ? std::string()
											 : loc::Tr("map.world.loc.refused"));
		};
		AddIntField(*row, ui::Len::Fill(kFieldFill), sel->x, [this, move](int v) {
			if (const WorldMap::Location* c = Selected()) move(v, c->z);
		});
		AddIntField(*row, ui::Len::Fill(kFieldFill), sel->z, [this, move](int v) {
			if (const WorldMap::Location* c = Selected()) move(c->x, v);
		});
		row->Space(ui::Len::Fill(0.6f));
	}
	m_noteLabel = AddNote(*rows, m_note); // the status row, written by SetNote

	{ // what is behind the door
		std::vector<std::string> ids{loc::Tr("map.world.loc.sameasid")};
		for (const DungeonInfo& d : m_dungeons) ids.push_back(d.id);
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill),
							loc::Tr("map.world.loc.dungeon"))
			->centerV = true;
		row->Row<ui::DropDown>(
			ui::Len::Fill(kFieldFill * 2.0f), ids,
			sel->dungeon.empty() ? 0 : IndexOf(ids, sel->dungeon),
			[this, edit, ids](int i) {
				const std::string picked = i <= 0 ? std::string() : ids[i];
				edit([&picked](WorldMap::Location& n) {
					n.dungeon = picked;
					n.level.clear(); // the old level belonged to the old dungeon
				});
				m_uiRebuild = true;
			});
		row->Space(ui::Len::Fill(0.6f));
	}
	{ // which of its levels, if not the dungeon's own entry
		std::vector<std::string> items{loc::Tr("map.world.loc.entrylevel")};
		const std::vector<std::string>& lv = LevelsOf(sel->Dungeon());
		items.insert(items.end(), lv.begin(), lv.end());
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr("map.world.loc.level"))
			->centerV = true;
		row->Row<ui::DropDown>(ui::Len::Fill(kFieldFill * 2.0f), items,
							   sel->level.empty() ? 0 : IndexOf(items, sel->level),
							   [edit, items](int i) {
								   const std::string picked =
									   i <= 0 ? std::string() : items[i];
								   edit([&picked](WorldMap::Location& n) {
									   n.level = picked;
								   });
							   });
		row->Space(ui::Len::Fill(0.6f));
	}
	{ // where in that level it lands
		// A CHECKBOX, which makes the pair rule unrepresentable-wrong: the
		// loader asserts entryx and entryz are both set or neither, and two
		// bare number fields let you author exactly one of them.
		const bool lands = sel->entryX >= 0;
		ui::Stack* row = rows->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Checkbox>(ui::Len::Fill(kLabelFill),
							   loc::Tr("map.world.loc.entry"), lands,
							   [this, edit](bool on) {
								   edit([on](WorldMap::Location& n) {
									   n.entryX = n.entryZ = on ? 0 : -1;
								   });
								   m_uiRebuild = true;
							   });
		if (lands) {
			AddIntField(*row, ui::Len::Fill(kFieldFill), sel->entryX,
						[edit](int v) {
							edit([v](WorldMap::Location& n) { n.entryX = v; });
						});
			AddIntField(*row, ui::Len::Fill(kFieldFill), sel->entryZ,
						[edit](int v) {
							edit([v](WorldMap::Location& n) { n.entryZ = v; });
						});
		} else {
			row->Space(ui::Len::Fill(kFieldFill * 2.0f));
		}
		row->Space(ui::Len::Fill(0.6f));
	}
	AddNote(*rows, loc::Tr("map.world.loc.note"));
}

void WorldSettingsDialog::AddLocation() {
	if (!onAddLocation || !m_world) return;
	WorldMap::Location l;
	l.kind = "dungeon";
	for (int n = 1;; ++n) {
		l.id = std::format("door{}", n);
		bool taken = false;
		for (const WorldMap::Location& e : m_world->Locations())
			if (e.id == l.id) taken = true;
		if (!taken) break;
	}
	// THE FIRST PASSABLE, UNOCCUPIED CELL. A doorway on impassable ground is a
	// checker error and one on an occupied cell is refused outright, so placing
	// it anywhere would mean offering "+ Add" that sometimes does nothing.
	bool placed = false;
	for (int z = 0; z < m_world->Height() && !placed; ++z)
		for (int x = 0; x < m_world->Width() && !placed; ++x)
			if (m_world->Passable(x, z) && !m_world->LocationAt(x, z)) {
				l.x = x;
				l.z = z;
				placed = true;
			}
	if (!placed) {
		m_note = loc::Tr("map.world.loc.nowhere");
		m_uiRebuild = true;
		return;
	}
	// It opens onto the first dungeon there is; an absent `dungeon` would mean
	// "the one named after me", and nothing is named door1.
	if (!m_dungeons.empty()) l.dungeon = m_dungeons[0].id;
	const std::string id = l.id;
	if (onAddLocation(std::move(l))) {
		m_selected = id;
		m_note.clear();
	}
	m_uiRebuild = true;
}

void WorldSettingsDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	// The explainer owns the input while up: any click or Esc dismisses it, and
	// the dialog beneath stays frozen (the Balance dialog's bargain).
	if (m_helpOpen) {
		if (input.WasKeyPressed(VK_ESCAPE) ||
			input.WasMousePressed(MouseButton::Left))
			m_helpOpen = false;
		return;
	}

	if (m_uiRebuild) { // deferred from a widget callback (see the header)
		m_uiRebuild = false;
		if (m_tabs) m_activeTab = m_tabs->ActiveTab();
		BuildUI();
	}

	if (input.WasKeyPressed(VK_ESCAPE)) {
		// NOTHING TO REVERT: every edit here went straight into the world as
		// its own undo step, so Esc closes — the opposite of the Balance
		// dialog, which holds a working copy and puts it back.
		Close();
		return;
	}

	m_ui.Update(input, w, h);
	if (!m_open) return; // a footer button closed us this frame
	if (m_tabs) m_activeTab = m_tabs->ActiveTab();
}

void WorldSettingsDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th,
								 float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the world behind
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	m_ui.Render(batch, w, h);

	if (m_helpOpen) {
		const std::string_view paras[] = {loc::View("map.world.help.start"),
										  loc::View("map.world.help.opening"),
										  loc::View("map.world.help.areas"),
										  loc::View("map.world.help.doorways")};
		DrawHelpOverlay(batch, th, m_ui.GetFont(), w, h,
						loc::View("map.world.help.title"), paras);
	}
}

} // namespace dungeon::game
