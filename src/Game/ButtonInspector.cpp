// ============================================================================
// Game/ButtonInspector.cpp — see ButtonInspector.h.
// ============================================================================
#include "Game/ButtonInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

void ButtonInspector::Open(const Config& cfg, std::vector<std::string> doorNames,
						   FlagChoices flags, PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_doorNames = std::move(doorNames);
	m_flags = std::move(flags);
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

	// The flag it WAITS on: until it is on, the lever will not move.
	c.Row<ui::Label>(FormRow(), loc::Tr("map.btn.needs"));
	FlagDropDown(c, FormRow(), m_flags, m_cfg.needs, [this] {
		if (onApply) onApply(m_cfg);
	});

	// What a press DOES to a flag: the verb, then the flag it acts on. Stacked,
	// not side by side - half the column was too narrow for a flag's name and
	// its scope, which the dropdown then had to trim (and uioverlap reports).
	c.Row<ui::Label>(FormRow(), loc::Tr("map.btn.onpress"));
	const std::vector<std::string> ops{loc::Tr("map.btn.op.none"), loc::Tr("map.btn.op.set"),
									   loc::Tr("map.btn.op.clear"),
									   loc::Tr("map.btn.op.toggle")};
	c.Row<ui::DropDown>(FormRow(), ops, static_cast<int>(m_cfg.op), [this](int i) {
		m_cfg.op = static_cast<FlagOp>(std::clamp(i, 0, 3));
		if (onApply) onApply(m_cfg);
	});
	FlagDropDown(c, FormRow(), m_flags, m_cfg.sets, [this] {
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
