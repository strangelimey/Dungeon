// ============================================================================
// Game/StairInspector.cpp - see StairInspector.h.
// ============================================================================
#include "Game/StairInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

void StairInspector::Open(const Config& cfg, std::vector<std::string> locations,
						  PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_locations = std::move(locations);
	// An exit pointing somewhere the world no longer lists still shows (the
	// ButtonInspector rule), so opening and saving cannot quietly repoint it.
	if (!m_cfg.destIsLevel && m_cfg.dest != "-" && !m_cfg.dest.empty() &&
		std::find(m_locations.begin(), m_locations.end(), m_cfg.dest) ==
			m_locations.end())
		m_locations.push_back(m_cfg.dest);
	SetFacingValue(cfg.facing);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string StairInspector::Title() const {
	return loc::Format("map.stair.title", m_cfg.typeName, m_cfg.x, m_cfg.z);
}

void StairInspector::BuildContent(ui::Stack& c) {
	// Where it goes. A short heading over the value, not one sentence: beside
	// the preview the column is a third of the dialog, and "Leaves the dungeon
	// through crypt_gate" ran 131px past it.
	if (!m_cfg.destIsLevel) {
		// An exit: chosen here (see the header). Row 0 = nowhere yet ("-").
		c.Row<ui::Label>(FormRow(), loc::Tr("map.stair.leaves"))->dim = true;
		std::vector<std::string> names;
		names.push_back(loc::Tr("map.stair.nowhere"));
		int sel = 0;
		for (size_t i = 0; i < m_locations.size(); ++i) {
			names.push_back(m_locations[i]);
			if (m_locations[i] == m_cfg.dest) sel = static_cast<int>(i) + 1;
		}
		c.Row<ui::DropDown>(FormRow(), names, sel, [this](int i) {
			m_cfg.dest = i <= 0 ? std::string("-") : m_locations[static_cast<size_t>(i) - 1];
			if (onApply) onApply(m_cfg);
		});
		return;
	}
	// A paired stair: shown, not edited (see the header).
	c.Row<ui::Label>(FormRow(), loc::Tr("map.stair.leadsto"))->dim = true;
	c.Row<ui::Label>(FormRow(),
					 loc::Format("map.stair.cell", m_cfg.dest, m_cfg.destX, m_cfg.destZ));

	c.Row<ui::Button>(FormRow(), loc::Format("map.stair.goto", m_cfg.dest), [this] {
		Close(); // keeping the edits: they are live, like any unsaved edit
		if (onGoTo) onGoTo(m_cfg);
	});
}

void StairInspector::ApplyLive() { // the common strip turns the flight
	m_cfg.facing = FacingValue();
	if (onApply) onApply(m_cfg);
}

void StairInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void StairInspector::Revert() {
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
