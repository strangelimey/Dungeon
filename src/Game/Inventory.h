// ============================================================================
// Game/Inventory.h — a party member's carried + worn items.
//
// One Inventory per Character:
//   * equipment[] the worn/held paper-doll slots, indexed by EquipSlot. This
//                 INCLUDES the two weapon hands (LeftHand/RightHand): the sheet
//                 doll and the HUD control-bar hand boxes are the same storage,
//                 so an item placed in one shows in the other. Hand(0/1) is the
//                 convenience accessor the HUD uses.
//   * backpack    a DYNAMIC list of carry slots, shown as a grid on the sheet —
//                 starts small (kBackpackStart) and grows later as bags/spells/
//                 items raise capacity (Grow()).
// An ItemSlot names the item by its CATALOG id ("rune_fire", ...) — empty
// string = nothing. Items are single (no stacking yet). The world resolves a
// typeId back to its ItemKind / model / icon (DungeonWorld::ItemKindFor); this
// header stays pure data so Character, the HUD/sheet, and the save layer share it.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

// Backpack slots a fresh character starts with (capacity grows at runtime).
inline constexpr int kBackpackStart = 6;

// Placeholder accent tint for an item category — drives both the floor mesh
// (tablet, tinted) and the generated hand/cursor icon, so they read alike until
// real per-item art lands. Runes don't use this (they tint by spell element).
inline Vec4 CategoryTint(std::string_view category) {
	if (category == "weapon")   return {0.62f, 0.64f, 0.70f, 1.0f}; // steel grey
	if (category == "armor")    return {0.46f, 0.31f, 0.18f, 1.0f}; // leather brown
	if (category == "clothing") return {0.30f, 0.46f, 0.56f, 1.0f}; // cloth blue
	if (category == "food")     return {0.74f, 0.34f, 0.26f, 1.0f}; // warm red
	return {0.55f, 0.55f, 0.55f, 1.0f};                             // misc grey
}

struct ItemSlot {
	std::string typeId; // catalog id; empty = the slot is free
	bool Empty() const { return typeId.empty(); }
	void Clear() { typeId.clear(); }
};

// Worn/held paper-doll slots, indexed for both the sheet doll and the save
// (the "equip" line serializes in this order). LeftHand/RightHand are the
// weapon hands shared with the HUD control bar; the two rings share a display
// label. Reordering changes the save layout — bump SaveData::version if you do.
enum class EquipSlot {
	Head, Body, Legs, Feet, Cloak, Amulet, LeftHand, RightHand, Ring1, Ring2, Count
};
inline constexpr int kEquipCount = static_cast<int>(EquipSlot::Count);

// Loc keys for each equipment slot, parallel to EquipSlot. Both hands show
// "Hand" and both rings show "Ring".
inline constexpr const char* kEquipLabels[kEquipCount] = {
	"equip.head",  "equip.body",   "equip.legs", "equip.feet", "equip.cloak",
	"equip.amulet", "equip.hand",   "equip.hand", "equip.ring", "equip.ring",
};

struct Inventory {
	std::array<ItemSlot, kEquipCount> equipment; // worn/held, indexed by EquipSlot
	std::vector<ItemSlot> backpack = std::vector<ItemSlot>(kBackpackStart);

	// The weapon hand slots (0 = left, 1 = right), aliases into equipment[] so the
	// HUD hand boxes and the sheet doll share one storage.
	ItemSlot& Hand(int h) {
		return equipment[static_cast<size_t>(h == 0 ? EquipSlot::LeftHand
													: EquipSlot::RightHand)];
	}
	const ItemSlot& Hand(int h) const {
		return equipment[static_cast<size_t>(h == 0 ? EquipSlot::LeftHand
													: EquipSlot::RightHand)];
	}

	// Index of the first empty backpack slot, or -1 when the pack is full.
	int FirstFreeBackpack() const {
		for (size_t i = 0; i < backpack.size(); ++i)
			if (backpack[i].Empty()) return static_cast<int>(i);
		return -1;
	}

	// Drops `typeId` into the first free backpack slot; false if the pack is
	// full. (The caller decides what to do when it fails — log, keep holding.)
	bool AddToBackpack(const std::string& typeId) {
		const int i = FirstFreeBackpack();
		if (i < 0) return false;
		backpack[static_cast<size_t>(i)].typeId = typeId;
		return true;
	}

	// Adds `extra` empty backpack slots (a bag/spell raised capacity).
	void Grow(int extra) {
		if (extra > 0) backpack.resize(backpack.size() + static_cast<size_t>(extra));
	}

	// Empties every slot but keeps the current backpack capacity.
	void Clear() {
		for (ItemSlot& s : equipment) s.Clear(); // includes the hands
		for (ItemSlot& s : backpack) s.Clear();
	}
};

} // namespace dungeon::game
