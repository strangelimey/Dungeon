// ============================================================================
// Game/MonsterConfigDialog.cpp — see MonsterConfigDialog.h.
// ============================================================================
#include "Game/MonsterConfigDialog.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// The panel, as fractions (0..1) of the window; the card inside it is stacked
// (Game/DialogLayout.h) — tabs on the left, preview pane on the right.
constexpr gfx::Rect kPanel{0.14f, 0.10f, 0.72f, 0.80f};
constexpr float kTabsFill = 0.62f, kPaneFill = 0.38f, kGutterRow = 1.0f;

// Archetype option order MUST match the ai::Archetype enum (dropdown index -> enum).
constexpr const char* kArchKeys[] = {"brute",  "skirmisher", "caster",
									 "swarm", "lurker",     "sentry"};

std::string StateLabel(anim::CreatureState s) {
	return loc::Tr("anim.state." + std::string(anim::StateName(s)));
}
// The state a library clip belongs to (named <state>__<clip>): the token before
// "__", or the whole name if it is itself a state token, else empty.
std::string ClipState(const std::string& name) {
	const size_t sep = name.find("__");
	const std::string head = sep == std::string::npos ? name : name.substr(0, sep);
	return anim::ParseState(head) ? head : std::string();
}
// A clip name without its "<state>__" prefix, for display.
std::string ClipLabel(const std::string& name) {
	const size_t sep = name.find("__");
	return sep == std::string::npos ? name : name.substr(sep + 2);
}
} // namespace

MonsterConfigDialog::MonsterConfigDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void MonsterConfigDialog::Open(const std::string& type, const std::string& display,
							   const Support& supported, const Clips& clips,
							   ai::Archetype archetype, float keepRange, float fleeBelow,
							   const std::string& spell, const ThreatTuning& threat,
							   const std::vector<std::string>& modelClips,
							   const std::vector<std::string>& spellIds) {
	m_open = true;
	m_display = display;
	m_cfg = {type, supported, clips, archetype, keepRange, fleeBelow, spell, threat};
	m_original = m_cfg; // snapshot for revert
	m_modelClips = modelClips;
	m_spellIds = spellIds;
	m_selState = static_cast<int>(anim::CreatureState::Idle);
	m_selClip = FirstClipOf(m_selState); // auto-preview the first clip of the state
	m_activeTab = 0;
	m_rebuild = false;
	BuildUI();
}

bool MonsterConfigDialog::ClipBelongs(const std::string& name, int state) const {
	const std::string token(anim::StateName(static_cast<anim::CreatureState>(state)));
	if (ClipState(name) == token) return true;
	const auto& vec = m_cfg.clips[state]; // already-assigned clips stay visible/editable
	return std::find(vec.begin(), vec.end(), name) != vec.end();
}

std::string MonsterConfigDialog::FirstClipOf(int state) const {
	for (const std::string& c : m_modelClips)
		if (ClipBelongs(c, state)) return c;
	return {};
}

gfx::Rect MonsterConfigDialog::PreviewRect(float, float) const {
	// The pane widget's own rect, from the layout that just ran — one truth for
	// the backing it draws and the animation the owner blits over it.
	return m_pane ? m_pane->Pixel() : gfx::Rect{0.0f, 0.0f, 0.0f, 0.0f};
}

// --- widget tree ----------------------------------------------------------

void MonsterConfigDialog::BuildUI() {
	m_ui.Clear();
	m_pane = nullptr;
	DialogChrome chrome = BuildDialogChrome(
		m_ui, kPanel, loc::Format("map.cfg.title", m_display), m_closeIcon, [this] {
			if (onApply) onApply(m_original); // revert the live kind
			Close();
		});

	chrome.body->horizontal = true;
	m_tabs = chrome.body->Row<ui::TabControl>(ui::Len::Fill(kTabsFill), 0.075f);
	const size_t tabBehav = m_tabs->AddTab(loc::Tr("map.cfg.tab.behavior"));
	const size_t tabAnim = m_tabs->AddTab(loc::Tr("map.cfg.tab.animation"));
	BuildBehaviorTab(tabBehav);
	BuildAnimationTab(tabAnim);
	m_tabs->SetActiveTab(m_activeTab);

	// Preview column: header over the pane, so neither can sit on the other.
	chrome.body->Space(ui::Len::Fixed(kGutterRow));
	ui::Stack* pane = chrome.body->Row<ui::Stack>(ui::Len::Fill(kPaneFill));
	pane->debugName = "preview";
	pane->Row<ui::Label>(FormRow(0.8f), loc::Tr("map.cfg.preview"))->dim = true;
	m_pane = pane->Row<PreviewPane>(ui::Len::Fill());
	m_pane->border = true;
	// Cached once: selecting a clip does not rebuild the tree, so Update flips
	// the flag rather than re-translating the string every frame.
	m_pane->hint = loc::Tr("map.cfg.nopreview");

	chrome.footer->Space(ui::Len::Fill());
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.save"), [this] {
		if (onSave) onSave(m_cfg);
		Close();
	});
	chrome.footer->Space(ui::Len::Fill());
}

void MonsterConfigDialog::BuildBehaviorTab(size_t tab) {
	std::vector<std::string> archItems;
	for (const char* k : kArchKeys) archItems.push_back(loc::Tr("archetype." + std::string(k)));

	m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.05f, 0.04f, 0.9f, 0.06f},
								loc::Tr("map.cfg.archetype"));
	m_tabs->AddChild<ui::DropDown>(tab, gfx::Rect{0.05f, 0.11f, 0.62f, 0.08f}, archItems,
								   static_cast<int>(m_cfg.archetype), [this](int i) {
									   m_cfg.archetype = static_cast<ai::Archetype>(i);
									   Apply();
									   m_rebuild = true; // dependent fields change
								   });

	float y = 0.24f;
	const bool kites = m_cfg.archetype == ai::Archetype::Skirmisher ||
					   m_cfg.archetype == ai::Archetype::Caster;
	if (kites) {
		m_tabs->AddChild<ui::Slider>(tab, gfx::Rect{0.05f, y, 0.9f, 0.10f},
									 loc::Tr("map.cfg.keeprange"), 1.0f, 10.0f,
									 m_cfg.keepRange, [this](float v) {
										 m_cfg.keepRange = v;
										 Apply();
									 });
		y += 0.15f;
	}
	// Flee threshold applies to any archetype (0 = never).
	m_tabs->AddChild<ui::Slider>(tab, gfx::Rect{0.05f, y, 0.9f, 0.10f},
								 loc::Tr("map.cfg.fleebelow"), 0.0f, 1.0f, m_cfg.fleeBelow,
								 [this](float v) {
									 m_cfg.fleeBelow = v;
									 Apply();
								 });
	y += 0.17f;
	if (m_cfg.archetype == ai::Archetype::Caster) {
		m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.05f, y, 0.9f, 0.06f},
									loc::Tr("map.cfg.spell"));
		y += 0.07f;
		int sel = 0;
		for (size_t i = 0; i < m_spellIds.size(); ++i)
			if (m_spellIds[i] == m_cfg.spell) { sel = static_cast<int>(i); break; }
		std::vector<std::string> items = m_spellIds;
		if (items.empty()) items.push_back(loc::Tr("map.cfg.nospells"));
		m_tabs->AddChild<ui::DropDown>(tab, gfx::Rect{0.05f, y, 0.62f, 0.08f}, items, sel,
									   [this](int i) {
										   if (i >= 0 && i < static_cast<int>(m_spellIds.size()))
											   m_cfg.spell = m_spellIds[i];
										   Apply();
									   });
		y += 0.13f;
	}

	// Per-type THREAT multipliers (× the balance.cat globals; 1 = unchanged) —
	// this kind's targeting personality. Rows past the tab bottom scroll.
	m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.05f, y, 0.9f, 0.06f},
								loc::Tr("map.cfg.threat"));
	y += 0.08f;
	struct ThreatRow {
		const char* key;
		float ThreatTuning::*field;
	};
	static const ThreatRow kThreatRows[] = {
		{"map.cfg.threat_scale", &ThreatTuning::scale},
		{"map.cfg.threat_threshold", &ThreatTuning::threshold},
		{"map.cfg.threat_switch", &ThreatTuning::switchMargin},
		{"map.cfg.threat_decay", &ThreatTuning::decay},
	};
	for (const ThreatRow& r : kThreatRows) {
		m_tabs->AddChild<ui::Slider>(
			tab, gfx::Rect{0.05f, y, 0.9f, 0.10f}, loc::Tr(r.key), 0.0f, 3.0f,
			m_cfg.threat.*(r.field), [this, f = r.field](float v) {
				m_cfg.threat.*f = v;
				Apply();
			});
		y += 0.135f;
	}
}

void MonsterConfigDialog::BuildAnimationTab(size_t tab) {
	constexpr float rowH = 0.062f, top = 0.09f;
	m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.02f, 0.01f, 0.32f, 0.06f},
								loc::Tr("map.cfg.states"));
	m_tabs->AddChild<ui::Label>(
		tab, gfx::Rect{0.37f, 0.01f, 0.6f, 0.06f},
		loc::Format("map.cfg.anims",
					StateLabel(static_cast<anim::CreatureState>(m_selState))));

	// State column: a Button spans the row (click = pick this state's clips), with a
	// Checkbox box at the right for its supported flag (added after, so it wins its
	// area). Idle is the rest floor — always supported, no toggle.
	for (int i = 0; i < N; ++i) {
		const auto s = static_cast<anim::CreatureState>(i);
		const float y = top + i * rowH;
		auto* btn = m_tabs->AddChild<ui::Button>(
			tab, gfx::Rect{0.02f, y, 0.24f, rowH * 0.9f}, StateLabel(s), [this, i] {
				if (m_selState != i) {
					m_selState = i;
					m_selClip = FirstClipOf(i);
					m_rebuild = true;
				}
			});
		btn->active = (i == m_selState);
		if (i != static_cast<int>(anim::CreatureState::Idle)) {
			m_tabs->AddChild<ui::Checkbox>(tab, gfx::Rect{0.27f, y, 0.06f, rowH * 0.9f}, "",
										   m_cfg.supported[i], [this, i](bool on) {
											   m_cfg.supported[i] = on;
											   Apply();
										   });
		}
	}

	// Clip column for the selected state (name-encoded or already assigned). Rows
	// past the tab bottom scroll (TabControl handles it). Empty = a hint label.
	std::vector<int> clips;
	for (int i = 0; i < static_cast<int>(m_modelClips.size()); ++i)
		if (ClipBelongs(m_modelClips[i], m_selState)) clips.push_back(i);
	if (clips.empty()) {
		m_tabs->AddChild<ui::Label>(tab, gfx::Rect{0.37f, top, 0.6f, rowH},
									loc::Tr("map.cfg.noclips"));
		return;
	}
	const auto& vec = m_cfg.clips[m_selState];
	for (size_t r = 0; r < clips.size(); ++r) {
		const std::string name = m_modelClips[clips[r]]; // full <state>__<clip>
		const float y = top + r * rowH;
		auto* btn = m_tabs->AddChild<ui::Button>(
			tab, gfx::Rect{0.37f, y, 0.46f, rowH * 0.9f}, ClipLabel(name), [this, name] {
				m_selClip = name; // select for preview
			});
		btn->active = (name == m_selClip);
		const bool on = std::find(vec.begin(), vec.end(), name) != vec.end();
		m_tabs->AddChild<ui::Checkbox>(tab, gfx::Rect{0.85f, y, 0.06f, rowH * 0.9f}, "", on,
									   [this, name](bool checked) {
										   auto& v = m_cfg.clips[m_selState];
										   const auto it = std::find(v.begin(), v.end(), name);
										   if (checked && it == v.end())
											   v.push_back(name);
										   else if (!checked && it != v.end())
											   v.erase(it);
										   m_selClip = name; // toggling also previews it
										   Apply();
									   });
	}
}

// --- modal loop -----------------------------------------------------------

void MonsterConfigDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	// One font now: the title text used to be a second Font at the very same
	// size as this context's. GameUI::UpdateFonts commits every library font
	// once per frame, so there is nothing to flush here either.
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	if (input.WasKeyPressed(VK_ESCAPE)) { // cancel: revert live to the snapshot
		if (onApply) onApply(m_original);
		Close();
		return;
	}

	m_ui.Update(input, w, h);
	if (!m_open) return; // a footer button (Save/Close) closed us this frame
	if (m_pane) m_pane->showHint = m_selClip.empty();
	if (m_tabs) m_activeTab = m_tabs->ActiveTab();
	if (m_rebuild) { // a callback changed which rows exist — rebuild off the stack
		m_rebuild = false;
		BuildUI();
	}
}

void MonsterConfigDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
								 float h) {
	if (!m_open) return;
	auto px = [&](const gfx::Rect& r) {
		return gfx::Rect{r.x * w, r.y * h, r.w * w, r.h * h};
	};

	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	const gfx::Rect panel = px(kPanel);
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	// Title, tabs, preview pane and footer are all widgets; the owner (Game)
	// blits the live looping animation into PreviewRect on top afterwards.
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
