// ============================================================================
// Game/InventoryWindow.h — combined party inventory overlay (one backpack column per member).
//
// SCREEN-ANCHORED, not parent-relative: it centres itself on the window and
// draws in the OVERLAY pass, so `bounds` stays zero and its rect comes from
// PanelRect(ctx). That is deliberate for a floating panel — it must not be
// clipped or positioned by whatever happens to own it — but it means the tree
// inspector reports it as 0x0. See docs/ui-hierarchy.md.
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

} // namespace dungeon::game
