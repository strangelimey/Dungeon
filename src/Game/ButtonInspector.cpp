// ============================================================================
// Game/ButtonInspector.cpp — see ButtonInspector.h.
// ============================================================================
#include "Game/ButtonInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

void ButtonInspector::Open(const Config& cfg, std::vector<std::string> doorNames,
						   PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_doorNames = std::move(doorNames);
	// A wired target whose door has been renamed/removed still shows: keep it
	// selectable so Save doesn't silently drop it.
	if (!m_cfg.target.empty() &&
		std::find(m_doorNames.begin(), m_doorNames.end(), m_cfg.target) ==
			m_doorNames.end())
		m_doorNames.push_back(m_cfg.target);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string ButtonInspector::Title() const {
	return loc::Format("map.btn.title", m_cfg.x, m_cfg.z);
}

void ButtonInspector::BuildContent(ui::Stack& c) {
	// Target: the door name this button toggles (doors are named in the door
	// inspector). None = unwired.
	c.Row<ui::Label>(FormRow(), loc::Tr("map.btn.target"));
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.btn.notarget"));
	int sel = 0;
	for (size_t i = 0; i < m_doorNames.size(); ++i) {
		names.push_back(m_doorNames[i]);
		if (m_doorNames[i] == m_cfg.target) sel = static_cast<int>(i) + 1;
	}
	c.Row<ui::DropDown>(FormRow(), names, sel, [this](int i) {
		m_cfg.target =
			i <= 0 ? std::string() : m_doorNames[static_cast<size_t>(i) - 1];
		if (onApply) onApply(m_cfg);
	});
}

void ButtonInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void ButtonInspector::Revert() {
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
