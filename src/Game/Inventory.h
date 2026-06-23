// ============================================================================
// Game/Inventory.h — a party member's carried + worn items.
//
// One Inventory per Character:
//   * equipment[] the worn/held paper-doll slots, indexed by EquipSlot. This
//                 INCLUDES the two weapon hands (LeftHand/RightHand): the sheet
//                 doll and the HUD control-bar hand boxes are the same storage,
//                 so an item placed in one shows in the other. Hand(0/1) is the
//                 convenience accessor the HUD uses.
//   * packs       the carried CONTAINERS (backpack, ammo pouch, ...) — a pack
//                 row, each Pack holding its own dynamic contents. Slot 0 is the
//                 starting backpack. The SELECTED pack's contents are the grid
//                 shown/edited on the sheet (SelectedContents()).
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

// Pack-row slots: the containers (bags) a member carries. Slot 0 holds the
// starting backpack every character begins with; the others are open for more
// packs later. The SELECTED pack's contents fill the slot grid below it.
inline constexpr int kPackRowSlots = 4;
inline constexpr const char* kStartingPack = "backpack"; // catalog id, pack slot 0

// Placeholder accent tint for an item category — drives both the floor mesh
// (tablet, tinted) and the generated hand/cursor icon, so they read alike until
// real per-item art lands. Runes don't use this (they tint by spell element).
inline Vec4 CategoryTint(std::string_view category) {
	if (category == "weapon")   return {0.62f, 0.64f, 0.70f, 1.0f}; // steel grey
	if (category == "armor")    return {0.46f, 0.31f, 0.18f, 1.0f}; // leather brown
	if (category == "clothing") return {0.30f, 0.46f, 0.56f, 1.0f}; // cloth blue
	if (category == "food")     return {0.74f, 0.34f, 0.26f, 1.0f}; // warm red
	if (category == "container") return {0.40f, 0.26f, 0.14f, 1.0f}; // dark leather
	if (category == "ingredient") return {0.34f, 0.60f, 0.32f, 1.0f}; // herb green
	return {0.55f, 0.55f, 0.55f, 1.0f};                             // misc grey
}

struct ItemSlot {
	std::string typeId; // catalog id; empty = the slot is free
	bool Empty() const { return typeId.empty(); }
	void Clear() { typeId.clear(); }
};

// A carried container (backpack, ammo pouch, medicine pouch, ...) plus its own
// contents. An empty typeId = an empty pack-row slot (no container).
struct Pack {
	std::string typeId;             // pack catalog id; "" = empty pack slot
	std::vector<ItemSlot> contents; // items inside this pack
	bool Empty() const { return typeId.empty(); }
	// True if the pack holds any item (so it can't be swapped out / lost).
	bool HasItems() const {
		for (const ItemSlot& s : contents)
			if (!s.Empty()) return true;
		return false;
	}
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

// Outline-silhouette key for each equipment slot, parallel to EquipSlot (the
// sheet draws assets/textures/slot_<key>.png as the empty slot's ghost). Both
// hands share "hand"; both rings share "ring".
inline constexpr const char* kEquipIcon[kEquipCount] = {
	"head", "body",   "legs", "feet", "cloak",
	"amulet", "hand", "hand", "ring", "ring",
};

struct Inventory {
	std::array<ItemSlot, kEquipCount> equipment; // worn/held, indexed by EquipSlot
	// The carried containers; slot 0 is the starting backpack (seeded by the
	// constructor / Clear). Each Pack carries its own contents; the SELECTED
	// pack's contents are what the sheet's slot grid shows.
	std::array<Pack, kPackRowSlots> packs;
	int selectedPack = 0; // which pack-row slot's contents the grid shows

	Inventory() { ResetPacks(); }

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

	// The selected pack's contents — the slot grid the sheet shows and edits.
	std::vector<ItemSlot>& SelectedContents() {
		return packs[static_cast<size_t>(selectedPack)].contents;
	}
	const std::vector<ItemSlot>& SelectedContents() const {
		return packs[static_cast<size_t>(selectedPack)].contents;
	}

	// Index of the first empty slot in pack `p`'s contents, or -1 if full / no
	// container in that slot.
	int FirstFree(int p) const {
		if (packs[static_cast<size_t>(p)].Empty()) return -1;
		const auto& c = packs[static_cast<size_t>(p)].contents;
		for (size_t i = 0; i < c.size(); ++i)
			if (c[i].Empty()) return static_cast<int>(i);
		return -1;
	}

	// Drops `typeId` into pack `p`'s first free slot; false if full / no pack.
	bool AddToPack(const std::string& typeId, int p) {
		const int i = FirstFree(p);
		if (i < 0) return false;
		packs[static_cast<size_t>(p)].contents[static_cast<size_t>(i)].typeId = typeId;
		return true;
	}
	// Stows into the SELECTED pack (the active container) — the default target.
	bool Stow(const std::string& typeId) { return AddToPack(typeId, selectedPack); }

	// Adds `extra` empty slots to the selected pack (a bag/spell raised capacity).
	void Grow(int extra) {
		if (extra <= 0) return;
		auto& c = SelectedContents();
		c.resize(c.size() + static_cast<size_t>(extra));
	}

	// Resets the pack row to a fresh starting backpack in slot 0 (others empty).
	void ResetPacks() {
		for (Pack& p : packs) { p.typeId.clear(); p.contents.clear(); }
		packs[0].typeId = kStartingPack;
		packs[0].contents.assign(kBackpackStart, {});
		selectedPack = 0;
	}

	// Empties every worn/held slot and resets the packs to the starting backpack.
	void Clear() {
		for (ItemSlot& s : equipment) s.Clear(); // includes the hands
		ResetPacks();
	}
};

} // namespace dungeon::game
