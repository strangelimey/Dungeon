// ============================================================================
// Game/DoorInspector.h — the editor's per-INSTANCE door editor.
//
// A concrete InstanceInspector (see that header). A door's orientation is the
// doorway's own (auto-detected at placement), so there is no Facing row. The
// body edits the door's AUTHORED state across three pages:
//
//   Door    Open (the leaf moves live as the checkbox flips, and the record's
//           open= param follows), the key item it requires (items.cat entries
//           with category=key; a keyed door opens to the party's click only
//           while a member carries the item — see DungeonWorld::ToggleDoorAhead
//           — while wired buttons bypass the lock), and the name a button's
//           target= points at.
//   Motion  how long the throw takes and the two curves that shape it.
//   Opener  which hand-hold hangs on which jamb, and its own two curves.
//
// Save persists the level; Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"
#include "UI/Controls.h" // ui::TabControl (the pages EaseRow appends to)
#include "UI/Layout.h"   // ui::Stack (a tab page's rows)

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class DoorInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		bool open = false;   // authored initial state (the live door follows)
		std::string key;     // items.cat id required to open by hand ("" = none)
		std::string name;    // button-target id ("" = unwired); record-safe chars
		// The opener OVERRIDES, three-state: "" inherits the door type, "none"
		// is this placement having no hand-hold whatever the type says, and an
		// id picks one. Same for the side ("" / "left" / "right"). The dialog's
		// first row names what the type would give, so inheriting stays
		// expressible after an edit.
		std::string opener;
		std::string openerSide;
		// Motion shaping, same three-state rule. Two pairs because the leaf and
		// the hand-hold are two separate motions.
		std::string easeIn, easeOut;
		std::string openerEaseIn, openerEaseOut;
		// The leaf's throw time, and the one field here that is NOT three-state.
		// The slider always shows the EFFECTIVE number, starting at whatever the
		// type gives, and the caller writes an override only where it DIFFERS
		// (Game_Wiring's onApply). A slider cannot say "inherit" and a value at
		// once — its label is fixed at construction — and both alternatives (a
		// checkbox to arm it, or a minimum position meaning Default) cost a
		// control or a lie for a distinction nothing can see. `typeSeconds` is
		// what the type gives, so that comparison can be made. What it gives up
		// is pinning an instance to a number its type happens to share, which
		// only ever differs if the type is later retuned.
		float seconds = 0.7f;
		float typeSeconds = 0.7f;
	};

	DoorInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `keys` are the selectable key items as (id, display) pairs; the dialog
	// prepends the "None" row itself. `openers` are the door catalog's opener
	// entries the same way. `typeOpener` and `typeSide` are what the door's TYPE
	// would supply, already display-ready — they are what the "Default (...)"
	// rows name, and a Default row that named the wrong thing would be worse
	// than one that named nothing.
	void Open(const Config& cfg, std::vector<std::pair<std::string, std::string>> keys,
			  std::vector<std::pair<std::string, std::string>> openers,
			  std::string typeOpener, std::string typeSide,
			  PreviewSpec preview = {});

	// Push the working state to the live door + its .ent record (both edits are
	// in-memory until savemap, like every other editor edit).
	std::function<void(const Config&)> onApply;
	std::function<void()> onSave; // persist the level (.ent)

protected:
	std::string Title() const override;
	// WIDER than the other inspectors, and grouped into tabs rather than grown
	// taller. A door instance now carries eight settings; stacked in one column
	// beside the preview pane they were a scrolling ribbon of half-width
	// dropdowns, which is busy however tall the panel gets.
	gfx::Rect Panel() const override { return {0.22f, 0.13f, 0.56f, 0.70f}; }
	// No facing row: the doorway's flanking walls fix the orientation.
	std::vector<Direction> FacingChoices() const override { return {}; }
	void BuildContent(ui::Stack& content) override;
	// One label plus a side-by-side [ease in] [ease out] pair, appended to a
	// tab's page. The page is a Stack, so this adds rows and says how much room
	// each needs — it does not place them.
	void EaseRow(ui::Stack& page, const std::string& label, std::string& in,
				 std::string& out);
	void ApplyLive() override {} // no common-strip edits (no facing)
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::pair<std::string, std::string>> m_keys;    // (id, display)
	std::vector<std::pair<std::string, std::string>> m_openers; // (id, display)
	std::string m_typeOpener, m_typeSide; // what the TYPE gives, for the Default rows
};

} // namespace dungeon::game
