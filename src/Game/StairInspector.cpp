// ============================================================================
// Game/StairInspector.cpp - see StairInspector.h.
// ============================================================================
#include "Game/StairInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

namespace dungeon::game {

void StairInspector::Open(const Config& cfg, PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	SetFacingValue(cfg.facing);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string StairInspector::Title() const {
	return loc::Format("map.stair.title", m_cfg.typeName, m_cfg.x, m_cfg.z);
}

void StairInspector::BuildContent(ui::Stack& c) {
	// Where it goes. Shown, not edited: see the header. A short heading over
	// the value, not one sentence: beside the preview the column is a third of
	// the dialog, and "Leaves the dungeon through crypt_gate" ran 131px past it.
	if (!m_cfg.destIsLevel) {
		c.Row<ui::Label>(FormRow(), loc::Tr("map.stair.leaves"))->dim = true;
		c.Row<ui::Label>(FormRow(), m_cfg.dest);
		return;
	}
	c.Row<ui::Label>(FormRow(), loc::Tr("map.stair.leadsto"))->dim = true;
	c.Row<ui::Label>(FormRow(),
					 loc::Format("map.stair.cell", m_cfg.dest, m_cfg.destX, m_cfg.destZ));

	// Which way the party faces on landing.
	c.Row<ui::Label>(FormRow(), loc::Tr("map.stair.arrive"));
	static constexpr Direction kDirs[] = {Direction::North, Direction::East, Direction::South,
										  Direction::West};
	std::vector<std::string> names;
	int sel = 0;
	for (size_t i = 0; i < std::size(kDirs); ++i) {
		names.push_back(loc::Tr(FacingLocKey(kDirs[i])));
		if (kDirs[i] == m_cfg.destFacing) sel = static_cast<int>(i);
	}
	c.Row<ui::DropDown>(FormRow(), names, sel, [this](int i) {
		if (i < 0 || i >= static_cast<int>(std::size(kDirs))) return;
		m_cfg.destFacing = kDirs[static_cast<size_t>(i)];
		if (onApply) onApply(m_cfg);
	});

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
