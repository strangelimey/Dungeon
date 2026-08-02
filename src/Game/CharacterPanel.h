// ============================================================================
// Game/CharacterPanel.h — one slot of the top party bar (portrait, name, resource bars, effect strip).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <functional>
#include <vector>

namespace dungeon::game {

class CharacterPanel : public ui::Widget {
public:
	// portraitFont draws the big placeholder initial (the Game passes its
	// title font, which tracks the window scale like everything else).
	// onClick fires on a left click on the PORTRAIT area (open the sheet / place a
	// held tablet); onRight on a right click there (open this member's inventory).
	// onBars fires on EITHER button over the stat-bar area (open the Stats tab).
	// `icons` (Game-owned, may be null) supplies the status-effect strip's
	// icon art (a ward draws the Protect rune tablet's icon). onEffects fires
	// on a left click on one of the strip's icons (open the sheet's Effects
	// tab — the icon's long form); it wins over onClick for that spot.
	CharacterPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   size_t member,
				   const ui::Font* portraitFont, const ResourceBarColors* barColors,
				   const HitSplatIcons* hitSplats, const ItemIconBank* icons,
				   std::function<void()> onClick,
				   std::function<void()> onRight, std::function<void()> onBars,
				   std::function<void()> onEffects);

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	// The stat-bar sub-region (right of the portrait, below the name) in pixels —
	// kept in sync with Draw's bar layout; a click here opens the Stats tab.
	gfx::Rect BarsRect(ui::UIContext& ctx) const;
	// The portrait square (left of the panel), and the Nth status-effect icon
	// — a row in the NAME band, right-aligned and growing right-to-left (many
	// effects stack; spill past the name is a later problem). One layout,
	// hit-tested by Update (hover names the effect) and drawn by Draw.
	gfx::Rect PortraitRect() const;
	gfx::Rect EffectIconRect(ui::UIContext& ctx, size_t index) const;

	const std::vector<Character>* m_roster;
	size_t m_member;
	// Resolved from (m_roster, m_member) at the top of every Update/Draw —
	// never valid across frames, so a roster resize can't dangle it.
	const Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const HitSplatIcons* m_hitSplats; // may be null (icons not loaded)
	const ItemIconBank* m_icons;      // may be null (effect icons skip art)
	// Index into the member's effects list of the icon under the cursor
	// (size_t(-1) = none) — set by Update, read by Draw for the hover label.
	size_t m_hotEffect = static_cast<size_t>(-1);
	std::function<void()> m_onClick;
	std::function<void()> m_onRight;
	std::function<void()> m_onBars;
	std::function<void()> m_onEffects;
	bool m_hot = false;
	bool m_held = false;      // left-button press latched on this panel
	bool m_heldRight = false; // right-button press latched on this panel
};
} // namespace dungeon::game
