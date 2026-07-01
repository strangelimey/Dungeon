// ============================================================================
// Game/FixtureInspector.cpp — see FixtureInspector.h.
// ============================================================================
#include "Game/FixtureInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

namespace dungeon::game {

void FixtureInspector::Open(const Config& cfg, const std::vector<Direction>& walls) {
	m_cfg = cfg;
	m_walls = walls;
	m_currentWall = cfg.wall;
	m_originalWall = cfg.wall;
	SetFacingValue(cfg.wall);
	OpenModal();
}

std::string FixtureInspector::Title() const {
	return loc::Format("map.fix.title", m_cfg.x, m_cfg.z);
}

void FixtureInspector::BuildContent(const gfx::Rect& content) {
	// Placeholder until torch settings (colour/intensity/lit) land; facing lives
	// in the common strip above.
	UI().Add<ui::Label>(gfx::Rect{content.x, content.y, content.w, 0.05f},
						loc::Tr("map.fix.nosettings"));
}

void FixtureInspector::ApplyLive() {
	const Direction to = FacingValue();
	if (to == m_currentWall) return;
	if (onRemount && onRemount(m_cfg.x, m_cfg.z, m_currentWall, to)) {
		m_currentWall = to;
	} else { // couldn't re-mount — snap the dropdown back to the live wall
		SetFacingValue(m_currentWall);
		RequestRebuild();
	}
}

void FixtureInspector::Persist() {
	if (onSave) onSave();
}

void FixtureInspector::Revert() {
	if (m_currentWall != m_originalWall && onRemount &&
		onRemount(m_cfg.x, m_cfg.z, m_currentWall, m_originalWall))
		m_currentWall = m_originalWall;
}

} // namespace dungeon::game
