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

#include <array>
#include <flat_map>
#include <flat_set>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

// One member's HEARTBEAT, behind the health bar's pulse. The phase is
// integrated here, on the CPU, rather than computed as time x rate in the
// shader - so a change of rate speeds the beat up or slows it down instead of
// jumping it to a different point in the beat. Presentation only, not saved.
struct BarPulse {
	float phase = 0.0f; // 0..1 within the current beat
	float bpm = 60.0f;  // current rate, eased toward HeartRateTarget
};

// How the resource bars look (docs/icon-updates-plan.md): kit bar #1's iron
// frame (assets/ui/bar_frame.png, cut by tools/CutBarFrame.py) around a
// procedural fill (assets/shaders/bar.hlsl). Owned by GameUI, shared by the
// party bar and the sheet, which read it live every draw. Not a user setting -
// each fill carries its own colour; the Settings > UI "Resource Bars" pickers
// went when the fills became procedural.
struct ResourceBarStyle {
	static constexpr size_t kMaxMembers = 4;

	const gfx::Texture* frame = nullptr; // null = the flat bars
	bool framed = true;                  // false = the flat debug look (uiskin=0)
	float clock = 0.0f;                  // real seconds, the fills' animation clock
	std::array<BarPulse, kMaxMembers> pulse{};
	// Dev console `hudbars` (judging the look without staging a fight): `demo`
	// sweeps every bar empty -> full -> empty; `pinnedBpm` >= 0 holds every
	// heartbeat at that rate instead of HeartRateTarget.
	bool demo = false;
	float pinnedBpm = -1.0f;
	// The FLAT look's fills (uiskin=0, or no frame texture), and the solid fills
	// of the two SUPPLY meters (docs/health-and-healing.md) - a placeholder until
	// they get a look of their own. Warm bread against cold water, so a glance
	// tells them apart without reading the labels.
	Vec4 health{0.62f, 0.18f, 0.14f, 1.0f};
	Vec4 stamina{0.26f, 0.52f, 0.22f, 1.0f};
	Vec4 mana{0.22f, 0.36f, 0.68f, 1.0f};
	Vec4 food{0.58f, 0.40f, 0.18f, 1.0f};
	Vec4 water{0.24f, 0.50f, 0.60f, 1.0f};

	const BarPulse& PulseOf(size_t member) const {
		static constexpr BarPulse kStill{0.0f, 0.0f};
		return member < kMaxMembers ? pulse[member] : kStill;
	}
};

// THE PARTY LEADER as the HUD sees it (ui-updates Phase 9): who leads, asked
// every draw (the world owns it - DungeonWorld::Leader), and what a click on a
// member's NAME does. GameUI owns one; every member panel points at it.
struct LeaderLink {
	std::function<int()> leader;       // roster index; null = slot 0
	std::function<void(size_t)> pick;  // a click on that member's name
	std::string leaderTip, pickTip;    // the hover lines (localized)
};

// One FLOATING HUD panel's placement and look (ui-panels P3a: the party bar,
// the two left plates, the Movement / Hands / Magic docks). The master copy
// lives in GameSettings (kHudPanelFields; Settings -> UI, settings.ini
// hud_<id>_pos / _scale / _opacity); the panel points at it and reads the live
// values every layout and draw, so a slider needs no apply step, and a corner
// drag and the slider edit the SAME scale.
struct HudPanelLook {
	float x = -1.0f;      // top-left as window fractions; < 0 = not moved yet,
	float y = -1.0f;      // so the panel sits at its default spot
	float scale = 1.0f;   // 0.5..1.5: the panel AND its text
	float opacity = 1.0f; // 0..1: the panel face only, never its controls
	// MINIMIZED into the closed-panels tray (ui-updates Phase 8; ini
	// hud_<id>_hidden): not drawn at all, a tray button stands in for it.
	bool hidden = false;
};

// WHERE an item sits in a member's inventory, for the item mouse buttons
// (right = details, middle = use menu; docs/ui-updates-plan.md P2): a doll cell
// (index = the EquipSlot, so the hands are LeftHand/RightHand), a slot of the
// SELECTED pack's contents, or a bag in the pack row. An address rather than a
// pointer: it is resolved again when a menu row is picked, so a slot that
// changed while the menu was open is noticed rather than acted on.
struct ItemPlace {
	enum class Kind { Doll, Pack, Bag };
	Kind kind = Kind::Pack;
	int index = 0;
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
// draws no icon. The address handed to GameUI is stable. Looked up by VIEW
// (transparent compare): the sheet asks for its slot outlines by string
// literal every frame, and a const std::string& parameter built a temporary
// string per call.
struct ItemIconBank {
	std::flat_map<std::string, const gfx::Texture*, std::less<>> byType;
	const gfx::Texture* For(std::string_view typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? nullptr : it->second;
	}
	// The Magic window's GLOWING runes (tools/BuildRuneGlow.py), by SpellSymbol
	// index: the glyph alone and its soft halo, both white for the draw to tint.
	// Null = not installed, and the rune falls back to its plain icon.
	static constexpr size_t kRuneSlots = 12;
	const gfx::Texture* runeGlyph[kRuneSlots]{};
	const gfx::Texture* runeGlow[kRuneSlots]{};
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
	// Where each item is WORN (catalog `wear`); absent = nowhere.
	std::flat_map<std::string, WearSlot, std::less<>> wearByType;
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
	// The doll slot this item is WORN in (catalog `wear`), or None for
	// something that can only be held or packed.
	WearSlot WornAt(const std::string& typeId) const {
		const auto it = wearByType.find(typeId);
		return it == wearByType.end() ? WearSlot::None : it->second;
	}
	// True if this item may go in `slot`. The hands ask `holdable`; every
	// other slot is type-specific (Inventory.h).
	bool FitsSlot(const std::string& typeId, EquipSlot slot) const {
		if (slot == EquipSlot::LeftHand || slot == EquipSlot::RightHand)
			return Holdable(typeId);
		return WearSlotFits(WornAt(typeId), slot);
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
