// ============================================================================
// Game/DoorInspector.cpp — see DoorInspector.h.
// ============================================================================
#include "Game/DoorInspector.h"

#include "Core/Easing.h" // kEaseShapeNames (the shape dropdowns)
#include "Core/Loc.h"
#include "UI/Controls.h"

#include <cctype>

namespace dungeon::game {

void DoorInspector::Open(const Config& cfg,
						 std::vector<std::pair<std::string, std::string>> keys,
						 std::vector<std::pair<std::string, std::string>> openers,
						 std::string typeOpener, std::string typeSide,
						 PreviewSpec preview) {
	m_cfg = cfg;
	m_original = cfg;
	m_keys = std::move(keys);
	m_openers = std::move(openers);
	m_typeOpener = std::move(typeOpener);
	m_typeSide = std::move(typeSide);
	SetPreview(std::move(preview));
	OpenModal();
}

std::string DoorInspector::Title() const {
	return loc::Format("map.door.title", m_cfg.x, m_cfg.z);
}

// Row metrics, in fractions of one TAB PAGE. Each tab holds three or four
// settings, which is what grouping bought: no page needs to scroll.
//
// A LABEL'S BOX HAS AIR IN IT, and there is a gap before the control it names.
// At kDialogTextScale the dialog font is large, so a label box sized to look
// generous still had its text sitting on the box beneath. Reserve the margin
// explicitly instead of hoping the label's own height provides it.
namespace {
constexpr float kLabelH = 0.105f;
constexpr float kLabelGap = 0.022f; // between a label and the control it names
constexpr float kCtlH = 0.105f;
constexpr float kGap = 0.055f;      // between one setting and the next
} // namespace

// One label plus a side-by-side [ease in] [ease out] pair. Row 0 of each
// dropdown is the inherit-from-type default, so the three-state rule the opener
// uses holds here too: empty inherits, a name overrides.
void DoorInspector::EaseRow(ui::TabControl& tabs, std::size_t tab, float& y,
							const std::string& label, std::string& in,
							std::string& out) {
	tabs.AddChild<ui::Label>(tab, gfx::Rect{0.0f, y, 1.0f, kLabelH}, label);
	y += kLabelH + kLabelGap;
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.door.ease_default"));
	for (int i = 0; i < kEaseShapeCount; ++i)
		names.push_back(loc::Tr(std::string("ease.shape.") + kEaseShapeNames[i]));
	auto indexOf = [](const std::string& v) {
		for (int i = 0; i < kEaseShapeCount; ++i)
			if (v == kEaseShapeNames[i]) return i + 1;
		return 0;
	};
	// The two ends sit side by side because they are ONE decision about one
	// motion; stacked, they read as two unrelated settings.
	constexpr float half = 0.485f;
	tabs.AddChild<ui::DropDown>(tab, gfx::Rect{0.0f, y, half, kCtlH}, names,
								indexOf(in), [this, &in](int i) {
									in = i <= 0 ? std::string()
												: kEaseShapeNames[i - 1];
									if (onApply) onApply(m_cfg);
								});
	tabs.AddChild<ui::DropDown>(tab, gfx::Rect{1.0f - half, y, half, kCtlH}, names,
								indexOf(out), [this, &out](int i) {
									out = i <= 0 ? std::string()
												 : kEaseShapeNames[i - 1];
									if (onApply) onApply(m_cfg);
								});
	y += kCtlH + kGap;
}

void DoorInspector::BuildContent(const gfx::Rect& c) {
	// GROUPED INTO TABS, like the type dialog. The instance grew from three
	// settings to seven and a single column could not hold them: first the last
	// rows fell past the footer, then scrolling them turned the dialog into a
	// ribbon you had to hunt through. Two pages split it where the seam already
	// is — the DOOR and the thing that WORKS it are separate objects with
	// separate motions, which is the same division the catalog makes.
	//
	// Children are fractions of a tab page (a ScrollArea, so an overfull page
	// would scroll rather than clip — but neither does).
	ui::TabControl* tabs = UI().Add<ui::TabControl>(c, 0.09f);
	const std::size_t tDoor = tabs->AddTab(loc::Tr("map.door.tab_door"));
	const std::size_t tOpener = tabs->AddTab(loc::Tr("map.door.tab_opener"));

	// --- the door itself -----------------------------------------------------
	float y = 0.02f;
	// Open flips the live leaf (it animates) and the record's authored state.
	tabs->AddChild<ui::Checkbox>(tDoor, gfx::Rect{0.0f, y, 1.0f, kCtlH},
								 loc::Tr("map.door.open"), m_cfg.open,
								 [this](bool on) {
									 m_cfg.open = on;
									 if (onApply) onApply(m_cfg);
								 });
	y += kCtlH + kGap;

	// Required key: "None" + every items.cat entry with category=key. Selecting
	// one authors key=<id> on the record — the party's click then opens the
	// door only while a member carries the item; wired buttons ignore locks.
	tabs->AddChild<ui::Label>(tDoor, gfx::Rect{0.0f, y, 1.0f, kLabelH},
							  loc::Tr("map.door.key"));
	y += kLabelH + kLabelGap;
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.door.nokey"));
	int sel = 0;
	for (size_t i = 0; i < m_keys.size(); ++i) {
		names.push_back(m_keys[i].second);
		if (m_keys[i].first == m_cfg.key) sel = static_cast<int>(i) + 1;
	}
	tabs->AddChild<ui::DropDown>(tDoor, gfx::Rect{0.0f, y, 1.0f, kCtlH}, names, sel,
								 [this](int i) {
									 m_cfg.key =
										 i <= 0 ? std::string()
												: m_keys[static_cast<size_t>(i) - 1].first;
									 if (onApply) onApply(m_cfg);
								 });
	y += kCtlH + kGap;

	EaseRow(*tabs, tDoor, y, loc::Tr("map.door.ease"), m_cfg.easeIn, m_cfg.easeOut);

	// Name: what a button's target= points at. Kept record-safe as it is typed
	// (records are whitespace-tokenised key=value lines, so spaces/'=' would
	// corrupt the .ent — the filter drops anything outside [A-Za-z0-9_-]).
	tabs->AddChild<ui::Label>(tDoor, gfx::Rect{0.0f, y, 1.0f, kLabelH},
							  loc::Tr("map.door.name"));
	y += kLabelH + kLabelGap;
	ui::TextField* name = tabs->AddChild<ui::TextField>(
		tDoor, gfx::Rect{0.0f, y, 1.0f, kCtlH}, m_cfg.name);
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

	// --- the hand-hold -------------------------------------------------------
	y = 0.02f;
	// Row 0 is "Default (<what the type gives>)" — the empty override — so an
	// instance can go back to inheriting after being changed. Row 1 is an
	// explicit None, which is NOT the same thing: it overrides a type that has
	// an opener, leaving this door button-only.
	tabs->AddChild<ui::Label>(tOpener, gfx::Rect{0.0f, y, 1.0f, kLabelH},
							  loc::Tr("map.door.opener"));
	y += kLabelH + kLabelGap;
	std::vector<std::string> openers;
	openers.push_back(loc::Format("map.door.opener_default", m_typeOpener));
	openers.push_back(loc::Tr("map.door.opener_none"));
	int osel = 0;
	if (m_cfg.opener == "none") osel = 1;
	for (size_t i = 0; i < m_openers.size(); ++i) {
		openers.push_back(m_openers[i].second);
		if (m_openers[i].first == m_cfg.opener) osel = static_cast<int>(i) + 2;
	}
	tabs->AddChild<ui::DropDown>(tOpener, gfx::Rect{0.0f, y, 1.0f, kCtlH}, openers,
								 osel, [this](int i) {
									 m_cfg.opener =
										 i <= 0   ? std::string()
										 : i == 1 ? std::string("none")
												  : m_openers[static_cast<size_t>(i) - 2].first;
									 if (onApply) onApply(m_cfg);
								 });
	y += kCtlH + kGap;

	// Which jamb it hangs on. Either is free — the leaf's mortice runs inside
	// the wall, not across its face.
	tabs->AddChild<ui::Label>(tOpener, gfx::Rect{0.0f, y, 1.0f, kLabelH},
							  loc::Tr("map.door.openerside"));
	y += kLabelH + kLabelGap;
	const std::vector<std::string> sides{
		loc::Format("map.door.opener_default", m_typeSide),
		loc::Tr("map.door.side_left"), loc::Tr("map.door.side_right")};
	const int ssel = m_cfg.openerSide == "left"    ? 1
					 : m_cfg.openerSide == "right" ? 2
												   : 0;
	tabs->AddChild<ui::DropDown>(tOpener, gfx::Rect{0.0f, y, 1.0f, kCtlH}, sides,
								 ssel, [this](int i) {
									 m_cfg.openerSide = i == 1   ? "left"
														: i == 2 ? "right"
																 : "";
									 if (onApply) onApply(m_cfg);
								 });
	y += kCtlH + kGap;

	EaseRow(*tabs, tOpener, y, loc::Tr("map.door.openerease"), m_cfg.openerEaseIn,
			m_cfg.openerEaseOut);
}

void DoorInspector::Persist() {
	if (onApply) onApply(m_cfg);
	if (onSave) onSave();
}

void DoorInspector::Revert() {
	if (onApply) onApply(m_original);
}

} // namespace dungeon::game
