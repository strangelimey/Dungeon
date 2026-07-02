// ============================================================================
// Game/DoorInspector.cpp — see DoorInspector.h.
// ============================================================================
#include "Game/DoorInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

namespace dungeon::game {

void DoorInspector::Open(const Config& cfg,
						 std::vector<std::pair<std::string, std::string>> keys,
						 PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_keys = std::move(keys);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string DoorInspector::Title() const {
	return loc::Format("map.door.title", m_cfg.x, m_cfg.z);
}

void DoorInspector::BuildContent(const gfx::Rect& c) {
	// Open flips the live panel (slide anim) and the record's authored state.
	UI().Add<ui::Checkbox>(gfx::Rect{c.x, c.y, c.w, 0.055f}, loc::Tr("map.door.open"),
						   m_cfg.open, [this](bool on) {
							   m_cfg.open = on;
							   if (onApply) onApply(m_cfg);
						   });

	// Required key: "None" + every items.cat entry with category=key. Selecting
	// one authors key=<id> on the record — the party's click then refuses until
	// key items (and an inventory check) exist; wired buttons ignore locks.
	UI().Add<ui::Label>(gfx::Rect{c.x, c.y + 0.10f, c.w, 0.05f},
						loc::Tr("map.door.key"));
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.door.nokey"));
	int sel = 0;
	for (size_t i = 0; i < m_keys.size(); ++i) {
		names.push_back(m_keys[i].second);
		if (m_keys[i].first == m_cfg.key) sel = static_cast<int>(i) + 1;
	}
	UI().Add<ui::DropDown>(gfx::Rect{c.x, c.y + 0.15f, c.w, 0.05f}, names, sel,
						   [this](int i) {
							   m_cfg.key = i <= 0 ? std::string()
												  : m_keys[static_cast<size_t>(i) - 1].first;
							   if (onApply) onApply(m_cfg);
						   });
}

void DoorInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void DoorInspector::Revert() {
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
