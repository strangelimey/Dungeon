// ============================================================================
// Game/DoorInspector.cpp — see DoorInspector.h.
// ============================================================================
#include "Game/DoorInspector.h"

#include "Core/Easing.h" // kEaseShapeNames (the shape dropdowns)
#include "Core/Loc.h"
#include "Game/DialogLayout.h" // FormRow, TabStack
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

// One label plus a side-by-side [ease in] [ease out] pair. Row 0 of each
// dropdown is the inherit-from-type default, so the three-state rule the opener
// uses holds here too: empty inherits, a name overrides.
void DoorInspector::EaseRow(ui::Stack& page, const std::string& label,
							std::string& in, std::string& out) {
	page.Row<ui::Label>(FormRow(), label);
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
	// motion; stacked, they read as two unrelated settings. A HORIZONTAL Stack
	// rather than two half-width rects: the pair splits whatever the page is
	// wide, so neither end has to know the other's fraction.
	ui::Stack* pair = page.Row<ui::Stack>(FormRow(), gfx::Rect{0, 0, 1, 1}, true);
	pair->gapRem = 0.4f;
	pair->Row<ui::DropDown>(ui::Len::Fill(), names, indexOf(in),
							[this, &in](int i) {
								in = i <= 0 ? std::string() : kEaseShapeNames[i - 1];
								if (onApply) onApply(m_cfg);
							});
	pair->Row<ui::DropDown>(ui::Len::Fill(), names, indexOf(out),
							[this, &out](int i) {
								out = i <= 0 ? std::string() : kEaseShapeNames[i - 1];
								if (onApply) onApply(m_cfg);
							});
}

void DoorInspector::BuildContent(ui::Stack& c) {
	// GROUPED INTO TABS, like the type dialog. The instance grew from three
	// settings to eight and a single column could not hold them: first the last
	// rows fell past the footer, then scrolling them turned the dialog into a
	// ribbon you had to hunt through. The pages split it where the seams already
	// are — what the door IS, how it MOVES, and the thing that WORKS it are
	// three objects, which is the division the catalog itself makes with
	// motion/travel/open_seconds/ease_* sitting apart from key and name.
	//
	// Each page is a content-sized Stack (DialogLayout's TabStack), so a page is
	// as long as its rows and scrolls if it outgrows the card. Nothing here
	// writes a coordinate.
	ui::TabControl* tabs = c.Row<ui::TabControl>(ui::Len::Fill(), 0.09f);
	const std::size_t tDoor = tabs->AddTab(loc::Tr("map.door.tab_door"));
	const std::size_t tMotion = tabs->AddTab(loc::Tr("map.door.tab_motion"));
	const std::size_t tOpener = tabs->AddTab(loc::Tr("map.door.tab_opener"));
	ui::Stack& door = *TabStack(*tabs, tDoor);
	ui::Stack& motion = *TabStack(*tabs, tMotion);
	ui::Stack& opener = *TabStack(*tabs, tOpener);

	// --- the door itself -----------------------------------------------------
	// Open flips the live leaf (it animates) and the record's authored state.
	door.Row<ui::Checkbox>(FormRow(), loc::Tr("map.door.open"), m_cfg.open,
						   [this](bool on) {
							   m_cfg.open = on;
							   if (onApply) onApply(m_cfg);
						   });

	// Required key: "None" + every items.cat entry with category=key. Selecting
	// one authors key=<id> on the record — the party's click then opens the
	// door only while a member carries the item; wired buttons ignore locks.
	door.Row<ui::Label>(FormRow(), loc::Tr("map.door.key"));
	std::vector<std::string> names;
	names.push_back(loc::Tr("map.door.nokey"));
	int sel = 0;
	for (size_t i = 0; i < m_keys.size(); ++i) {
		names.push_back(m_keys[i].second);
		if (m_keys[i].first == m_cfg.key) sel = static_cast<int>(i) + 1;
	}
	door.Row<ui::DropDown>(FormRow(), names, sel, [this](int i) {
		m_cfg.key =
			i <= 0 ? std::string() : m_keys[static_cast<size_t>(i) - 1].first;
		if (onApply) onApply(m_cfg);
	});

	// Name: what a button's target= points at. Kept record-safe as it is typed
	// (records are whitespace-tokenised key=value lines, so spaces/'=' would
	// corrupt the .ent — the filter drops anything outside [A-Za-z0-9_-]).
	door.Row<ui::Label>(FormRow(), loc::Tr("map.door.name"));
	ui::TextField* name = door.Row<ui::TextField>(FormRow(), m_cfg.name);
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

	// --- how it moves --------------------------------------------------------
	// A Slider is SELF-CONTAINED — it draws its own label above its track — so
	// this row needs no Label of its own and asks for TWO lines' worth of room,
	// unlike every other control here.
	//
	// The range runs from a door that snaps to one that groans: the shipped
	// types sit at 0.8, 1.4 and 2.0, so a ceiling of 4 leaves room to make a
	// vault door genuinely slow without the useful half of the track being a
	// sliver. It shows the EFFECTIVE seconds and starts at the type's value —
	// see the Config comment for why this one control is not three-state.
	motion.Row<ui::Slider>(FormRow(2.0f), loc::Tr("map.door.speed"), 0.2f, 4.0f,
						   m_cfg.seconds, [this](float v) {
							   m_cfg.seconds = v;
							   if (onApply) onApply(m_cfg);
						   });
	EaseRow(motion, loc::Tr("map.door.ease"), m_cfg.easeIn, m_cfg.easeOut);

	// --- the hand-hold -------------------------------------------------------
	// Row 0 is "Default (<what the type gives>)" — the empty override — so an
	// instance can go back to inheriting after being changed. Row 1 is an
	// explicit None, which is NOT the same thing: it overrides a type that has
	// an opener, leaving this door button-only.
	opener.Row<ui::Label>(FormRow(), loc::Tr("map.door.opener"));
	std::vector<std::string> openers;
	openers.push_back(loc::Format("map.door.opener_default", m_typeOpener));
	openers.push_back(loc::Tr("map.door.opener_none"));
	int osel = 0;
	if (m_cfg.opener == "none") osel = 1;
	for (size_t i = 0; i < m_openers.size(); ++i) {
		openers.push_back(m_openers[i].second);
		if (m_openers[i].first == m_cfg.opener) osel = static_cast<int>(i) + 2;
	}
	opener.Row<ui::DropDown>(FormRow(), openers, osel, [this](int i) {
		m_cfg.opener = i <= 0   ? std::string()
					   : i == 1 ? std::string("none")
								: m_openers[static_cast<size_t>(i) - 2].first;
		if (onApply) onApply(m_cfg);
	});

	// Which jamb it hangs on. Either is free — the leaf's mortice runs inside
	// the wall, not across its face.
	opener.Row<ui::Label>(FormRow(), loc::Tr("map.door.openerside"));
	const std::vector<std::string> sides{
		loc::Format("map.door.opener_default", m_typeSide),
		loc::Tr("map.door.side_left"), loc::Tr("map.door.side_right")};
	const int ssel = m_cfg.openerSide == "left"    ? 1
					 : m_cfg.openerSide == "right" ? 2
												   : 0;
	opener.Row<ui::DropDown>(FormRow(), sides, ssel, [this](int i) {
		m_cfg.openerSide = i == 1 ? "left" : i == 2 ? "right" : "";
		if (onApply) onApply(m_cfg);
	});

	EaseRow(opener, loc::Tr("map.door.openerease"), m_cfg.openerEaseIn,
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
