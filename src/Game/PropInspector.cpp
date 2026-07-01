// ============================================================================
// Game/PropInspector.cpp — see PropInspector.h.
// ============================================================================
#include "Game/PropInspector.h"

#include "Core/Loc.h"

namespace dungeon::game {

void PropInspector::Open(const Config& cfg) {
	m_cfg = cfg;
	m_original = cfg;
	SetFacingValue(cfg.facing);
	OpenModal();
}

std::string PropInspector::Title() const {
	const char* key = m_cfg.kind == Config::Kind::Item ? "map.prop.item" : "map.prop.deco";
	return loc::Format(key, m_cfg.type);
}

void PropInspector::ApplyLive() {
	m_cfg.facing = FacingValue();
	if (onApply) onApply(m_cfg);
}

void PropInspector::Persist() {
	if (onSave) onSave();
}

void PropInspector::Revert() {
	m_cfg.facing = m_original.facing;
	if (onApply) onApply(m_cfg);
}

} // namespace dungeon::game
