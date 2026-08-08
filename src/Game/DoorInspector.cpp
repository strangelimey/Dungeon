// ============================================================================
// Game/DoorInspector.cpp — see DoorInspector.h.
// ============================================================================
#include "Game/DoorInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <cctype>

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

void DoorInspector::BuildContent(ui::Stack& c) {
	// Open flips the live panel (slide anim) and the record's authored state.
	c.Row<ui::Checkbox>(FormRow(), loc::Tr("map.door.open"), m_cfg.open,
						[this](bool on) {
							m_cfg.open = on;
							if (onApply) onApply(m_cfg);
						});

	// Required key: "None" + every items.cat entry with category=key. Selecting
	// one authors key=<id> on the record — the party's click then opens the
	// door only while a member carries the item; wired buttons ignore locks.
	c.Row<ui::Label>(FormRow(), loc::Tr("map.door.key"));
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.door.nokey"));
	int sel = 0;
	for (size_t i = 0; i < m_keys.size(); ++i) {
		names.push_back(m_keys[i].second);
		if (m_keys[i].first == m_cfg.key) sel = static_cast<int>(i) + 1;
	}
	c.Row<ui::DropDown>(FormRow(), names, sel, [this](int i) {
		m_cfg.key =
			i <= 0 ? std::string() : m_keys[static_cast<size_t>(i) - 1].first;
		if (onApply) onApply(m_cfg);
	});

	// Name: what a button's target= points at. Kept record-safe as it is typed
	// (records are whitespace-tokenised key=value lines, so spaces/'=' would
	// corrupt the .ent — the filter drops anything outside [A-Za-z0-9_-]).
	c.Row<ui::Label>(FormRow(), loc::Tr("map.door.name"));
	ui::TextField* name = c.Row<ui::TextField>(FormRow(), m_cfg.name);
	name->placeholder = loc::Tr("map.door.namehint");
	name->maxLength = 24;
	name->onChange = [this, name] {
		std::erase_if(name->text, [](char ch) {
			const unsigned char u = static_cast<unsigned char>(ch);
			return !(std::isalnum(u) || ch == '_' || ch == '-');
		});
		m_cfg.name = name->text;
		if (onApply) onApply(m_cfg);
	};
}

void DoorInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void DoorInspector::Revert() {
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
