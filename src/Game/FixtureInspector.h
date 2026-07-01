// ============================================================================
// Game/FixtureInspector.h — the editor's per-INSTANCE wall-torch (sconce) editor.
//
// A concrete InstanceInspector (see that header). A sconce's orientation IS the
// wall it hangs on, so its common Facing dropdown is narrowed to the cell's
// solid walls (passed in at Open) and choosing one RE-MOUNTS the torch onto that
// wall live (onRemount). Type-specific settings (light colour/intensity/...) are
// still to come; the body is a placeholder for now.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class FixtureInspector : public InstanceInspector {
public:
	struct Config {
		std::string type; // fixture kind (for the title; "torch")
		int x = 0, z = 0;
		Direction wall = Direction::North; // the wall it currently hangs on
	};

	explicit FixtureInspector(gfx::GraphicsDevice& device) : InstanceInspector(device) {}

	// `walls` are the facings the torch may take (the cell's solid, unoccupied
	// walls, including its current one).
	void Open(const Config& cfg, const std::vector<Direction>& walls);

	// Re-mount the sconce at (x,z) from one wall to another (live); returns success.
	std::function<bool(int x, int z, Direction from, Direction to)> onRemount;
	std::function<void()> onSave; // persist the level (.map)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.35f, 0.33f, 0.30f, 0.30f}; }
	std::vector<Direction> FacingChoices() const override { return m_walls; }
	void BuildContent(const gfx::Rect& content) override;
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	std::vector<Direction> m_walls;
	Direction m_currentWall = Direction::North; // wall as it stands in the world now
	Direction m_originalWall = Direction::North; // for revert on Close/Esc
};

} // namespace dungeon::game
