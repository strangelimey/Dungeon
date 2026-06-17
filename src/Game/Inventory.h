// ============================================================================
// Game/Inventory.h — a party member's carried + worn items.
//
// One Inventory per Character:
//   * hands[2]    the two HUD hand slots (held weapon/tablet, left/right)
//   * equipment[] the worn paper-doll slots (head/body/.../rings) on the sheet
//   * backpack    a DYNAMIC list of carry slots, shown as a grid on the sheet —
//                 starts small (kBackpackStart) and grows later as bags/spells/
//                 items raise capacity (Grow()).
// An ItemSlot names the item by its CATALOG id ("rune_fire", ...) — empty
// string = nothing. Items are single (no stacking yet). The world resolves a
// typeId back to its ItemKind / model / icon (DungeonWorld::ItemKindFor); this
// header stays pure data so Character, the HUD/sheet, and the save layer share it.
// ============================================================================
#pragma once

#include <array>
#include <string>
#include <vector>

namespace dungeon::game {

// Backpack slots a fresh character starts with (capacity grows at runtime).
inline constexpr int kBackpackStart = 6;

struct ItemSlot {
	std::string typeId; // catalog id; empty = the slot is free
	bool Empty() const { return typeId.empty(); }
	void Clear() { typeId.clear(); }
};

// Worn equipment slots (the sheet's left-hand paper doll). Order is the save +
// layout order — APPEND, never reorder. The two rings share a display label.
enum class EquipSlot { Head, Body, Hands, Feet, Cloak, Amulet, Ring1, Ring2, Count };
inline constexpr int kEquipCount = static_cast<int>(EquipSlot::Count);

// Loc keys for each equipment slot, parallel to EquipSlot.
inline constexpr const char* kEquipLabels[kEquipCount] = {
	"equip.head", "equip.body",   "equip.hands", "equip.feet",
	"equip.cloak", "equip.amulet", "equip.ring",  "equip.ring",
};

struct Inventory {
	ItemSlot hands[2];                            // 0 = left, 1 = right
	std::array<ItemSlot, kEquipCount> equipment;  // worn armor/clothing
	std::vector<ItemSlot> backpack = std::vector<ItemSlot>(kBackpackStart);

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
		hands[0].Clear();
		hands[1].Clear();
		for (ItemSlot& s : equipment) s.Clear();
		for (ItemSlot& s : backpack) s.Clear();
	}
};

} // namespace dungeon::game
