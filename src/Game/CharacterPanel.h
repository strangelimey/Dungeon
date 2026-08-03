// ============================================================================
// Game/CharacterPanel.h — one slot of the top party bar, and its parts.
//
// The slot is a container (docs/ui-hierarchy.md): portrait, effect strip and
// stat bars are CHILD widgets, each owning its own hover and click, so the
// panel itself paints only the frame and the name. Their bounds are assigned
// every layout by CharacterPanel::LayoutSelf rather than authored as constants,
// because they are aspect- or font-locked: the portrait is a square sized by
// the slot's HEIGHT (so its width fraction depends on the slot's aspect, which
// the party-bar scale slider changes), and the effect icons and the bar band
// are sized from the font's line advance. Computed bounds are still
// parent-relative — each child multiplies out against this panel — the panel
// just works the fractions out per frame instead of at build time.
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <functional>
#include <vector>

namespace dungeon::game {

// The portrait square: the member's bust, plus the transient hit splat over it.
// Left click opens the sheet / places a held tablet, right click the backpack.
class PortraitBox : public ui::Widget {
public:
	PortraitBox(const std::vector<Character>* roster, size_t member,
				const HitSplatIcons* hitSplats, std::function<void()> onClick,
				std::function<void()> onRight);

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	const HitSplatIcons* m_hitSplats; // may be null (icons not loaded)
	std::function<void()> m_onClick;
	std::function<void()> m_onRight;
	bool m_held = false;
	bool m_heldRight = false;
};

// One status effect in the name band: the kind's icon under a school-tinted
// border, with a depleting time sliver. Hovering names it on a plaque under the
// panel (drawn in the OVERLAY pass, so it floats over whatever is beneath);
// clicking opens the sheet's Effects tab — the icon's long form.
class EffectIcon : public ui::Widget {
public:
	EffectIcon(const std::vector<Character>* roster, size_t member, size_t index,
			   const ItemIconBank* icons, std::function<void()> onClick);

private:
	// The effect this icon stands for, re-resolved every frame — a repeated
	// child holds its INDEX, never a pointer into the model.
	const fx::Inst* Effect() const;
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	size_t m_index;
	const ItemIconBank* m_icons; // may be null (effect icons skip art)
	std::function<void()> m_onClick;
	bool m_hot = false;
	bool m_held = false;
};

// The three resource bars (health / stamina / mana). A click anywhere on the
// band, either button, opens the sheet's Stats tab.
class StatsArea : public ui::Widget {
public:
	StatsArea(const std::vector<Character>* roster, size_t member,
			  const ResourceBarColors* barColors, std::function<void()> onBars);

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	const ResourceBarColors* m_barColors;
	std::function<void()> m_onBars;
	bool m_held = false;
	bool m_heldRight = false;
};

class CharacterPanel : public ui::Widget {
public:
	// The big placeholder initial resolves its own font now (Display at kBustRem
	// of the HUD — CharacterPanel.cpp), so nothing is handed down for it.
	// onClick fires on a left click on the PORTRAIT (open the sheet / place a
	// held tablet); onRight on a right click there (open this member's
	// inventory). onBars fires on either button over the stat bars (the Stats
	// tab), onEffects on a click on an effect icon (the Effects tab).
	CharacterPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   size_t member, const ResourceBarColors* barColors,
				   const HitSplatIcons* hitSplats, const ItemIconBank* icons,
				   std::function<void()> onClick,
				   std::function<void()> onRight, std::function<void()> onBars,
				   std::function<void()> onEffects);

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	// Places the three children against this slot's live pixel rect (see the
	// header note on why they aren't authored constants).
	void LayoutSelf(ui::UIContext& ctx) override;
	// The slot highlights as one piece, so its hover is latched before the
	// children claim the mouse.
	void UpdateBeforeChildren(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Inset shared by every part, as a fraction of the slot's HEIGHT.
	static constexpr float kPad = 0.08f;

	const std::vector<Character>* m_roster;
	size_t m_member;
	PortraitBox* m_portrait = nullptr;
	ui::Repeater* m_effects = nullptr;
	StatsArea* m_stats = nullptr;
	bool m_hot = false;
	bool m_pressed = false; // a press latched anywhere in this slot
};
} // namespace dungeon::game
