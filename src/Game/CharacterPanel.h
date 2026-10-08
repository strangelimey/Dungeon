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

	// How many effect plaques have been drawn in ARMED frames (alloc::
	// FrameArmed), ever: the alloctest verdict's efftips=, AllocTest -Effects'
	// proof that its hover drew one - and its dash - where the guard was
	// watching (code-review C229).
	static u64 ArmedTipDraws();

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

// The member's NAME at the head of the slot, sized to the measured name, and
// the party leader's picker (ui-updates Phase 9): a click makes this member the
// leader. The leader's name is drawn in the accent over a soft glow; a hovered
// name of someone who could lead is underlined and says so. A downed member's
// name is not a target - only a standing member can lead.
class NameTag : public ui::Widget {
public:
	NameTag(const std::vector<Character>* roster, size_t member);
	void SetLink(const LeaderLink* link) { m_link = link; }
	bool IsLeader() const;

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	const LeaderLink* m_link = nullptr; // null = a plain name, not a picker
	bool m_hot = false;
	bool m_held = false;
};

// The three resource bars (health / stamina / mana). A click anywhere on the
// band, either button, opens the sheet's Stats tab.
class StatsArea : public ui::Widget {
public:
	StatsArea(const std::vector<Character>* roster, size_t member,
			  const ResourceBarStyle* barStyle, std::function<void()> onBars);

private:
	// Space between framed bars, in rem (flat bars keep 0.25): FRAME to frame,
	// since ui-updates stacks whole frames so their caps cannot interleave.
	static constexpr float kFramedGapRem = 0.12f;

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	const ResourceBarStyle* m_barStyle;
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
				   size_t member, const ResourceBarStyle* barStyle,
				   const HitSplatIcons* hitSplats, const ItemIconBank* icons,
				   std::function<void()> onClick,
				   std::function<void()> onRight, std::function<void()> onBars,
				   std::function<void()> onEffects);

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

	// The party leader: who leads, and the click on this member's name that
	// picks them (Phase 9). Null = the name is only a name.
	void SetLeaderLink(const LeaderLink* link) { m_name->SetLink(link); }

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
	// The member's name, as a multiple of the panel's text size.
	static constexpr float kNameScale = 1.3f;
	// The portrait's own, tighter inset: its carved frame already separates it
	// from the slot's edge (Michael: less padding outside the border).
	static constexpr float kPortraitPad = 0.04f;
	// Where the name / effect strip / bars start: past the portrait and a kPad gap.
	float ColumnLeft(float slotW, float slotH) const {
		return (kPortraitPad * slotH + (slotH - 2 * kPortraitPad * slotH) + kPad * slotH) /
			   slotW;
	}

	const std::vector<Character>* m_roster;
	size_t m_member;
	PortraitBox* m_portrait = nullptr;
	NameTag* m_name = nullptr;
	ui::Repeater* m_effects = nullptr;
	StatsArea* m_stats = nullptr;
	bool m_hot = false;
	bool m_pressed = false; // a press latched anywhere in this slot
};
} // namespace dungeon::game
