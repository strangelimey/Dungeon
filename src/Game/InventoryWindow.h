// ============================================================================
// Game/InventoryWindow.h — combined party inventory overlay (one backpack column per member).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <optional>
#include <string>
#include <vector>

namespace dungeon::game {

class InventoryWindow : public ui::Widget {
public:
	InventoryWindow(std::vector<Character>* roster, const ItemIconBank* icons,
					std::optional<std::string>* held);

	void Open() { m_open = true; }
	void Close() { m_open = false; }
	bool IsOpen() const { return m_open; }

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext&, gfx::SpriteBatch&) override {} // overlay-only
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	int MemberCount() const;
	gfx::Rect PanelRect(const ui::UIContext& ctx) const;
	gfx::Rect SlotRect(const gfx::Rect& panel, int member, int slot) const;

	std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	std::optional<std::string>* m_held;
	bool m_open = false;
	std::string m_title; // localized once at construction
};

// The character sheet. A fixed header (portrait, name, the health/stamina/mana
// bars) tops three switchable modes, toggled by the small icon buttons under
// the portrait (the active mode's button draws "pressed"):
//   * Inventory — the worn-equipment paper doll + the (dynamic) backpack grid.
//     Held-aware: a tablet carried on the cursor drops into an equipment or
//     backpack slot (swapping any occupant onto the cursor); empty-handed, a
//     click picks the slot's item up.
//   * Stats     — the member's attributes.
//   * Skills    — placeholder until a skill system exists.
// icons resolves item art; held is Game's cursor item (the overlay cursor is
// drawn by GameUI). The sheet is frozen-state, so it edits its member live
// (mutable pointer).
} // namespace dungeon::game
