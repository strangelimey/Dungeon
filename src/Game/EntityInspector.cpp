// ============================================================================
// Game/EntityInspector.cpp — see EntityInspector.h.
// ============================================================================
#include "Game/EntityInspector.h"

#include "Core/Loc.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <format>

namespace dungeon::game {

void EntityInspector::Open(const Config& cfg, const std::vector<std::string>& spellIds,
						   PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_spellIds = spellIds;
	// A caster with no spell shows - and saves - the first one (C99). Only the
	// working copy: Esc puts back the original, spell-less as it was.
	if (m_cfg.archetype == ai::Archetype::Caster) DefaultCasterSpell(m_cfg.spell, m_spellIds);
	SetFacingValue(cfg.facing);
	SetPreview(std::move(preview));
	OpenModal();
}

void EntityInspector::PickArchetype(ai::Archetype archetype) {
	m_cfg.archetype = archetype;
	// Made a caster, it casts something from this edit on - the live monster
	// too, not only the rows the rebuild draws next frame (C99).
	if (archetype == ai::Archetype::Caster) DefaultCasterSpell(m_cfg.spell, m_spellIds);
	ApplyLive();
	RequestRebuild(); // dependent fields change
}

void EntityInspector::SetLeash(float range) {
	m_cfg.leashRange = range;
	ApplyLive();
}

void EntityInspector::ClickEditRoute() {
	if (onEditRoute) onEditRoute(m_cfg.runtimeId);
	Close(); // hand the grid to the editor for laying
}

void EntityInspector::ClickClearRoute() {
	if (onClearRoute) onClearRoute(m_cfg.runtimeId);
	m_cfg.patrolCount = 0;
	RequestRebuild(); // on the Patrol tab still: the base keeps it (C104)
}

std::string EntityInspector::ShownSpell() const {
	if (!m_spellDrop) return {};
	const int i = m_spellDrop->Selected();
	return i >= 0 && i < static_cast<int>(m_spellDrop->items.size())
			   ? m_spellDrop->items[static_cast<size_t>(i)]
			   : std::string();
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
	// Registered with the base, so a rebuild stays on the tab it was on (C104).
	ui::TabControl* tabs = ContentTabs(content.Row<ui::TabControl>(ui::Len::Fill(), 0.09f));
	const size_t tabAi = tabs->AddTab(loc::Tr("map.insp.tab.ai"));
	const size_t tabPatrol = tabs->AddTab(loc::Tr("map.insp.tab.patrol"));
	ui::Stack* ai = TabStack(*tabs, tabAi);
	ui::Stack* patrol = TabStack(*tabs, tabPatrol);
	m_spellDrop = nullptr;

	// Placement flags.
	ai->Row<ui::Checkbox>(FormRow(), loc::Tr("map.insp.asleep"), m_cfg.asleep,
						  [this](bool on) {
							  m_cfg.asleep = on;
							  ApplyLive();
						  });
	ai->Row<ui::Slider>(FormRow(1.9f), loc::Tr("map.insp.leash"), 0.0f, kLeashMax,
						m_cfg.leashRange, [this](float v) { SetLeash(v); });

	// Behaviour override (archetype + dependent params), mirroring the type dialog.
	std::vector<std::string> archItems;
	// In enum order, so the dropdown's index IS the archetype.
	for (const char* k : ai::kArchetypeNames)
		archItems.push_back(loc::Tr("archetype." + std::string(k)));
	ai->Row<ui::Label>(FormRow(), loc::Tr("map.cfg.archetype"))->centerV = true;
	ai->Row<ui::DropDown>(FormRow(), archItems, static_cast<int>(m_cfg.archetype),
						  [this](int i) { PickArchetype(static_cast<ai::Archetype>(i)); });
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
	if (m_cfg.archetype == ai::Archetype::Caster)
		m_spellDrop = CasterSpellRow(*ai, m_spellIds, m_cfg.spell, [this] { ApplyLive(); });
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
	ui::Stack* route = patrol->Row<ui::Stack>(FormRow(1.3f), true);
	route->gapRem = 0.5f;
	RowIcon(*route, m_device, "route", loc::Tr("map.insp.editroute"),
			[this] { ClickEditRoute(); });
	RowIcon(*route, m_device, "delete", loc::Tr("map.insp.clearroute"),
			[this] { ClickClearRoute(); });
	route->Space(ui::Len::Fill());
}

} // namespace dungeon::game
