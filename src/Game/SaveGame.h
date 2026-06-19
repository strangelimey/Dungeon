// ============================================================================
// Game/SaveGame.h — the save file: the level's dynamic state, on disk.
//
// A save NEVER stores the static layer (DungeonMap / the .map file) — only the
// things that can change during play. Loading reconstructs the level from its
// .map + .ent baseline, then applies a save on top:
//   - State with NO static baseline (party pose, fog of war, character
//     resources, torch palette) is stored whole — it is born at runtime.
//   - State derived from the .ent baseline (monsters/items/...) is stored as a
//     DIFF: only entities whose live state differs from their spawn record are
//     emitted, keyed by Entity::id (stable across the .ent cell sort).
//
// Format is plain UTF-8 text in the project's record dialect (';' comments,
// whitespace tokens — see Entity.h), so saves are human-readable and editable
// like the .map/.ent/settings.ini files. Files live in paths::SaveDir()
// (Documents\DungeonSaves) as "<slug>.dsav". DungeonWorld fills/applies the
// world half (CaptureState/ApplyState); Game owns the character half and the
// file I/O. This header is pure data + serialization — no game logic.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

// The serializable dynamic state of one in-progress game.
struct SaveData {
	int version = 6; // v6: free-look offset ("look" line); v5 folded hands into equip[]
	std::string name;         // display name (free text; may contain spaces)
	std::string currentLevel; // the level stem the party is on (where to resume)
	std::string timestamp;    // human-readable local time, for the slot list

	// --- party (no static baseline → stored whole) --------------------------
	int partyX = 0, partyZ = 0;
	int partyFacing = 2; // 0=N 1=E 2=S 3=W
	// Right-mouse free-look offset (radians) layered on the grid facing, so a
	// save mid-look returns to the EXACT camera angle. The offset parks where the
	// player left it (it eases back only on the next move); looking marks an
	// active drag. Defaults (0/0/false) = orthogonal, so pre-v6 saves load square-on.
	float lookYaw = 0.0f, lookPitch = 0.0f;
	bool looking = false;

	int torchPalette = 0; // HUD torchlight index (0 warm, 1 cold, 2 eerie)

	// Per-roster-slot mutable resources, in roster order.
	struct CharState {
		float health = 1, maxHealth = 1;
		float stamina = 1, maxStamina = 1;
		float mana = 1, maxMana = 1;
		u32 knownSymbols = 0; // memorized spell symbols (SymbolBit mask)
		// Carried/worn items, by catalog id ("" = empty). Inventory travels with
		// the party (not per-level). equipment is in EquipSlot order and now
		// includes the two weapon hands (LeftHand/RightHand); backpack is dynamic
		// (capacity grows in play).
		std::vector<std::string> equipment;
		std::vector<std::string> backpack;
	};
	std::vector<CharState> characters;

	// Per-entity overrides, keyed by Entity::id — only entities that differ
	// from their .ent spawn are present (monsters carry a moved grid cell,
	// whether they have announced themselves, and current hit points: a slain
	// monster saves hp=0 so it stays down on load; inventory fields extend this
	// struct later). hp = -1 means "not recorded" (older saves) → keep spawn hp.
	//
	// Editor-placed monsters have NO .ent baseline to diff against, so they save
	// WHOLE: `type` is set (empty for a baseline diff row) and `spawnX/spawnZ/
	// facing` carry what's needed to recreate the instance on load. Such rows
	// serialize as a "monster" record; baseline diffs stay "ent" records.
	struct EntityState {
		int id = -1;
		int x = 0, z = 0;
		bool announced = false;
		float hp = -1.0f;
		std::string type;        // non-empty => recreate whole; empty => diff
		int spawnX = 0, spawnZ = 0;
		int facing = 2;          // Direction value (0=N 1=E 2=S 3=W)
	};

	// Dynamic state of one level: revealed cells (fog, stored whole) + the
	// entity diff. One entry per VISITED level — the world keeps each level's
	// state so leaving and returning preserves fog/progress (P6 multi-level).
	// An item lying on a level's floor (a full snapshot, not a diff): everything
	// currently on the ground, baseline or dropped, by cell + catalog id.
	struct FloorItem {
		int x = 0, z = 0;
		std::string typeId;
	};
	struct LevelState {
		std::string stem;
		std::vector<std::pair<int, int>> seen;
		std::vector<EntityState> entities;
		std::vector<FloorItem> floorItems; // what's on this level's floor
	};
	std::vector<LevelState> levels;
};

// One save file's header, for the slot browser (cheap: parsed from the file).
struct SaveSlot {
	std::string name;
	std::string level;
	std::string timestamp;
	std::string path; // full path, ready for ReadSave
};

// Absolute path for a save named `name` (sanitized to a "<slug>.dsav" file
// under SaveDir). Two names that sanitize alike share a slot — intended:
// re-saving "My Game" overwrites it.
std::string SaveSlotPath(const std::string& name);

// Serialize / parse one save file. WriteSave creates SaveDir as needed and
// returns false on write failure; ReadSave returns nullopt for a missing or
// unparseable file.
bool WriteSave(const SaveData& data, const std::string& path);
std::optional<SaveData> ReadSave(const std::string& path);

// Every "*.dsav" in SaveDir, newest first (by timestamp string). Files that
// fail to parse are skipped. Empty if the folder doesn't exist yet.
std::vector<SaveSlot> ListSaves();

} // namespace dungeon::game
