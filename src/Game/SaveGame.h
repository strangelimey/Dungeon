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
	//
	// v2 (2026-09-24, docs/world-on-demand.md): a save names the WORLD it
	// belongs to (`save world=`). Worlds are loaded when a game starts now, so
	// Continue and Load have to know which world to load before they can read
	// anything else in the file. A v1 save cannot say, and guessing would load
	// it into the wrong world — Michael's call: it is REFUSED (the floor is 2).
	int version = 2;

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
		// A colour of its own (fx::Inst::tint - a magical torch's burn). Rides
		// enteffect / brkeffect lines as an 8th token, written only when set.
		bool tinted = false;
		float tint[3] = {0.0f, 0.0f, 0.0f};
	};

	// The GLOBAL tier: what is true of the game rather than of one dungeon —
	// where the party is in the world, elapsed time, what it has discovered, the
	// quest flags. WorldState is the same type Game holds at runtime (see
	// WorldMap.h), so there is no conversion step to keep in step.
	WorldState world;

	std::string worldName;    // the world's FOLDER name (what -project names)
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
	// The party leader's roster index (DungeonWorld::Leader). Absent in older
	// saves = 0, which is right: slot 0 leads a new game.
	int leader = 0;
	// How many members the party has ("roster" line; party creation makes 1..4).
	// 0 = a save older than party creation, which is the default four.
	size_t rosterSize = 0;

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
		// The portrait id ("portrait" line, portraits.cat). Empty = a save older
		// than portraits by id, which keeps the default party's.
		std::string portrait;
		// WHO THEY ARE (party creation): "name", "race", "color" and "pace" lines.
		// Each absent in an older save, which keeps the default party's value for
		// the slot - the save never carried them because the party never varied.
		std::string name, race;
		bool hasColor = false;
		float color[4] = {0, 0, 0, 1};
		bool hasPace = false;
		float pace = 1.0f;
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
		float charge = -1.0f;                   // item: its own charge (a torch's seconds left)
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

	// A wall torch or brazier lit or put out in play: burning is the opposite
	// of its authored `lit` (DungeonMap WallSconce::Burning). Keyed by cell +
	// wall like a niche (-1 = a brazier). Absent from older saves, which simply
	// means every fire is as authored.
	struct FireBurning {
		int x = 0, z = 0;
		int wall = -1;
		bool burning = true;
		bool empty = false; // a wall torch taken off its bracket
		// The torch mounted in it when not the fixture's own (WallSconce::torch,
		// its unlit id) and its charge. Absent from an older line = the own one.
		std::string torch;
		float torchCharge = -1.0f;
	};

	// A piece of dungeon that is HURT but still standing: its hit points, and
	// whatever is riding it (a door left alight keeps burning across a save).
	// Keyed exactly like BrokenProp, and for the same reasons. A broken piece is
	// a BrokenProp instead - it has no hp and carries nothing. Written as a
	// "damaged" line with its effects hung beneath as "brkeffect" lines, the
	// enteffect pattern; a save without them simply has nothing damaged.
	struct DamagedPiece {
		int x = 0, z = 0;
		std::string type;
		int wall = -1;
		float hp = 0.0f;
		std::vector<EffectState> effects;
	};

	struct LevelState {
		std::string stem;
		std::vector<std::pair<int, int>> seen;
		std::vector<EntityState> entities; // all kinds, diffs + spawns
		std::vector<NicheOpen> niches;     // reveal-state diffs
		std::vector<BrokenProp> broken;    // smashed props
		std::vector<FireBurning> fires;    // lit/doused diffs
		std::vector<DamagedPiece> damaged; // hurt but standing (hp + effects)
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
inline constexpr int kMinReadableVersion = 2;

// The file's HEADER: its leading "save key=value" lines, and the only part of a
// save anything needs in order to LIST it or to choose which world to load it
// into. The header is exactly those leading lines (blank and ';' lines
// skipped); a "save" line after the first record is not part of it, in either
// reader, so ReadSave and ReadSaveHeader cannot disagree about a file.
//
// version defaults to 0, NOT SaveData's current version: a file with no version
// line is refused by the floor, as the note on `version` says it is.
struct SaveHeader {
	int version = 0;
	std::string world;     // SaveData::worldName
	std::string name;      // SaveData::name
	std::string level;     // SaveData::currentLevel
	std::string timestamp; // SaveData::timestamp
};

// One save file's header, for the slot browser (cheap: parsed from the file).
struct SaveSlot {
	std::string world; // the folder of the world it belongs to
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
// Reads ONLY the header, stopping at the first record, and applies the same
// refusals ReadSave does (the version floor, a save naming no world) - so a
// header this accepts is a save ReadSave will load, and vice versa. This is
// what ListSaves runs on every menu build; the full parse is for loading.
// A refusal is logged once per session per distinct message, not per call: the
// menus ask on every Esc, and a stale save would otherwise add a warning each
// time.
std::optional<SaveHeader> ReadSaveHeader(const std::string& path);

// Every "*.dsav" in SaveDir, newest first (by timestamp string). Files that
// are refused (see ReadSaveHeader) are skipped. Empty if the folder doesn't
// exist yet. Header reads only, so it is cheap enough to ask on every menu build.
std::vector<SaveSlot> ListSaves();
// Limits ListSaves to one world's saves, for the whole process ("" = every
// world). A `-project` run sets it: that flag is how a harness or a test
// scenario opens a world, and a Continue must not carry it off into another.
void SetSaveWorldFilter(std::string world);

} // namespace dungeon::game
