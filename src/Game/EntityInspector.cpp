// ============================================================================
// Game/EntityInspector.cpp — see EntityInspector.h.
// ============================================================================
#include "Game/EntityInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Panel + region geometry, as fractions (0..1) of the window (taller now — the
// behaviour controls join the AI tab).
constexpr gfx::Rect kPanel{0.31f, 0.20f, 0.38f, 0.60f};
constexpr gfx::Rect kTitle{0.33f, 0.225f, 0.34f, 0.04f};
constexpr gfx::Rect kTabs{0.325f, 0.285f, 0.35f, 0.42f};
constexpr gfx::Rect kSave{0.40f, 0.735f, 0.11f, 0.05f};
constexpr gfx::Rect kClose{0.53f, 0.735f, 0.11f, 0.05f};

// Archetype dropdown order MUST match the ai::Archetype enum.
constexpr const char* kArchKeys[] = {"brute", "skirmisher", "caster", "swarm", "lurker"};
} // namespace

EntityInspector::EntityInspector(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

void EntityInspector::Open(const Config& cfg, const std::vector<std::string>& spellIds) {
	m_open = true;
	m_cfg = cfg;
	m_original = cfg;
	m_spellIds = spellIds;
	m_rebuild = false;
	BuildUI();
}

void EntityInspector::BuildUI() {
	m_ui.Clear();
	m_tabs = m_ui.Add<ui::TabControl>(kTabs, 0.09f);
	const size_t tabAi = m_tabs->AddTab(loc::Tr("map.insp.tab.ai"));

	// Placement flags.
	m_tabs->AddChild<ui::Checkbox>(tabAi, gfx::Rect{0.05f, 0.03f, 0.9f, 0.09f},
								   loc::Tr("map.insp.asleep"), m_cfg.asleep,
								   [this](bool on) {
									   m_cfg.asleep = on;
									   Apply();
								   });
	m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, 0.13f, 0.9f, 0.11f},
								 loc::Tr("map.insp.leash"), 0.0f, 12.0f, m_cfg.leashRange,
								 [this](float v) {
									 m_cfg.leashRange = v;
									 Apply();
								 });

	// Behaviour override (archetype + dependent params), mirroring the type dialog.
	std::vector<std::string> archItems;
	for (const char* k : kArchKeys) archItems.push_back(loc::Tr("archetype." + std::string(k)));
	m_tabs->AddChild<ui::Label>(tabAi, gfx::Rect{0.05f, 0.28f, 0.9f, 0.06f},
								loc::Tr("map.cfg.archetype"));
	m_tabs->AddChild<ui::DropDown>(tabAi, gfx::Rect{0.05f, 0.35f, 0.7f, 0.08f}, archItems,
								   static_cast<int>(m_cfg.archetype), [this](int i) {
									   m_cfg.archetype = static_cast<ai::Archetype>(i);
									   Apply();
									   m_rebuild = true; // dependent fields change
								   });
	float y = 0.47f;
	const bool kites = m_cfg.archetype == ai::Archetype::Skirmisher ||
					   m_cfg.archetype == ai::Archetype::Caster;
	if (kites) {
		m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, y, 0.9f, 0.11f},
									 loc::Tr("map.cfg.keeprange"), 1.0f, 10.0f, m_cfg.keepRange,
									 [this](float v) {
										 m_cfg.keepRange = v;
										 Apply();
									 });
		y += 0.14f;
	}
	m_tabs->AddChild<ui::Slider>(tabAi, gfx::Rect{0.05f, y, 0.9f, 0.11f},
								 loc::Tr("map.cfg.fleebelow"), 0.0f, 1.0f, m_cfg.fleeBelow,
								 [this](float v) {
									 m_cfg.fleeBelow = v;
									 Apply();
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
										   Apply();
									   });
	}

	m_ui.Add<ui::Button>(kSave, loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_cfg);
		Close();
	});
	m_ui.Add<ui::Button>(kClose, loc::Tr("map.cfg.close"), [this] {
		if (onApply) onApply(m_original); // revert the live monster to the snapshot
		Close();
	});
}

void EntityInspector::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

	if (input.WasKeyPressed(VK_ESCAPE)) { // cancel: revert live
		if (onApply) onApply(m_original);
		Close();
		return;
	}
	m_ui.Update(input, w, h);
	if (!m_open) return; // a footer button closed us this frame
	if (m_rebuild) {     // archetype changed which rows exist — rebuild off the stack
		m_rebuild = false;
		BuildUI();
	}
}

void EntityInspector::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							 float h) {
	if (!m_open) return;
	auto px = [&](const gfx::Rect& r) {
		return gfx::Rect{r.x * w, r.y * h, r.w * w, r.h * h};
	};
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel = px(kPanel);
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	const gfx::Rect title = px(kTitle);
	m_font.Draw(batch, loc::Format("map.insp.title", m_cfg.type), title.x, title.y, th.text);

	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
