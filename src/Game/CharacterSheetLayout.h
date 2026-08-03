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

// --- shared tab body columns ------------------------------------------------
inline constexpr float kLeft = 0.072f;
inline constexpr float kHeaderY = 0.325f;
inline constexpr float kBodyTop = 0.375f;
inline constexpr float kEmptyListY = 0.411f;

// --- equipment paper doll ---------------------------------------------------
inline constexpr float kEquipW = 0.092f;
inline constexpr float kEquipH = 0.129f;
inline constexpr float kDollStepX = 0.105f;
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
inline constexpr float kPackW = 0.092f;
inline constexpr float kPackH = 0.129f;
inline constexpr float kPackGapX = 0.013f;
inline constexpr float kPackGapY = 0.018f;
inline constexpr float kPackX = 0.551f;
inline constexpr int kPackCols = 4;
inline constexpr float kPackRowY = kBodyTop;
inline constexpr float kPackSepY = 0.520f;
inline constexpr float kPackY = 0.536f;

// --- mode toggle buttons under the portrait ---------------------------------
inline constexpr int kModeCount = 5;
inline constexpr float kModeBtnW = 0.038f;
inline constexpr float kModeBtnH = 0.054f;
inline constexpr float kModeBtnGap = 0.006f;
inline constexpr float kModeBtnX = 0.031f;
inline constexpr float kModeBtnY = 0.236f;

// --- header portrait / name -------------------------------------------------
inline constexpr float kPortraitX = 0.031f, kPortraitY = 0.036f;
inline constexpr float kPortraitW = 0.128f, kPortraitH = 0.179f;
inline constexpr float kNameX = 0.179f, kNameY = 0.054f;

// --- stats / skills columns -------------------------------------------------
inline constexpr float kLabelX = 0.072f;
inline constexpr float kValueRight = 0.385f;
inline constexpr float kFirstRowY = 0.389f;
inline constexpr float kRowH = 0.071f;
inline constexpr float kBarLabelX = 0.462f;
inline constexpr float kBarX = 0.603f;
inline constexpr float kBarW = 0.308f;
inline constexpr float kBarH = 0.039f;
inline constexpr float kSkillBarX = 0.462f;
inline constexpr float kSkillBarW = 0.436f;

// --- list scroll band -------------------------------------------------------
inline constexpr float kScrollTop = 0.368f;
inline constexpr float kScrollBottomPad = 0.021f;
inline constexpr float kScrollBarW = 0.013f;
// In REM (UI/Units.h) — the sheet's chrome tracks its own text size.
inline constexpr float kScrollBarPadRem = 0.2f;
inline constexpr float kScrollThumbMinRem = 1.1f;
inline constexpr float kWheelStepRem = 1.8f;

// --- effects / spells list rows ---------------------------------------------
inline constexpr float kEffectRowGap = 0.039f;
inline constexpr float kEffectIconX = 0.072f;
inline constexpr float kEffectIconW = 0.062f;
inline constexpr float kEffectIconH = 0.086f;
inline constexpr float kEffectTextX = 0.154f;
inline constexpr float kTextRight = 0.928f;
inline constexpr float kSpellRowGap = 0.025f;
inline constexpr float kSpellTextX = 0.072f;
// Description prose — a spell's, an effect's — is drawn in the Script role at
// this multiple of the sheet's root font. It runs LARGER than the interface
// text beside it because a script face carries much less x-height per em
// (IM Fell English sits at 445/1000 against Consolas' far taller lowercase),
// so matched pixel sizes do not read as matched. Shared by both row kinds so
// the two cannot drift, and used by their MEASURE as well as their DRAW.
inline constexpr float kDescRem = 1.25f;

} // namespace sheet
} // namespace dungeon::game
