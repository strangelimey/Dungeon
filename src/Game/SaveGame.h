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
#include "Game/Entity.h" // EntityKind, Direction

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

// The serializable dynamic state of one in-progress game.
struct SaveData {
	// v7: one generic per-entity record (EntityState) covers monsters, items, and
	//     buttons as a diff (keyed by .ent id) or a whole spawn (no baseline);
	//     replaces the v6 split of "ent"/"monster" rows + a whole "floor" item
	//     snapshot. v6: free-look offset ("look" line); v5 folded hands into equip[].
	int version = 7;
	std::string name;         // display name (free text; may contain spaces)
	std::string currentLevel; // the level stem the party is on (where to resume)
	std::string timestamp;    // human-readable local time, for the slot list

	// --- party (no static baseline → stored whole) --------------------------
	int partyX = 0, partyZ = 0;
	int partyFacing = 2; // 0=N 1=E 2=S 3=W
	// Right-mouse free-look offset (radians) layered on the grid facing, so a
	// save mid-look returns to the EXACT camera angle — and, with looking=false,
	// replays the in-flight ease back to orthogonal. Defaults (0/0/false) =
	// orthogonal, so pre-v6 saves load square-on.
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

	// One generic per-entity record — the unified save primitive (v7). Every
	// dynamic entity kind (monster, item, button) round-trips through this, in
	// one of two modes decided by `id`:
	//   - DIFF (id >= 0): references a .ent baseline entity by its stable id, and
	//     carries only the fields that drifted from the spawn record. Applied onto
	//     the baseline the .ent load already built. `type` is empty.
	//   - SPAWN (id < 0): a runtime entity with NO .ent baseline — an editor-placed
	//     monster or a dropped item — stored WHOLE (`type` + spawn cell/facing) so
	//     a load can recreate it from nothing.
	// Per-kind mutable fields default to "unchanged from baseline": a field only
	// matters for the kind that owns it (announced/hp = monster, collected = item,
	// activated = button). hp = -1 means "not recorded" (older saves) → keep spawn
	// hp. This single record replaces v6's separate ent/monster/floor row types.
	struct EntityState {
		int id = -1;                            // .ent baseline id; < 0 = spawn
		EntityKind kind = EntityKind::Monster;  // which world list this belongs to
		std::string type;                       // catalog id (spawns); empty = diff
		int x = 0, z = 0;                        // current cell
		int spawnX = 0, spawnZ = 0;             // spawn origin (spawns only)
		int facing = 2;                         // Direction value (0=N 1=E 2=S 3=W)
		bool announced = false;                 // monster: has greeted the party
		bool aware = false;                     // monster: has noticed the party (sticky)
		float hp = -1.0f;                       // monster: current hit points
		bool collected = false;                 // item: lifted off the floor
		int slot = 0;                           // item: floor quarter slot (0..3)
		bool activated = false;                 // button: pressed / toggled on
	};

	// Dynamic state of one level: revealed cells (fog, stored whole) + the entity
	// diff/spawn list. One entry per VISITED level — the world keeps each level's
	// state so leaving and returning preserves fog/progress (P6 multi-level).
	struct LevelState {
		std::string stem;
		std::vector<std::pair<int, int>> seen;
		std::vector<EntityState> entities; // all kinds, diffs + spawns
		// v6 read compat: a v6 save stored every floor item as a whole "floor"
		// snapshot (no per-item diff). When loaded, those rows land in `entities`
		// as Item spawns and this flag is set, so ApplyActiveSnapshot REPLACES the
		// floor wholesale (v6 semantics) instead of applying item diffs. v7 saves
		// never set it.
		bool fullFloorSnapshot = false;
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
