// ============================================================================
// Game/PartyHudTypes.h — shared data the party HUD widgets read live.
//
// Game owns the banks/colors (and loads the textures they point at); the
// widgets hold raw pointers and re-read every draw so a missing icon simply
// draws nothing. RosterMember re-resolves a party slot each frame so a roster
// resize can never dangle a Character*.
// ============================================================================
#pragma once

#include "Game/Character.h"

#include <flat_map>
#include <flat_set>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

// Resource bar fill colors, shared by the party bar and the sheet. The
// master copy lives in Game (Settings → UI edits it, persisted to
// settings.ini as bar_<name>); both widgets point at it and read the live
// values every draw.
struct ResourceBarColors {
	Vec4 health{0.62f, 0.18f, 0.14f, 1.0f};
	Vec4 stamina{0.26f, 0.52f, 0.22f, 1.0f};
	Vec4 mana{0.22f, 0.36f, 0.68f, 1.0f};
};

// The three hit-feedback splat icons, indexed by severity (0 = small, 1 =
// medium, 2 = hard), drawn over a struck member's portrait. The textures are
// owned by Game (loaded from assets); the party panels point at this struct and
// read it live, so a null entry (icon missing) simply draws no splat.
// (Indexed rather than named because <windows.h> #defines `small` as char.)
struct HitSplatIcons {
	const gfx::Texture* icon[3] = {nullptr, nullptr, nullptr};
	const gfx::Texture* For(int severity) const {
		return icon[severity < 0 ? 0 : (severity > 2 ? 2 : severity)];
	}
};

// Item icons keyed by catalog id ("rune_fire" → its rune_icon_fire texture),
// for the held cursor, the hand slots, and the inventory window. Owned by Game
// (the textures live there); the HUD widgets read it live, so a missing id just
// draws no icon. The address handed to GameUI is stable.
struct ItemIconBank {
	std::flat_map<std::string, const gfx::Texture*> byType;
	const gfx::Texture* For(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? nullptr : it->second;
	}
};

// Item carry weights (kg) keyed by catalog id, the data behind a member's carry
// load. Built once by Game from the items catalog; the sheet reads it live. A
// missing id weighs 0 (e.g. a typo or a weightless item).
struct ItemWeightBank {
	std::flat_map<std::string, float> byType;
	float For(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? 0.0f : it->second;
	}
};

// Item categories + (for containers) content-slot capacities, keyed by catalog
// id. Built once by Game; the sheet reads it to tell whether a held item is a
// pack (container) and how many slots a freshly-equipped pack should have.
struct ItemCategoryBank {
	std::flat_map<std::string, std::string> byType;   // category
	std::flat_map<std::string, int> capacityByType;   // pack content slots
	// Categories a pack may HOLD (empty / contains "any" = unrestricted).
	std::flat_map<std::string, std::vector<std::string>> acceptsByType;
	// Ids whose catalog entry sets holdable=1 — the only items a HAND slot
	// (control bar or sheet doll) accepts. Everything else is refused.
	// (A set, not flat_map<...,bool>: vector<bool> can't back a flat_map.)
	std::flat_set<std::string> holdableTypes;
	bool Is(const std::string& typeId, std::string_view category) const {
		const auto it = byType.find(typeId);
		return it != byType.end() && it->second == category;
	}
	std::string CategoryOf(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? std::string() : it->second;
	}
	// Content-slot capacity for a pack id, or 0 if unknown (caller defaults).
	int Capacity(const std::string& typeId) const {
		const auto it = capacityByType.find(typeId);
		return it == capacityByType.end() ? 0 : it->second;
	}
	// True if pack `packId` accepts an item of `category` in its contents.
	bool Accepts(const std::string& packId, const std::string& category) const {
		const auto it = acceptsByType.find(packId);
		if (it == acceptsByType.end() || it->second.empty()) return true; // unrestricted
		for (const std::string& a : it->second)
			if (a == "any" || a == category) return true;
		return false;
	}
	// True if pack `packId` may hold item `itemId`: the no-bag-in-a-bag rule plus
	// the accepts list. The one check shared by every place items enter a pack.
	bool PackAcceptsItem(const std::string& packId, const std::string& itemId) const {
		if (Is(itemId, "container")) return false; // no nesting bags
		return Accepts(packId, CategoryOf(itemId));
	}
	// True if a hand slot may hold this item (catalog holdable=1).
	bool Holdable(const std::string& typeId) const {
		return holdableTypes.contains(typeId);
	}
};

// Resolves roster slot `i` to the live member, or null when the roster is
// shorter than that (the party may have fewer than 4 members). The widgets
// call this at the top of Update/Draw instead of holding a Character*.
inline const Character* RosterMember(const std::vector<Character>* roster,
									 size_t i) {
	return roster && i < roster->size() ? &(*roster)[i] : nullptr;
}
inline Character* RosterMember(std::vector<Character>* roster, size_t i) {
	return roster && i < roster->size() ? &(*roster)[i] : nullptr;
}

} // namespace dungeon::game
