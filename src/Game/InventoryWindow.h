// ============================================================================
// Game/InventoryWindow.h — combined party inventory (one backpack column per member).
//
// A FLOATING WINDOW (ui-panels P3b): the content of a ui::FloatingPanel on the
// HUD's floating layer, which places it, moves it and scales it like every
// other HUD panel (the panel shows only while the window is open). It fills
// that panel and lays itself out in fractions of its own rect.
//
// It used to be a modal overlay - a screen-wide dim, centred on the window, a
// click anywhere outside it closing it. Grimrock's inventory floats over the
// running game instead, so this one does too: no dim, the world clickable
// around it, and closed by its corner box (or Esc, which Game routes here
// first).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dungeon::game {

class InventoryWindow : public ui::Widget {
public:
	// The window's size at scale 1, as fractions of the game window (the
	// floating panel's size function multiplies them out).
	static constexpr float kWidthFrac = 0.72f;
	static constexpr float kHeightFrac = 0.54f;

	// `closeIcon` is the shared corner box (AssetUtil CloseIcon); `onClose`
	// runs when it is clicked.
	InventoryWindow(std::vector<Character>* roster, const ItemIconBank* icons,
					HeldItem* held, const gfx::Texture* closeIcon,
					std::function<void()> onClose);

	void Open() { m_open = true; }
	void Close() { m_open = false; }
	bool IsOpen() const { return m_open; }

	// The item mouse buttons, as on the sheet (docs/ui-updates-plan.md P2):
	// RIGHT on a non-empty slot = its details, MIDDLE = its use menu. `slot`
	// indexes member `member`'s selected pack.
	std::function<void(size_t member, int slot)> onItemDetails;
	std::function<void(size_t member, int slot)> onItemUse;

	// The background's opacity, read live (Settings -> UI). Null = opaque.
	const float* opacity = nullptr;

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	int MemberCount() const;
	gfx::Rect SlotRect(const gfx::Rect& panel, int member, int slot) const;

	std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	HeldItem* m_held;
	bool m_open = false;
	std::string m_title; // localized once at construction
};

} // namespace dungeon::game
