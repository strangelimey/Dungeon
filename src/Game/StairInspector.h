// ============================================================================
// Game/StairInspector.h - the editor's per-INSTANCE stair (and pit) editor.
//
// A concrete InstanceInspector (see that header), opened by right-clicking a
// stair's square. A stair is STATIC .map data (a `stairs` record) with a paired
// half on the level it leads to, so what this edits is deliberately narrow:
//   * Facing (the common strip) - which way the flight is turned. Only this
//     half: the pair on the other level is its own record.
//   * Arrive facing - which way the party faces on landing at the other end
//     (the record's destfacing). Not offered for an exit, whose far end is a
//     world-map location rather than a square.
// The destination itself is SHOWN, not edited: a stair leads to its own square
// on the next floor, where its pair stands, and retargeting one half would
// break that pairing (docs/level-building.md, the stair lesson). "Go there"
// takes the editor to the other end instead, and Delete removes both halves
// (DungeonWorld::RemoveStairAt), the same as a middle-click erase.
// Save persists the level (.map); Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>

namespace dungeon::game {

class StairInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		std::string typeName;   // the stairs.cat entry's display name
		std::string dest;       // destination level stem, or an exit's location id
		bool destIsLevel = true; // false = an exit to the world map
		int destX = 0, destZ = 0;
		Direction facing = Direction::South;
		Direction destFacing = Direction::South;
	};

	StairInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	void Open(const Config& cfg, PreviewSpec preview = {});

	std::function<void(const Config&)> onApply; // push facings to the live stair + record
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
};

} // namespace dungeon::game
