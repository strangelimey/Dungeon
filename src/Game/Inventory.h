// ============================================================================
// Game/Inventory.h — a party member's carried items.
//
// One Inventory per Character: a small backpack grid plus the two hand slots
// (the HUD's left/right HandSlots, Dungeon Master style). An ItemSlot names the
// item by its CATALOG id ("rune_fire", ...) — empty string = nothing. Items are
// single (no stacking yet: a rune is one tablet), so a slot is just a typeId.
//
// The world resolves a typeId back to its ItemKind / model / icon
// (DungeonWorld::ItemKindFor); this header stays pure data so Character, the
// HUD, and the save layer can all share it without pulling in the renderer.
// ============================================================================
#pragma once

#include <array>
#include <string>

namespace dungeon::game {

// Backpack slots per character. Hands are separate (the two HUD HandSlots).
inline constexpr int kBackpackSlots = 8;

struct ItemSlot {
	std::string typeId; // catalog id; empty = the slot is free
	bool Empty() const { return typeId.empty(); }
	void Clear() { typeId.clear(); }
};

struct Inventory {
	std::array<ItemSlot, kBackpackSlots> backpack;
	ItemSlot hands[2]; // 0 = left, 1 = right (the item held, not the cooldown)

	// Index of the first empty backpack slot, or -1 when the pack is full.
	int FirstFreeBackpack() const {
		for (int i = 0; i < kBackpackSlots; ++i)
			if (backpack[i].Empty()) return i;
		return -1;
	}

	// Drops `typeId` into the first free backpack slot; false if the pack is
	// full. (The caller decides what to do when it fails — log, keep holding.)
	bool AddToBackpack(const std::string& typeId) {
		const int i = FirstFreeBackpack();
		if (i < 0) return false;
		backpack[i].typeId = typeId;
		return true;
	}

	// Empties every slot (a fresh party carries nothing).
	void Clear() {
		for (ItemSlot& s : backpack) s.Clear();
		hands[0].Clear();
		hands[1].Clear();
	}
};

} // namespace dungeon::game
