// ============================================================================
// Game/InspectPicker.h — the editor's "what do you want to edit?" chooser.
//
// When a Select-clicked cell holds more than one inspectable object (e.g. two
// wall torches on different walls, or a monster stacked with a torch), the
// editor pops this small modal list first: one row per object. Picking a row
// fires onPick(index) and closes; the owner (Game) then opens the matching
// inspector. Deliberately generic — it knows only labels + indices, so it stays
// decoupled from the monster/fixture domain (Game maps the index to a target).
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class InspectPicker {
public:
	explicit InspectPicker(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	// Show a titled list; each entry is one selectable object at the clicked cell.
	void Open(const std::string& title, const std::vector<std::string>& items);
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Fired when a row is chosen (index into the Open() items). Closes first.
	std::function<void(int index)> onPick;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;    // the dialog's title text
	ui::UIContext m_ui; // the row buttons + Close

	bool m_open = false;
	std::string m_title;
	std::vector<std::string> m_items;
	gfx::Rect m_panel{}; // computed in Open (sized to the row count), used by Render
	gfx::Rect m_titleRect{};
};

} // namespace dungeon::game
