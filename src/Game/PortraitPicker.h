// ============================================================================
// Game/PortraitPicker.h - choose a portrait: a grid of thumbnails filtered by
// race, sex and age (docs/portraits-plan.md, phase 3).
//
// STANDALONE on purpose. The character sheet opens it today; party creation,
// which does not exist yet, is its real home (Michael's brain dump,
// docs/portraits-notes.md). So it knows nothing about members or saves: Open
// takes a title, the id to outline as current, and what to do with a pick.
// A click on a thumbnail picks it and closes; the close box or Esc (through
// GameUI::DismissPopup) leaves without one.
//
// WHAT IT BROWSES is assets/portraits/portraits.cat - thousands of portraits,
// more than the SRV heap holds - so a tile's image comes from a ThumbCache:
// loaded a few a frame, only for the tiles on screen, as the trimmed small
// mips of each .dds, and evicted least-recently-seen.
//
// THE GRID IS ONE WIDGET. The editor's AssetPicker builds a widget per tile,
// which is fine for the asset pool and wrong for 2879 portraits re-laid out
// every frame. PortraitGrid sizes itself to all of its rows (so the ScrollArea
// around it scrolls the right distance) and draws and hit-tests only the rows
// inside the view.
//
// Built ONCE, like ItemDetailsDialog: the filter is a reserved index vector,
// so changing it allocates nothing, and the count text is reserved. The frames
// it is open are not guarded at all (Game::SteadyStateFrame - streaming
// thumbnails as you scroll is loading, not a steady state), and the frame that
// opens it is excused by the opener (Game::OverlayOpenedThisFrame).
// ============================================================================
#pragma once

#include "Game/ThumbCache.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::ui {
class DropDown;
class FontLibrary;
class Label;
class ScrollArea;
} // namespace dungeon::ui

namespace dungeon::game {

class Catalog;
class PortraitGrid; // the one-widget grid (PortraitPicker.cpp)

class PortraitPicker {
public:
	PortraitPicker(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);
	~PortraitPicker();

	// (Re)builds the widgets in the current language, keeping the filter. The
	// constructor calls it; a language switch calls it again (outside any widget
	// callback - it clears the tree).
	void Build();

	// Reads the portraits and their tags (Game::LoadPortraits, after the catalog
	// loads). Resets the filter; a picker that is open closes.
	void SetCatalog(const Catalog& portraits);

	void Open(std::string_view title, const std::string& currentId,
			  std::function<void(const std::string&)> onPick);
	void Close();
	bool IsOpen() const { return m_open; }

	// Modal for the mouse while open. Loads the visible tiles' thumbnails (a few
	// a frame) after the widgets have updated, then evicts.
	void Update(const Input& input, float width, float height, float dt);
	void Render(gfx::SpriteBatch& batch, float width, float height);

	ui::UIContext& UI() { return m_ui; }

	// --- dev / harness (`portrait picker ...`) ---------------------------------
	// The filters' words, in drop-down order after Any (portraits.cat's tags).
	static std::span<const char* const> Races();
	static std::span<const char* const> Sexes();
	static std::span<const char* const> Ages();
	// Filter by index: 0 = Any, else a value (Races() etc., 1-based). Out of
	// range is ignored. The drop-downs follow.
	void SetFilter(int race, int sex, int age);
	// Scroll to a fraction (0 = top, 1 = bottom) of the grid's scroll range.
	void ScrollTo(float fraction);
	struct Status {
		bool open = false;
		size_t shown = 0, total = 0, thumbs = 0;
		int race = 0, sex = 0, age = 0;
		size_t firstVisible = 0, visible = 0;
	};
	Status GetStatus() const;

private:
	friend class PortraitGrid;

	struct Entry {
		std::string id;
		int race = -1, sex = -1, age = -1; // -1 = unclear / untagged
	};
	struct Thumb {
		std::unique_ptr<gfx::Texture> texture;
	};

	void ApplyFilter();
	void Pick(size_t shownIndex);
	const std::string& IdAt(size_t shownIndex) const;
	const gfx::Texture* ThumbFor(size_t shownIndex); // draw time: marks seen
	void LoadVisibleThumbs(size_t max);

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	std::vector<Entry> m_entries;
	std::vector<size_t> m_shown; // indices into m_entries, reserved to its size
	int m_race = 0, m_sex = 0, m_age = 0; // 0 = Any (the drop-downs' indices)
	std::string m_current;                 // outlined in the grid
	std::function<void(const std::string&)> m_onPick;
	bool m_open = false;
	bool m_scrollToCurrent = false; // Open: once the grid has a layout
	float m_scrollFraction = -1.0f; // ScrollTo: applied after the next layout

	ThumbCache<Thumb> m_thumbs;

	// Widgets (owned by m_ui, dead after a Clear; Build sets them again).
	ui::Label* m_title = nullptr;
	ui::Label* m_count = nullptr;
	ui::DropDown* m_raceDrop = nullptr;
	ui::DropDown* m_sexDrop = nullptr;
	ui::DropDown* m_ageDrop = nullptr;
	ui::ScrollArea* m_area = nullptr;
	PortraitGrid* m_grid = nullptr;
};

} // namespace dungeon::game
