// ============================================================================
// Game/NicheInspector.h — the editor's per-INSTANCE wall-niche editor.
//
// A concrete InstanceInspector (see that header), opened by Select-clicking a
// niche's cell. The common Facing strip is repurposed as the FACE picker (which
// wall of the cell the niche is carved into), narrowed to the cell's solid,
// un-carved walls — so a niche placed on the wrong wall is re-faced here rather
// than deleted and redone; any treasure in its pocket travels with it
// (DungeonWorld::RemountNiche).
// The body edits the niche's AUTHORED state: its shape Type (wallfeatures.cat),
// whether it Starts closed (a secret niche, blank wall until a button whose
// target= names it opens it — DungeonWorld::ToggleNichesNamed), and that Name.
// Save persists the level (.map); Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class NicheInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;                  // the niche's floor cell
		Direction wall = Direction::North; // the wall face it is carved into
		std::string type = "niche";        // wallfeatures.cat id (shape)
		bool hidden = false;               // starts closed (secret)
		std::string name;                  // button-target id ("" = unwired)
	};

	explicit NicheInspector(gfx::GraphicsDevice& device) : InstanceInspector(device) {}

	// `types` are the selectable niche shapes as (id, display) pairs; `walls` are
	// the faces this niche may move to (the cell's solid walls not already
	// carved, plus its current one).
	void Open(const Config& cfg, std::vector<std::pair<std::string, std::string>> types,
			  std::vector<Direction> walls, PreviewSpec preview = {});

	std::function<void(const Config&)> onApply; // push to the live niche + record
	// Move the niche at (x,z) from one face to another (live); returns success.
	// Mirrors the fixture dialog's sconce re-mount.
	std::function<bool(int x, int z, Direction from, Direction to)> onRemount;
	std::function<void()> onSave;               // persist the level (.map)

protected:
	std::string Title() const override;
	// Taller than the original 0.54: the Face strip costs the content band
	// (kFacingH - 0.02) of the panel, which pushed the Name field into the footer.
	// The content rows need 0.33 in window fractions, so the band — 0.58 * height
	// — has to stay above that (0.58 * 0.62 = 0.36, with room to spare).
	gfx::Rect Panel() const override { return {0.33f, 0.17f, 0.34f, 0.62f}; }
	// The faces this niche may occupy — the common Facing strip becomes a "which
	// wall" picker, so a mis-placed niche is re-faced instead of deleted+redone.
	std::vector<Direction> FacingChoices() const override { return m_walls; }
	void BuildContent(const gfx::Rect& content) override;
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::pair<std::string, std::string>> m_types; // (id, display)
	std::vector<Direction> m_walls;             // faces the niche may move to
	Direction m_currentWall = Direction::North; // face as it stands in the world now
};

} // namespace dungeon::game
