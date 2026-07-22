// ============================================================================
// Game/NicheInspector.cpp — see NicheInspector.h.
// ============================================================================
#include "Game/NicheInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <cctype>

namespace dungeon::game {

void NicheInspector::Open(const Config& cfg,
						  std::vector<std::pair<std::string, std::string>> types,
						  std::vector<Direction> walls, PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_types = std::move(types);
	m_walls = std::move(walls);
	m_currentWall = cfg.wall;
	SetFacingValue(cfg.wall); // the common strip is this niche's FACE picker
	SetPreview(std::move(preview));
	OpenModal();
}

std::string NicheInspector::Title() const {
	return loc::Format("map.niche.title", m_cfg.x, m_cfg.z);
}

void NicheInspector::BuildContent(const gfx::Rect& c) {
	// Type: the niche shape (wallfeatures.cat). Swapping it re-stamps the panel.
	UI().Add<ui::Label>(gfx::Rect{c.x, c.y, c.w, 0.05f}, loc::Tr("map.niche.type"));
	std::vector<std::string> names;
	int sel = 0;
	for (size_t i = 0; i < m_types.size(); ++i) {
		names.push_back(m_types[i].second);
		if (m_types[i].first == m_cfg.type) sel = static_cast<int>(i);
	}
	UI().Add<ui::DropDown>(gfx::Rect{c.x, c.y + 0.05f, c.w, 0.05f}, names, sel,
						   [this](int i) {
							   if (i >= 0 && i < static_cast<int>(m_types.size()))
								   m_cfg.type = m_types[static_cast<size_t>(i)].first;
							   if (onApply) onApply(m_cfg);
						   });

	// Starts closed: a secret niche renders as blank wall until a button reveals it.
	UI().Add<ui::Checkbox>(gfx::Rect{c.x, c.y + 0.14f, c.w, 0.055f},
						   loc::Tr("map.niche.hidden"), m_cfg.hidden, [this](bool on) {
							   m_cfg.hidden = on;
							   if (onApply) onApply(m_cfg);
						   });

	// Name: what a button's target= points at (record-safe chars only, like doors).
	UI().Add<ui::Label>(gfx::Rect{c.x, c.y + 0.23f, c.w, 0.05f}, loc::Tr("map.niche.name"));
	ui::TextField* name =
		UI().Add<ui::TextField>(gfx::Rect{c.x, c.y + 0.28f, c.w, 0.05f}, m_cfg.name);
	name->placeholder = loc::Tr("map.niche.namehint");
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

void NicheInspector::ApplyLive() { // the common Facing strip re-faces the niche
	const Direction to = FacingValue();
	if (to == m_currentWall) return;
	if (onRemount && onRemount(m_cfg.x, m_cfg.z, m_currentWall, to)) {
		m_currentWall = to;
		m_cfg.wall = to; // later edits (type/name/hidden) address the new face
	} else { // couldn't move it — snap the dropdown back to the live face
		SetFacingValue(m_currentWall);
		RequestRebuild();
	}
}

void NicheInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void NicheInspector::Revert() {
	// Put the niche back on its original face, then restore the authored fields.
	if (m_currentWall != m_original.wall && onRemount &&
		onRemount(m_cfg.x, m_cfg.z, m_currentWall, m_original.wall))
		m_currentWall = m_original.wall;
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
