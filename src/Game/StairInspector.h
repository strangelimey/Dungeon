// ============================================================================
// Game/StairInspector.h - the editor's per-INSTANCE stair (and pit) editor.
//
// A concrete InstanceInspector (see that header), opened by right-clicking a
// stair's square. A stair is STATIC .map data (a `stairs` record) with a paired
// half on the level it leads to, so what this edits is deliberately narrow:
// Facing (the common strip) - the way you face stepping off it into this level,
// which is also the way anyone arriving on it faces (StairLink::facing); the
// flight is turned to rise behind it. Only this half: the pair on the other
// level is its own record, and says how you face arriving THERE.
// A paired stair's destination is SHOWN, not edited: a stair leads to its own
// square on the next floor, where its pair stands, and retargeting one half
// would break that pairing (docs/level-building.md, the stair lesson). "Go
// there" takes the editor to the other end instead, and Delete removes both
// halves (DungeonWorld::RemoveStairAt), the same as a middle-click erase.
// A WAY OUT is the exception: it has no pair, so where it leads - a world-map
// location - IS editable, as a dropdown of the world's locations plus "nowhere
// yet". Placing one from the palette opens this dialog at once to ask.
// Save persists the level (.map); Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class StairInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		std::string typeName;   // the stairs.cat entry's display name
		std::string dest;       // destination level stem, or an exit's location id
		bool destIsLevel = true; // false = an exit to the world map
		int destX = 0, destZ = 0;
		Direction facing = Direction::North;
	};

	StairInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `locations` = the world-map location ids an EXIT may lead to (ignored for
	// a paired stair). An exit's current dest is kept selectable even when the
	// world no longer lists it, so Save cannot silently drop it.
	void Open(const Config& cfg, std::vector<std::string> locations = {},
			  PreviewSpec preview = {});

	// Push the facing (and an exit's dest) to the live stair + record.
	std::function<void(const Config&)> onApply;
	std::function<void()> onSave;               // persist the level (.map)
	// Take the editor to the other end (the destination level, that square).
	// The dialog closes KEEPING its edits live, like any edit left unsaved.
	std::function<void(const Config&)> onGoTo;

protected:
	std::string Title() const override;
	// Five rows under the Facing strip, so as tall as the niche dialog's: at
	// 0.50 the stack had 132px for them and squeezed each row under its text.
	gfx::Rect Panel() const override { return {0.28f, 0.17f, 0.44f, 0.64f}; }
	void BuildContent(ui::Stack& content) override;
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::string> m_locations; // an exit's choices (see Open)
};

} // namespace dungeon::game
