// ============================================================================
// Game/GameUI_Stone.cpp - the Settings -> Material tab and the skin's stone.
//
// Split out of GameUI.cpp (more-ui-updates Phase 1). The stone used to be one
// dropdown on the UI tab; Michael asked for a tab of its own showing the
// stones to choose from, so the tab is a filter row (a name search, a family,
// light / dark) over a StonePicker grid of thumbnails (Game/StonePicker.h).
//
// Then, flicking through them, he asked for the PLACE to choose: a dungeon (or
// one level of it) names the material that suits it, wood and leaf and snow as
// well as stone. So the setting's default is "follow" - the first tile - and
// a pick PINS one material everywhere. Game::RefreshPlaceStone resolves the
// place (level `uistone`, else dungeons.cat `ui_stone`) and SetPlaceStone hands
// it here; ApplyStone loads whichever wins, only when its NAME changes.
//
// The materials are the curated set tools/BuildUiStones.py makes - the tiles,
// a thumbnail of each and stones.cat (luminance + family). ScanStones reads
// them ONCE: a language switch or a Video repopulate rebuilds the page, and
// reloading every thumbnail each time would be waste. The folder is still
// ui/stones and the setting still ui_stone - renaming them for the tab's new
// word would churn every saved ini for nothing a player sees.
// ============================================================================
#include "Game/GameUI.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/Serialize.h"
#include "Game/StonePicker.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace dungeon::game {

namespace {
// The families stones.cat names, in the filter's (and the grid's) order. Each
// has a settings.stone.kind.<family> label.
constexpr const char* kStoneFamilies[] = {"stone", "wood", "forest", "snow", "rock"};
} // namespace

// The stone every skinned face is cut from: assets/ui/stones/<name>.png
// (tools/BuildUiStones.py). A missing one is logged and leaves the faces on the
// skin's flat fallback colour - still chrome, just without grain.
//
// Also the Stone tab's live switch, so it may replace a stone frames still in
// flight are sampling: the GPU is drained before the old texture is released
// (the SRV rule - its slot recycles and its resource dies with it).
void GameUI::LoadStone(const std::string& name) {
	auto tex = TryLoadTextureFile(m_device, paths::Asset("ui\\stones\\" + name));
	if (!tex) log::Warn("UI stone missing: ui/stones/{}.png - faces draw without grain", name);
	if (m_stoneTex) m_device.WaitIdle();
	m_stoneTex = std::move(tex);
	m_skin.stone = m_stoneTex.get();
}

// The material to show: the pinned one, or - while the setting follows the
// place - the place's, else the default. Reloads only when the NAME moves, so
// a stair between two levels of one dungeon costs nothing.
void GameUI::ApplyStone() {
	const bool follow = m_settings.uiStone == GameSettings::kUiStoneFollow;
	const std::string& want = !m_previewStone.empty() ? m_previewStone
							  : !follow			   ? m_settings.uiStone
							  : !m_placeStone.empty()
								  ? m_placeStone
								  : std::string(GameSettings::kDefaultUiStone);
	if (want != m_shownStone) {
		m_shownStone = want;
		LoadStone(want);
	}
	// The follow tile previews what following means here and now.
	if (m_stonePicker) {
		const gfx::Texture* thumb = nullptr;
		const std::string& place =
			m_placeStone.empty() ? std::string(GameSettings::kDefaultUiStone) : m_placeStone;
		for (const StoneInfo& s : m_stones)
			if (s.name == place) thumb = s.thumb.get();
		m_stonePicker->SetThumb(GameSettings::kUiStoneFollow, thumb);
	}
}

const gfx::Texture* GameUI::StoneThumb(std::string_view name) {
	ScanStones();
	for (const StoneInfo& s : m_stones)
		if (s.name == name) return s.thumb.get();
	return nullptr;
}

std::vector<std::string> GameUI::StoneOrder() {
	ScanStones();
	std::vector<std::string> names;
	names.reserve(m_stones.size());
	for (const StoneInfo& s : m_stones) names.push_back(s.name);
	return names;
}

std::vector<std::string> GameUI::StoneFilterLabels() const {
	std::vector<std::string> labels{loc::Tr("settings.stone.kind.all"),
									loc::Tr("settings.stone.light"),
									loc::Tr("settings.stone.dark")};
	for (const char* f : kStoneFamilies)
		labels.push_back(loc::Tr(std::string("settings.stone.kind.") + f));
	return labels;
}

std::vector<Vec4> GameUI::StoneFilterColors() const {
	// A hint of each category, not a sample of any one material: "all" has no
	// chip, light and dark are greys at either end, and each kind wears the
	// colour it is mostly found in.
	return {
		{0.0f, 0.0f, 0.0f, 0.0f},   // all kinds: none
		{0.84f, 0.83f, 0.80f, 1.0f}, // light
		{0.20f, 0.20f, 0.22f, 1.0f}, // dark
		{0.56f, 0.57f, 0.60f, 1.0f}, // stone
		{0.56f, 0.33f, 0.17f, 1.0f}, // wood
		{0.30f, 0.56f, 0.22f, 1.0f}, // forest
		{0.84f, 0.92f, 1.00f, 1.0f}, // snow and ice
		{0.64f, 0.43f, 0.30f, 1.0f}, // rock
	};
}

unsigned GameUI::StoneFilterBits(std::string_view name) {
	ScanStones();
	for (const StoneInfo& s : m_stones) {
		if (s.name != name) continue;
		unsigned bits = 1u; // "all"
		bits |= s.luminance >= StonePicker::kLightFrom ? 1u << 1 : 1u << 2;
		for (size_t i = 0; i < std::size(kStoneFamilies); ++i)
			if (s.family == kStoneFamilies[i]) bits |= 1u << (3 + i);
		return bits;
	}
	return ~0u; // unknown: never hidden
}

bool GameUI::FollowPlaceStone() {
	if (m_settings.uiStone == GameSettings::kUiStoneFollow) return false;
	m_settings.uiStone = GameSettings::kUiStoneFollow;
	m_settings.Save();
	if (m_stonePicker) m_stonePicker->SetCurrent(GameSettings::kUiStoneFollow);
	ApplyStone();
	return true;
}

void GameUI::PreviewStone(std::string name) {
	m_previewStone = std::move(name);
	ApplyStone();
}

void GameUI::EndStonePreview() {
	if (m_previewStone.empty()) return;
	m_previewStone.clear();
	ApplyStone();
}

void GameUI::SetPlaceStone(std::string name) {
	m_placeStone = std::move(name);
	ApplyStone();
}

void GameUI::ScanStones() {
	if (m_stonesScanned) return;
	m_stonesScanned = true;

	// Every tile in the folder is a choice (one the index does not know is
	// still offered, as a dark stone); the index says how light each one is
	// and what family it belongs to.
	for (std::string& name : InstalledUiStones())
		m_stones.push_back({std::move(name), 0.0f, "stone", nullptr});

	if (auto bytes = assets::ReadBinaryFile(paths::Asset("ui\\stones\\stones.cat"))) {
		const std::string text(bytes->begin(), bytes->end());
		for (const serialize::Block& b : serialize::ParseBlocks(text))
			for (StoneInfo& s : m_stones)
				if (s.name == b.id) {
					s.luminance = std::strtof(b.Get("luminance", "0").c_str(), nullptr);
					s.family = b.Get("family", "stone");
				}
	} else {
		log::Warn("ui/stones/stones.cat missing - the Material tab cannot tell light from "
				  "dark, or one family from another");
	}

	for (StoneInfo& s : m_stones) {
		s.thumb = TryLoadTextureFile(m_device, paths::Asset("ui\\stones\\thumbs\\" + s.name));
		if (!s.thumb) log::Warn("UI stone '{}' has no thumbnail - re-run BuildUiStones.py", s.name);
	}

	// By family in the filter's order, lightest first within one: a list
	// ordered by kind and value reads as a range rather than an alphabet.
	auto rank = [](const std::string& f) {
		for (size_t i = 0; i < std::size(kStoneFamilies); ++i)
			if (f == kStoneFamilies[i]) return i;
		return std::size(kStoneFamilies);
	};
	std::sort(m_stones.begin(), m_stones.end(), [&](const StoneInfo& a, const StoneInfo& b) {
		if (rank(a.family) != rank(b.family)) return rank(a.family) < rank(b.family);
		return a.luminance != b.luminance ? a.luminance > b.luminance : a.name < b.name;
	});
}

void GameUI::BuildStoneTab(ui::TabControl& tabs) {
	ScanStones();
	const size_t tab = tabs.AddTab(loc::Tr("settings.tab.stone"));
	// A content-sized stack, like every other settings tab (GameUI.cpp
	// SettingsTab), so a long list scrolls the page.
	auto* rows = tabs.AddChild<ui::Stack>(tab, gfx::Rect{0, 0, 1, 1});
	rows->debugName = "rows";
	rows->fitContent = true;
	rows->padRem = 1.0f;
	rows->gapRem = 0.9f;

	// The filter row: the search box, then the family, then the shade. Each
	// option says what it shows, so the row needs no labels of its own.
	auto* bar = rows->Row<ui::Stack>(ui::Len::Fixed(1.45f), /*horizontal*/ true);
	bar->debugName = "StoneFilter";
	bar->gapRem = 0.8f;
	auto* field = bar->Row<ui::TextField>(ui::Len::Fill());
	field->placeholder = loc::Tr("settings.stone.filter");
	std::vector<std::string> kinds{loc::Tr("settings.stone.kind.all")};
	for (const char* f : kStoneFamilies) kinds.push_back(loc::Tr(std::string("settings.stone.kind.") + f));
	auto* kind = bar->Row<ui::DropDown>(ui::Len::Fixed(9.0f), std::move(kinds), 0, nullptr);
	std::vector<std::string> shows{loc::Tr("settings.stone.all"),
								   loc::Tr("settings.stone.light"),
								   loc::Tr("settings.stone.dark")};
	auto* show = bar->Row<ui::DropDown>(ui::Len::Fixed(9.0f), std::move(shows), 0, nullptr);

	std::vector<StonePicker::Entry> entries;
	entries.reserve(m_stones.size() + 1);
	// First, the choice of who decides: the place (ApplyStone points its
	// thumbnail at whatever the place has picked).
	entries.push_back({GameSettings::kUiStoneFollow, loc::Tr("settings.stone.follow"), 0.0f,
					   std::string(), nullptr, /*always*/ true});
	for (const StoneInfo& s : m_stones)
		entries.push_back(
			{s.name, loc::Tr("stone." + s.name), s.luminance, s.family, s.thumb.get()});
	m_stonePicker = rows->Row<StonePicker>(
		ui::Len::Fit(), std::move(entries), m_settings.uiStone, [this](const std::string& name) {
			Click();
			m_settings.uiStone = name;
			ApplyStone();
			m_settings.Save();
		});
	m_stonePicker->emptyText = loc::Tr("settings.stone.none");
	ApplyStone(); // the follow tile's thumbnail

	StonePicker* picker = m_stonePicker;
	field->onChange = [field, picker] { picker->SetFilter(field->text); };
	kind->onSelect = [this, picker](int index) {
		Click();
		picker->SetFamily(index > 0 && index <= static_cast<int>(std::size(kStoneFamilies))
							  ? kStoneFamilies[index - 1]
							  : "");
	};
	show->onSelect = [this, picker](int index) {
		Click();
		picker->SetShow(static_cast<StonePicker::Show>(std::clamp(index, 0, 2)));
	};
}

} // namespace dungeon::game
