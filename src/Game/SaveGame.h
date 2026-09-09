// ============================================================================
// Game/SaveGame.h — the save file: the level's dynamic state, on disk.
//
// A save NEVER stores the static layer (DungeonMap / the .map file) — only the
// things that can change during play. Loading reconstructs the level from its
// .map + .ent baseline, then applies a save on top:
//   - State with NO static baseline (party pose, cursor-held item, fog of war,
//     character resources, torch palette) is stored whole — born at runtime.
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
#include "Game/WorldMap.h" // WorldState (the save's GLOBAL tier)

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

// The serializable dynamic state of one in-progress game.
struct SaveData {
	// THE VERSION STARTS AGAIN AT 1 (Michael, 2026-09-09). The ladder that used
	// to stand here ran v1..v26, each rung a note on how to read the rung below
	// — and none of it can be read any more: the world tier re-cut what a save
	// IS, and no save written before it will ever load. Keeping twenty-six
	// comments explaining how to migrate files that cannot exist would be a
	// museum, not documentation. The old notes are in the git history if a
	// format question ever needs archaeology.
	//
	// So this is version 1 of the save format as it now stands, and
	// kMinReadableVersion is 1: anything else is refused outright rather than
	// half-understood. The next real format change starts a new ladder, and
	// that ladder will be worth keeping.
	int version = 1;

	// ONE active status effect, as it survives a save. Shared by both sides —
	// a party member's list and a monster's — because the effects system
	// is symmetric and the record has no reason not to be. `id` names the
	// effect kind; `nameKey` is written
	// for readability only (a kind names itself on load); `source` is the
	// roster index credited with a DoT's ticks, or -1.
	struct EffectState {
		std::string id;
		std::string school;
		float time = 0.0f;
		float duration = 0.0f;
		float magnitude = 0.0f;
		int source = -1;
		std::string nameKey;
	};

	// The GLOBAL tier: what is true of the game rather than of one dungeon —
	// where the party is in the world, elapsed time, what it has discovered, the
	// quest flags. WorldState is the same type Game holds at runtime (see
	// WorldMap.h), so there is no conversion step to keep in step.
	WorldState world;

	std::string name;         // display name (free text; may contain spaces)
	std::string currentLevel; // the level stem the party is on (where to resume)
	std::string timestamp;    // human-readable local time, for the slot list

	// --- party (no static baseline → stored whole) --------------------------
	int partyX = 0, partyZ = 0;
	int partyFacing = 2; // 0=N 1=E 2=S 3=W
	// Right-mouse free-look offset (radians) layered on the grid facing, so a
	// save mid-look returns to the EXACT camera angle — and, with looking=false,
	// replays the in-flight ease back to orthogonal. Defaults (0/0/false) =
	// orthogonal, so older saves load square-on.
	float lookYaw = 0.0f, lookPitch = 0.0f;
	bool looking = false;

	// Item floating on the mouse cursor (catalog id; "" = empty hand). Party-level
	// state, not anyone's inventory — born at runtime, stored whole.
	std::string heldItem;

	int torchPalette = 0; // HUD torchlight index (0 warm, 1 cold, 2 eerie)

	// Per-roster-slot mutable resources, in roster order.
	struct CharState {
		float health = 1, maxHealth = 1;
		float stamina = 1, maxStamina = 1;
		float mana = 1, maxMana = 1;
		u32 knownSymbols = 0; // memorized spell symbols (SymbolBit mask)
		// Carried/worn items, by catalog id ("" = empty). Inventory travels with
		// the party (not per-level). equipment is in EquipSlot order and includes
		// the two weapon hands (LeftHand/RightHand). packTypes is the pack-row
		// container ids; packContents[i] is pack i's items (parallel); selectedPack
		// is the active container.
		std::vector<std::string> equipment;
		std::vector<std::string> packTypes;
		std::vector<std::vector<std::string>> packContents;
		int selectedPack = 0;
		// Remembered default use PER HAND (0 = left, 1 = right) per item type
		// (Character::useDefaults), as (item id, command id) pairs. Absent in
		// older saves (defaults reset); a a flat "usedef" line loads
		// into BOTH hands.
		std::array<std::vector<std::pair<std::string, std::string>>, 2> useDefaults;
		// Spells learned by first successful cast (Character::learnedSpells),
		// spells.cat ids. Absent in older saves (re-learn by casting).
		std::vector<std::string> learnedSpells;
		// Most-recently-cast spells PER HAND, newest first
		// (Character::spellMru) — each hand's Magic quick-cast list. Absent
		// in older saves (rebuilds by casting); a a flat "mru" line
		// loads into BOTH hands.
		std::array<std::vector<std::string>, 2> mruSpells;
		// The offense share (Character::offenseShare) — how much of the
		// character's skill goes into attacking, the rest held back to guard
		// with. 1.0 = all-out (the default, and what every an older save
		// means). NOT clamped to 1: over-exertion goes past it.
		float offenseShare = 1.0f;
		// Active status effects (Character::effects) — one "effect" line each
		//. A v13 "shield" line loads as the matching ward (duration =
		// time left, name derived from the school); older saves carry none.
		std::vector<EffectState> effects;
		// Skill XP by skill id (Character::skillXp) and the stat-creep pools
		// by stat id (Character::statProgress) — docs/skills.md. Flat pairs,
		// one "skill"/"statxp" line each. Absent in older saves.
		std::vector<std::pair<std::string, float>> skills;
		std::vector<std::pair<std::string, float>> statProgress;
		// The five attributes ("attr" line) — stat creep grows them, so
		// they round-trip. hasAttrs=false (older save) keeps the archetype's.
		bool hasAttrs = false;
		int strength = 0, dexterity = 0, vitality = 0, willpower = 0,
			intelligence = 0;
		// The resource BASES ("base" line) — the authored half of the
		// derived maxima. hasBases=false (older save) back-solves them from
		// the stored maxima at apply time.
		bool hasBases = false;
		float baseHealth = 0, baseStamina = 0, baseMana = 0;
		// FOOD and WATER ("supply" line). hasSupplies=false (an older save)
		// arrives FULL rather than empty — a party loaded from a v24 save has
		// not been starving off-screen, and defaulting a new meter to zero would
		// have every existing save open onto four members taking damage.
		bool hasSupplies = false;
		float food = 0, water = 0;
		// DEAD (v18, "dead" line, written only when set): a downed member who
		// took deliberate overkill — never self-stabilizes. Absent = alive or
		// unconscious.
		bool dead = false;
	};
	std::vector<CharState> characters;

	// One generic per-entity record — the unified save primitive. Every
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
		int slot = 0;                           // sub-cell slot: item quarter (0..3)
												// or monster slot on its size's grid
		int niche = -1;                         // item: wall niche it sits in (-1 = floor)
		bool activated = false;                 // button: pressed / toggled on
		std::array<float, 4> threat{};          // monster: per-member aggro
		int threatLock = -1;                    // monster: locked member
		// monster: what it is currently afflicted by — the same record a
		// member's effects use, written as "enteffect" lines under its own.
		std::vector<EffectState> effects;
	};

	// Dynamic state of one level: revealed cells (fog, stored whole) + the entity
	// diff/spawn list. One entry per VISITED level — the world keeps each level's
	// state so leaving and returning preserves fog/progress (P6 multi-level).
	// A wall niche whose runtime open state drifted from its authored default
	// (open != !hidden) — e.g. a secret niche a button opened. Keyed by cell +
	// wall (a niche is registered on its floor cell, facing a wall direction).
	struct NicheOpen {
		int x = 0, z = 0;
		int wall = 0; // Direction as int
		bool open = true;
	};

	// A piece of DUNGEON that has been broken. Doors ride `entities` like
	// every other .ent record, so this is for the pieces that do NOT: decorations
	// are STATIC .map records, and their destroyed state is dynamic — the same
	// split `seen` makes.
	//
	// KEYED BY CELL + TYPE, deliberately not by index into the prop list. An index
	// is only stable until the editor inserts or removes a record, at which point
	// every saved index past it would name the wrong prop; a cell and a type name
	// the thing itself. Several props can share a cell, but not two of the same
	// TYPE in one cell, which is what makes the pair unique enough.
	struct BrokenProp {
		int x = 0, z = 0;
		std::string type;
		// Which wall, for a FIXTURE: several sconces may share a cell on different
		// walls, so the cell and type alone do not name one. -1 for anything that
		// has no wall — a prop, a door, a floor-standing brazier. Written as a
		// fourth token, and a v24 line without it reads as -1.
		int wall = -1;
	};

	struct LevelState {
		std::string stem;
		std::vector<std::pair<int, int>> seen;
		std::vector<EntityState> entities; // all kinds, diffs + spawns
		std::vector<NicheOpen> niches;     // reveal-state diffs
		std::vector<BrokenProp> broken;    // smashed props
	};
	// One entry per VISITED level, keyed by STEM.
	//
	// The plan called for these to become per-dungeon-per-level, and building it
	// said not to: a stem already names a level uniquely across the project, and
	// P1's checker refuses a level claimed by two dungeons — so which dungeon a
	// state belongs to is DERIVABLE. Storing it too would be a second source of
	// truth that can disagree with the first, and the global/local split the
	// design asks for is about WHAT IS SAVED WHERE, not about nesting.
	std::vector<LevelState> levels;
};

// The oldest save this build will read. Below it ReadSave refuses rather than
// half-loading — see the note on `version` above.
inline constexpr int kMinReadableVersion = 1;

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
