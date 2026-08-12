// ============================================================================
// Game/CharacterSheetLayout.h — shared layout fractions for CharacterSheet*.cpp.
// Parent-relative [0..1] of the sheet pixel rect. Not for use outside those TUs.
// ============================================================================
#pragma once

#include "Game/Inventory.h" // EquipSlot
#include "Graphics/SpriteBatch.h"

namespace dungeon::game {
namespace sheet {

// Resolve [0..1] of the sheet into pixels.
inline gfx::Rect At(const gfx::Rect& px, float x, float y, float w, float h) {
	return {px.x + x * px.w, px.y + y * px.h, w * px.w, h * px.h};
}
inline float Ax(const gfx::Rect& px, float x) { return px.x + x * px.w; }
inline float Ay(const gfx::Rect& px, float y) { return px.y + y * px.h; }

// THE PANEL GREW 30% WIDER (GameUI kSheetW 0.50 -> 0.65) to give the backpack
// tab three real columns: paper doll, defense numbers, backpack.
//
// Every fraction here is of the PANEL, so widening it stretches all of them —
// and a SIZE expressed as an x-fraction against an h-fraction stops being
// square the moment the panel's aspect changes. That is the same axis-mixing
// that made the HUD's hand boxes rectangular. So the width fractions of
// anything SQUARE (doll cells, pack cells, the portrait, the mode buttons) are
// divided by the growth factor below, which keeps them the pixel size they
// already were; only the POSITIONS were re-laid.
inline constexpr float kWiden = 0.50f / 0.65f; // old panel width / new

// --- shared tab body columns ------------------------------------------------
inline constexpr float kLeft = 0.055f;
inline constexpr float kHeaderY = 0.325f;
inline constexpr float kBodyTop = 0.375f;
inline constexpr float kEmptyListY = 0.411f;

// --- equipment paper doll ---------------------------------------------------
inline constexpr float kEquipW = 0.092f * kWiden;
inline constexpr float kEquipH = 0.129f;
inline constexpr float kDollStepX = 0.105f * kWiden;
inline constexpr float kDollStepY = 0.146f;
struct DollCell {
	EquipSlot slot;
	float col, row;
};
inline constexpr DollCell kDollCells[] = {
	{EquipSlot::Head,      1.0f, 0.0f},
	{EquipSlot::Amulet,    0.0f, 1.0f},
	{EquipSlot::Body,      1.0f, 1.0f},
	{EquipSlot::Cloak,     2.0f, 1.0f},
	{EquipSlot::LeftHand,  0.0f, 2.0f},
	{EquipSlot::Legs,      1.0f, 2.0f},
	{EquipSlot::RightHand, 2.0f, 2.0f},
	{EquipSlot::Ring1,    -0.3f, 3.0f},
	{EquipSlot::Ring2,     2.3f, 3.0f},
	{EquipSlot::Feet,      1.0f, 3.0f},
};
inline constexpr int kDollCellCount =
	static_cast<int>(sizeof(kDollCells) / sizeof(kDollCells[0]));

// --- pack row + backpack grid -----------------------------------------------
inline constexpr float kPackW = 0.092f * kWiden;
inline constexpr float kPackH = 0.129f;
inline constexpr float kPackGapX = 0.013f * kWiden;
inline constexpr float kPackGapY = 0.018f;
// COLUMN 3 of the backpack tab. Right-aligned to the panel's margin so the
// grid's own edge is the edge of the content, whatever the middle column does.
inline constexpr float kPackGridW =
	4.0f * (0.092f * kWiden) + 3.0f * (0.013f * kWiden);
inline constexpr float kPackX = 1.0f - 0.045f - kPackGridW;
inline constexpr int kPackCols = 4;
inline constexpr float kPackRowY = kBodyTop;
inline constexpr float kPackSepY = 0.520f;
inline constexpr float kPackY = 0.536f;

// --- defense readout: COLUMN 2 of the backpack tab ---------------------------
// The doll's widest cells are its RINGS, which sit at column -0.3 and 2.3 —
// that is what the middle column has to clear, and the reason the numbers used
// to overlap the doll horizontally even though they never collided on screen
// (the rings sit lower than the text). Derived from the doll rather than
// authored, so the columns cannot drift apart if the doll is retuned.
inline constexpr float kDollRight = kLeft + 2.3f * kDollStepX + kEquipW;
inline constexpr float kDefX = kDollRight + 0.030f;
// Its TITLE sits on the same line as the pack's "Load" heading — they are the
// two column headings of the same content area and should read as a row.
inline constexpr float kDefTitleY = kHeaderY;
inline constexpr float kDefY = kBodyTop;
inline constexpr float kDefRow = 0.038f;   // line pitch, in panel fractions
inline constexpr float kDefIndent = 0.014f; // the breakdown rows under "Roll"
// The value column: far enough right that a label never reaches it, and still
// clear of the pack grid.
inline constexpr float kDefValueX = kDefX + 0.150f;

// --- mode toggle buttons under the portrait ---------------------------------
inline constexpr int kModeCount = 5;
inline constexpr float kModeBtnW = 0.038f * kWiden;
inline constexpr float kModeBtnH = 0.054f;
inline constexpr float kModeBtnGap = 0.006f * kWiden;
inline constexpr float kModeBtnX = 0.031f * kWiden;
inline constexpr float kModeBtnY = 0.236f;

// --- header portrait / name -------------------------------------------------
inline constexpr float kPortraitX = 0.031f * kWiden, kPortraitY = 0.036f;
inline constexpr float kPortraitW = 0.128f * kWiden, kPortraitH = 0.179f;
inline constexpr float kNameX = 0.179f * kWiden, kNameY = 0.054f;

// --- stats / skills columns -------------------------------------------------
inline constexpr float kLabelX = 0.072f;
inline constexpr float kValueRight = 0.385f;
// (No kFirstRowY: the Stats rows start a measured line below the heading —
// see DrawStats — so the gap tracks the text instead of being authored twice.)
inline constexpr float kRowH = 0.071f;
inline constexpr float kBarLabelX = 0.462f;
inline constexpr float kBarX = 0.603f;
inline constexpr float kBarW = 0.308f;
inline constexpr float kBarH = 0.039f;
inline constexpr float kSkillBarX = 0.462f;
inline constexpr float kSkillBarW = 0.436f;
// (kStatRowH / kStatBarH are defined with kStatRem, further down.)

// --- list scroll band -------------------------------------------------------
inline constexpr float kScrollTop = 0.368f;
inline constexpr float kScrollBottomPad = 0.021f;
inline constexpr float kScrollBarW = 0.013f * kWiden;
// In REM (UI/Units.h) — the sheet's chrome tracks its own text size.
inline constexpr float kScrollBarPadRem = 0.2f;
inline constexpr float kScrollThumbMinRem = 1.1f;
inline constexpr float kWheelStepRem = 1.8f;

// --- effects / spells list rows ---------------------------------------------
// Gaps BETWEEN rows in the three scrolling tabs. Deliberately tight: these
// lists grow (every weapon class trains its own skill, every spell learned
// stays listed), so the tabs are read by scanning down them and a generous gap
// costs visible rows for nothing. The rows' own heights come from their text.
inline constexpr float kEffectRowGap = 0.014f;
inline constexpr float kEffectIconX = 0.072f;
// The symbol's SIZE is derived, not authored: it is square and spans exactly
// the name line, so its top sits on the name's top and its bottom on the
// description's — see CharacterSheet::EffectIconSize. This is the gap from its
// right edge to the text column that follows it, in rem.
inline constexpr float kEffectIconGapRem = 0.7f;
inline constexpr float kTextRight = 0.928f;
inline constexpr float kSpellRowGap = 0.010f;
inline constexpr float kSpellTextX = 0.072f;
// Description prose — a spell's, an effect's — is drawn in the Script role at
// this multiple of the sheet's root font. It runs LARGER than the interface
// text beside it because a script face carries much less x-height per em
// (IM Fell English sits at 445/1000 against Consolas' far taller lowercase),
// so matched pixel sizes do not read as matched. Shared by both row kinds so
// the two cannot drift, and used by their MEASURE as well as their DRAW.
inline constexpr float kDescRem = 1.25f;
// A row's NAME line — a spell's, an effect's, and an effect's duration. Sits
// just above the description so the entry reads title-then-prose.
inline constexpr float kNameRem = 1.3f;
// Gap between a row's NAME line and the description beneath it, in rem. Both
// the spell and effect rows measure AND draw with it, so they cannot drift.
// The offset is taken from the name's HEIGHT, not its line advance: the
// advance already carries a line's worth of leading, and adding a gap on top
// of that is what pushed the title away from its own description.
inline constexpr float kNameDescGapRem = 0.08f;
// The Stats and Skills tabs are pure data — attribute names, numbers, bars —
// and were far too small to read at a glance. This scales their TEXT, and
// kStatRowH / kStatBarH scale the row pitch and bar height with it: growing the
// font alone would push the numbers straight out of the bars they sit in.
inline constexpr float kStatRem = 1.5f;
// The SKILLS readout is the same kind of thing as the Stats one but has to
// stay dense: attributes are a fixed five, while skills accumulate one per
// school and one per weapon class as they train. So it takes its own, smaller
// scale, and its rows are sized from that text (MeasureSkillRow) rather than
// riding the Stats row pitch, which was built for a fixed five.
inline constexpr float kSkillRem = 1.25f;
inline constexpr float kSkillRowGap = 0.008f;
// A tab's TITLE — "Attributes", "Skills", "Known Spells", "Effects". Its own
// name rather than a reuse of kStatRem, which happens to share the value: the
// four titles should stay a set even if the stat readout is retuned alone.
inline constexpr float kTabTitleRem = 1.5f;
// Derived from the originals rather than re-authored, so the tabs stay in
// proportion if either is ever retuned.
inline constexpr float kStatRowH = kRowH * kStatRem;
inline constexpr float kStatBarH = kBarH * kStatRem;

} // namespace sheet
} // namespace dungeon::game
