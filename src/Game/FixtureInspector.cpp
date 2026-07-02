// ============================================================================
// Game/FixtureInspector.cpp — see FixtureInspector.h.
// ============================================================================
#include "Game/FixtureInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

namespace dungeon::game {

void FixtureInspector::Open(const Config& cfg, const std::vector<Direction>& walls) {
	m_cfg = cfg;
	m_original = cfg;
	m_walls = walls;
	m_currentWall = cfg.wall;
	SetFacingValue(cfg.wall);
	OpenModal();
}

std::string FixtureInspector::Title() const {
	return loc::Format("map.fix.title", m_cfg.x, m_cfg.z);
}

void FixtureInspector::ApplySettings() {
	if (onSettings)
		onSettings(m_cfg.x, m_cfg.z, m_currentWall, m_cfg.lit, m_cfg.brightness, m_cfg.turbidity);
}

void FixtureInspector::BuildContent(const gfx::Rect& c) {
	// Lit gates the light, flame particles and smoke; brightness is the light reach
	// (squares); turbidity is how much haze it adds to its cell and neighbours.
	UI().Add<ui::Checkbox>(gfx::Rect{c.x, c.y, c.w, 0.055f}, loc::Tr("map.fix.lit"), m_cfg.lit,
						   [this](bool on) {
							   m_cfg.lit = on;
							   ApplySettings();
						   });
	UI().Add<ui::Slider>(gfx::Rect{c.x, c.y + 0.09f, c.w, 0.11f}, loc::Tr("map.fix.brightness"),
						 1.0f, 8.0f, m_cfg.brightness, [this](float v) {
							 m_cfg.brightness = v;
							 ApplySettings();
						 });
	UI().Add<ui::Slider>(gfx::Rect{c.x, c.y + 0.21f, c.w, 0.11f}, loc::Tr("map.fix.turbidity"),
						 0.0f, 1.0f, m_cfg.turbidity, [this](float v) {
							 m_cfg.turbidity = v;
							 ApplySettings();
						 });
}

void FixtureInspector::ApplyLive() { // the common Facing dropdown re-mounts the torch
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
	// Restore the wall (re-mount back) and the light/smoke settings.
	if (m_currentWall != m_original.wall && onRemount &&
		onRemount(m_cfg.x, m_cfg.z, m_currentWall, m_original.wall))
		m_currentWall = m_original.wall;
	if (onSettings)
		onSettings(m_cfg.x, m_cfg.z, m_currentWall, m_original.lit, m_original.brightness,
				   m_original.turbidity);
}

} // namespace dungeon::game
