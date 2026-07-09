// ============================================================================
// Game/BalanceDialog.h — the editor's combat-tuning modal (docs/combat.md).
//
// Opened by the editor map's Balance header button. A centered panel over the
// editor, TABBED like the monster-config dialog (ui::TabControl):
//   • Formula — one numeric field per knob in the sheet (rows generated from
//     BalanceFields(), so a new knob in Balance.h appears here for free).
//   • Attacks — one row per attack in the closed attack table: its damage
//     type (read-only — identity is C++) and the three numeric fields
//     (damage ×, accuracy +, speed ×) attacks.cat carries.
// Row labels are the RAW catalog keys (unarmed_base, stat_damage, ...) on
// purpose: the dialog is a front-end for balance.cat/attacks.cat, and showing
// the data names keeps it WYSIWYG with the files a hand-editor sees.
//
// The dialog edits an in-memory working copy of the whole Balance and fires
// callbacks; the owner (Game) applies it live (onApply, every valid edit —
// swings pace, resists, and the derived resource maxima all move immediately)
// and persists it (onSave, the Save button → balance.cat + attacks.cat).
// Close/Esc reverts via onApply(original).
// ============================================================================
#pragma once

#include "Game/Balance.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>

namespace dungeon::ui {
class TabControl;
}

namespace dungeon::game {

class BalanceDialog {
public:
	explicit BalanceDialog(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	// Opens on a snapshot of the live tuning (the working copy starts there).
	void Open(const Balance& current);
	void Close() { m_open = false; }

	// Modal input: routes to the widget tree and handles Esc (revert + close).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Push the working copy live (every valid edit; the original on a revert).
	// onSave persists it to the project catalogs (the Save button).
	std::function<void(const Balance&)> onApply;
	std::function<void(const Balance&)> onSave;

private:
	void BuildUI();
	void BuildFormulaTab(size_t tab);
	void BuildAttacksTab(size_t tab);
	void Apply() { if (onApply) onApply(m_cfg); }

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;    // the dialog's own text (title)
	ui::UIContext m_ui; // tabs + numeric fields + footer buttons

	bool m_open = false;
	// The Attacks tab's "?" overlay: what the three columns mean (localized,
	// word-wrapped at draw time). Any click or Esc closes it; while it is up
	// the dialog beneath is frozen.
	bool m_helpOpen = false;
	Balance m_cfg;      // working copy
	Balance m_original; // snapshot for revert on Close/Esc

	ui::TabControl* m_tabs = nullptr; // owned by m_ui; kept to restore the tab
	int m_activeTab = 0;
};

} // namespace dungeon::game
