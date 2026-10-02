// ============================================================================
// Game/StonePicker.h - the Settings -> Stone tab's grid of UI stones.
//
// Michael (more-ui-updates, 2026-10-01): "make the stone background a setting
// tab ... that shows a filtered list of the stone textures to choose from."
// The list is the CURATED set only (assets/ui/stones, tools/BuildUiStones.py) -
// every entry was toned for panels, so whatever is picked is known to work.
//
// One tile per stone: its thumbnail (assets/ui/stones/thumbs, a crop of the
// tile at about the grain a panel shows) under the panel's bevel, so a tile
// reads as a little slab of the chrome it would make, with the stone's name
// beneath. The current stone is outlined in the accent. A click picks it.
//
// The FIRST tile is "follow the dungeon" (Entry::always): the place decides
// (Game::RefreshPlaceStone), and the tile wears whatever the place has picked,
// so it previews what following means right now. Wood, leaf, snow and rock
// joined the stones the same day (Michael), which is why the tab is called
// Material and has a family filter beside the light / dark one.
//
// The grid WRAPS: as many columns as fit the page, as many rows as that takes.
// That is why it is a Len::Fit row (UI/Layout.h) - its height depends on the
// width, which no fixed rem can say at every window shape. FILTERING (a name
// search, a light / dark choice) hides tiles and closes the grid up; the
// stack re-measures it on the next layout.
//
// The tab is built once per settings-page build; the entries and their
// thumbnails are owned by GameUI and outlive the page.
// ============================================================================
#pragma once

#include "UI/Widget.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::gfx { class Texture; }

namespace dungeon::game {

class StonePicker : public ui::Widget {
public:
	struct Entry {
		std::string name;   // the file stem: what ui_stone= stores
		std::string label;  // stone.<name>, localized
		float luminance = 0.0f;
		std::string family; // stone / wood / forest / snow / rock (stones.cat)
		const gfx::Texture* thumb = nullptr; // null = a flat swatch
		// Shown whatever the filters say - the "follow the dungeon" tile, which
		// is a choice of who decides, not a material to search for.
		bool always = false;
	};
	enum class Show { All, Light, Dark };

	// At or above this luminance a stone counts as light (BuildUiStones.py's
	// LIGHT_FROM - the two must agree).
	static constexpr float kLightFrom = 0.25f;

	StonePicker(const gfx::Rect& rect, std::vector<Entry> entries, std::string current,
				std::function<void(const std::string&)> onPick);

	void SetFilter(std::string_view text);
	void SetShow(Show show);
	// Only materials of this family; empty = every family.
	void SetFamily(std::string_view family) { m_family.assign(family); }
	// Re-points one entry's thumbnail (the follow tile shows the place's).
	void SetThumb(std::string_view name, const gfx::Texture* thumb);
	void SetCurrent(std::string_view name) { m_current = name; }
	const std::string& Current() const { return m_current; }
	size_t ShownCount() const;

	// Shown when the filter leaves nothing.
	std::string emptyText;

	float FitExtent(float crossPx, float remPx) const override;
	// The most any shown name is trimmed by (its tile is its room).
	float TextOverrun() const override;

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	bool Matches(const Entry& e) const;
	int Columns(float widthPx, float remPx) const;
	// The rect of the `slot`-th SHOWN tile (thumbnail + name line).
	gfx::Rect TileRect(size_t slot) const;

	std::vector<Entry> m_entries;
	std::string m_current;
	std::function<void(const std::string&)> m_onPick;
	std::string m_filter; // lower-cased
	Show m_show = Show::All;
	std::string m_family; // empty = all
	int m_hot = -1; // entry index under the pointer
};

} // namespace dungeon::game
