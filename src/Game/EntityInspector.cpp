// ============================================================================
// Game/EntityInspector.cpp — see EntityInspector.h.
// ============================================================================
#include "Game/EntityInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

namespace dungeon::game {

namespace {
// Archetype dropdown order MUST match the ai::Archetype enum.
constexpr const char* kArchKeys[] = {"brute",  "skirmisher", "caster",
									 "swarm", "lurker",     "sentry"};
} // namespace

void EntityInspector::Open(const Config& cfg, const std::vector<std::string>& spellIds,
						   PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_spellIds = spellIds;
	SetFacingValue(cfg.facing);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string EntityInspector::Title() const {
	return loc::Format("map.insp.title", m_cfg.type);
}

void EntityInspector::ApplyLive() {
	m_cfg.facing = FacingValue();
	if (onApply) onApply(m_cfg);
}

void EntityInspector::Persist() {
	if (onSave) onSave(m_cfg);
}

void EntityInspector::Revert() {
	if (onApply) onApply(m_original); // revert the live monster to the snapshot
}

void EntityInspector::BuildContent(const gfx::Rect& content) {
	m_tabs = UI().Add<ui::TabControl>(content, 0.09f);
	const size_t tabAi = m_tabs->AddTab(loc::Tr("map.insp.tab.ai"));
	const size_t tabPatrol = m_tabs->AddTab(loc::Tr("map.insp.tab.patrol"));

	// Placement flags.
	m_tabs->AddChild<ui::Checkbox>(tabAi, gfx::Rect{0.05f, 0.03f, 0.9f, 0.09f},
								   loc::Tr("map.insp.asleep"), m_cfg.asleep, [this](bool on) {
									   m_cfg.asleep = on;
									   ApplyLive();
								   });
	m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, 0.13f, 0.9f, 0.11f},
								 loc::Tr("map.insp.leash"), 0.0f, 12.0f, m_cfg.leashRange,
								 [this](float v) {
									 m_cfg.leashRange = v;
									 ApplyLive();
								 });

	// Behaviour override (archetype + dependent params), mirroring the type dialog.
	std::vector<std::string> archItems;
	for (const char* k : kArchKeys) archItems.push_back(loc::Tr("archetype." + std::string(k)));
	m_tabs->AddChild<ui::Label>(tabAi, gfx::Rect{0.05f, 0.28f, 0.9f, 0.06f},
								loc::Tr("map.cfg.archetype"));
	m_tabs->AddChild<ui::DropDown>(tabAi, gfx::Rect{0.05f, 0.35f, 0.7f, 0.08f}, archItems,
								   static_cast<int>(m_cfg.archetype), [this](int i) {
									   m_cfg.archetype = static_cast<ai::Archetype>(i);
									   ApplyLive();
									   RequestRebuild(); // dependent fields change
								   });
	float y = 0.47f;
	const bool kites = m_cfg.archetype == ai::Archetype::Skirmisher ||
					   m_cfg.archetype == ai::Archetype::Caster;
	if (kites) {
		m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, y, 0.9f, 0.11f},
									 loc::Tr("map.cfg.keeprange"), 1.0f, 10.0f, m_cfg.keepRange,
									 [this](float v) {
										 m_cfg.keepRange = v;
										 ApplyLive();
									 });
		y += 0.14f;
	}
	m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, y, 0.9f, 0.11f},
								 loc::Tr("map.cfg.fleebelow"), 0.0f, 1.0f, m_cfg.fleeBelow,
								 [this](float v) {
									 m_cfg.fleeBelow = v;
									 ApplyLive();
								 });
	y += 0.15f;
	if (m_cfg.archetype == ai::Archetype::Caster) {
		m_tabs->AddChild<ui::Label>(tabAi, gfx::Rect{0.05f, y, 0.9f, 0.06f},
									loc::Tr("map.cfg.spell"));
		y += 0.07f;
		int sel = 0;
		for (size_t i = 0; i < m_spellIds.size(); ++i)
			if (m_spellIds[i] == m_cfg.spell) { sel = static_cast<int>(i); break; }
		std::vector<std::string> items = m_spellIds;
		if (items.empty()) items.push_back(loc::Tr("map.cfg.nospells"));
		m_tabs->AddChild<ui::DropDown>(tabAi, gfx::Rect{0.05f, y, 0.7f, 0.08f}, items, sel,
									   [this](int i) {
										   if (i >= 0 && i < static_cast<int>(m_spellIds.size()))
											   m_cfg.spell = m_spellIds[i];
										   ApplyLive();
									   });
	}

	// Patrol tab: waypoint count + author on the map (grid-click) or clear.
	m_tabs->AddChild<ui::Label>(tabPatrol, gfx::Rect{0.05f, 0.05f, 0.9f, 0.08f},
								loc::Format("map.insp.waypoints", m_cfg.patrolCount));
	m_tabs->AddChild<ui::Button>(tabPatrol, gfx::Rect{0.05f, 0.18f, 0.9f, 0.11f},
								 loc::Tr("map.insp.editroute"), [this] {
									 if (onEditRoute) onEditRoute(m_cfg.runtimeId);
									 Close(); // hand the grid to the editor for laying
								 });
	m_tabs->AddChild<ui::Button>(tabPatrol, gfx::Rect{0.05f, 0.33f, 0.9f, 0.11f},
								 loc::Tr("map.insp.clearroute"), [this] {
									 if (onClearRoute) onClearRoute(m_cfg.runtimeId);
									 m_cfg.patrolCount = 0;
									 RequestRebuild();
								 });
}

} // namespace dungeon::game
