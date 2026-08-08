// ============================================================================
// Game/FixtureInspector.h — the editor's per-INSTANCE wall-torch (sconce) editor.
//
// A concrete InstanceInspector (see that header). A sconce's orientation IS the
// wall it hangs on, so its common Facing dropdown is narrowed to the cell's
// solid walls (passed in at Open) and choosing one RE-MOUNTS the torch onto that
// wall live (onRemount). The body edits the per-torch light/smoke settings: lit
// (gates light + flame + smoke), brightness (light reach in squares) and
// turbidity (smokiness). All apply live and revert on Close/Esc.
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
		bool brazier = false; // floor brazier (no wall/facing) vs wall torch
		int x = 0, z = 0;
		Direction wall = Direction::North; // the wall a torch hangs on (unused for brazier)
		bool lit = true;
		float brightness = 3.0f; // light reach in squares
		float turbidity = 0.28f; // smokiness
	};

	FixtureInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `walls` are the facings the torch may take (the cell's solid, unoccupied
	// walls, including its current one).
	void Open(const Config& cfg, const std::vector<Direction>& walls, PreviewSpec preview = {});

	// Re-mount the sconce at (x,z) from one wall to another (live); returns success.
	std::function<bool(int x, int z, Direction from, Direction to)> onRemount;
	// Apply the fixture's light/smoke settings live (cell + wall for a torch; brazier
	// flag tells the owner whether to route to the sconce or brazier setter).
	std::function<void(int x, int z, Direction wall, bool brazier, bool lit, float brightness,
					   float turbidity)>
		onSettings;
	std::function<void()> onSave; // persist the level (.map)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.29f, 0.16f, 0.44f, 0.66f}; }
	std::vector<Direction> FacingChoices() const override { return m_walls; }
	void BuildContent(ui::Stack& content) override;
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	void ApplySettings(); // push lit/brightness/turbidity via onSettings

	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<Direction> m_walls;
	Direction m_currentWall = Direction::North; // wall as it stands in the world now
};

} // namespace dungeon::game
