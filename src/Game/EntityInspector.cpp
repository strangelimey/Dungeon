// ============================================================================
// Game/EntityInspector.cpp — see EntityInspector.h.
// ============================================================================
#include "Game/EntityInspector.h"

#include "Core/Loc.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <format>

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

void EntityInspector::BuildContent(ui::Stack& content) {
	// One row taking the whole content box, and inside each tab a content-sized
	// stack (Game/DialogLayout.h TabStack) — the AI tab's rows depend on the
	// archetype, and the y cursor that used to place them had to know each
	// row's height twice over.
	m_tabs = content.Row<ui::TabControl>(ui::Len::Fill(), 0.09f);
	const size_t tabAi = m_tabs->AddTab(loc::Tr("map.insp.tab.ai"));
	const size_t tabPatrol = m_tabs->AddTab(loc::Tr("map.insp.tab.patrol"));
	ui::Stack* ai = TabStack(*m_tabs, tabAi);
	ui::Stack* patrol = TabStack(*m_tabs, tabPatrol);

	// Placement flags.
	ai->Row<ui::Checkbox>(FormRow(), loc::Tr("map.insp.asleep"), m_cfg.asleep,
						  [this](bool on) {
							  m_cfg.asleep = on;
							  ApplyLive();
						  });
	ai->Row<ui::Slider>(FormRow(1.9f), loc::Tr("map.insp.leash"), 0.0f, 12.0f,
						m_cfg.leashRange, [this](float v) {
							m_cfg.leashRange = v;
							ApplyLive();
						});

	// Behaviour override (archetype + dependent params), mirroring the type dialog.
	std::vector<std::string> archItems;
	for (const char* k : kArchKeys) archItems.push_back(loc::Tr("archetype." + std::string(k)));
	ai->Row<ui::Label>(FormRow(), loc::Tr("map.cfg.archetype"))->centerV = true;
	ai->Row<ui::DropDown>(FormRow(), archItems,
						  static_cast<int>(m_cfg.archetype), [this](int i) {
							  m_cfg.archetype = static_cast<ai::Archetype>(i);
							  ApplyLive();
							  RequestRebuild(); // dependent fields change
						  });
	const bool kites = m_cfg.archetype == ai::Archetype::Skirmisher ||
					   m_cfg.archetype == ai::Archetype::Caster;
	if (kites)
		ai->Row<ui::Slider>(FormRow(1.9f), loc::Tr("map.cfg.keeprange"), 1.0f, 10.0f,
							m_cfg.keepRange, [this](float v) {
								m_cfg.keepRange = v;
								ApplyLive();
							});
	ai->Row<ui::Slider>(FormRow(1.9f), loc::Tr("map.cfg.fleebelow"), 0.0f, 1.0f,
						m_cfg.fleeBelow, [this](float v) {
							m_cfg.fleeBelow = v;
							ApplyLive();
						});
	if (m_cfg.archetype == ai::Archetype::Caster) {
		ai->Row<ui::Label>(FormRow(), loc::Tr("map.cfg.spell"))->centerV = true;
		int sel = 0;
		for (size_t i = 0; i < m_spellIds.size(); ++i)
			if (m_spellIds[i] == m_cfg.spell) { sel = static_cast<int>(i); break; }
		std::vector<std::string> items = m_spellIds;
		if (items.empty()) items.push_back(loc::Tr("map.cfg.nospells"));
		ai->Row<ui::DropDown>(FormRow(), items, sel, [this](int i) {
			if (i >= 0 && i < static_cast<int>(m_spellIds.size()))
				m_cfg.spell = m_spellIds[i];
			ApplyLive();
		});
	}
	// Live threat readout (runtime aggro, display only — snapshot at Open, like
	// the patrol count; per-member scores in roster order + the locked index).
	ai->Row<ui::Label>(
		  FormRow(),
		  loc::Format("map.insp.threat",
					  std::format("{:.1f} / {:.1f} / {:.1f} / {:.1f}", m_cfg.threat[0],
								  m_cfg.threat[1], m_cfg.threat[2], m_cfg.threat[3]),
					  m_cfg.threatLock >= 0 ? std::to_string(m_cfg.threatLock)
											: std::string("-")))
		->centerV = true;

	// Patrol tab: waypoint count + author on the map (grid-click) or clear.
	patrol->Row<ui::Label>(FormRow(),
						   loc::Format("map.insp.waypoints", m_cfg.patrolCount))
		->centerV = true;
	patrol->Row<ui::Button>(FormRow(1.3f), loc::Tr("map.insp.editroute"), [this] {
		if (onEditRoute) onEditRoute(m_cfg.runtimeId);
		Close(); // hand the grid to the editor for laying
	});
	patrol->Row<ui::Button>(FormRow(1.3f), loc::Tr("map.insp.clearroute"), [this] {
		if (onClearRoute) onClearRoute(m_cfg.runtimeId);
		m_cfg.patrolCount = 0;
		RequestRebuild();
	});
}

} // namespace dungeon::game
