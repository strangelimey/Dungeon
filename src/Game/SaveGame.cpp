// ============================================================================
// Game/SaveGame.cpp — see SaveGame.h.
// ============================================================================
#include "Game/SaveGame.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/Entity.h" // ReadLevelLines, SplitRecordTokens

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>

namespace dungeon::game {
namespace {

int IntOf(std::string_view sv) { return std::atoi(std::string(sv).c_str()); }
float FloatOf(std::string_view sv) {
	return static_cast<float>(std::atof(std::string(sv).c_str()));
}

} // namespace

std::string SaveSlotPath(const std::string& name) {
	std::string slug;
	for (char c : name) {
		const unsigned char uc = static_cast<unsigned char>(c);
		if (std::isalnum(uc))
			slug += static_cast<char>(std::tolower(uc));
		else if (c == ' ' || c == '-' || c == '_')
			slug += '_';
		// other punctuation is dropped
	}
	if (slug.empty()) slug = "save";
	return paths::SaveDir() + "\\" + slug + ".dsav";
}

bool WriteSave(const SaveData& data, const std::string& path) {
	std::string t = "; Dungeon save — dynamic level state (see SaveGame.h)\n";
	t += std::format("save version={}\n", data.version);
	t += std::format("save name={}\n", data.name);
	t += std::format("save current={}\n", data.currentLevel);
	t += std::format("save time={}\n", data.timestamp);
	t += std::format("party {} {} {}\n", data.partyX, data.partyZ, data.partyFacing);
	// Free-look offset (radians) + whether a look drag was in flight. Omitted only
	// to keep older saves lean is unnecessary — always written; pre-v6 readers
	// ignore unknown lines and pre-v6 saves simply lack it (look defaults to 0).
	t += std::format("look {:.5f} {:.5f} {}\n", data.lookYaw, data.lookPitch,
					 data.looking ? 1 : 0);
	// Tablet on the mouse cursor (party-level, not in any pack). Omitted when the
	// hand is empty; older saves lack it and default to no held item.
	if (!data.heldItem.empty()) t += std::format("held {}\n", data.heldItem);
	t += std::format("torch {}\n", data.torchPalette);

	// Empty item ids serialize as "-" so slot positions are preserved. Inventory
	// is split into its own lines (equip/pack) so the dynamic backpack and the
	// equipment set can each vary in length without ambiguity. The weapon hands
	// ride in the equip line (EquipSlot::LeftHand/RightHand).
	auto itemTok = [](const std::string& s) { return s.empty() ? std::string("-") : s; };
	for (size_t i = 0; i < data.characters.size(); ++i) {
		const SaveData::CharState& c = data.characters[i];
		t += std::format("char {} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {}\n", i,
						 c.health, c.maxHealth, c.stamina, c.maxStamina, c.mana,
						 c.maxMana, c.knownSymbols);
		t += std::format("equip {}", i);
		for (const std::string& e : c.equipment) t += " " + itemTok(e);
		t += '\n';
		// "packs <i> <selected> <type0..typeN>" — the container row + selection;
		// then one "packc <i> <packIdx> <item...>" per pack with its contents.
		t += std::format("packs {} {}", i, c.selectedPack);
		for (const std::string& p : c.packTypes) t += " " + itemTok(p);
		t += '\n';
		for (size_t p = 0; p < c.packContents.size(); ++p) {
			t += std::format("packc {} {}", i, p);
			for (const std::string& it : c.packContents[p]) t += " " + itemTok(it);
			t += '\n';
		}
		// "usedef <i> <item> <cmd> ..." — the member's remembered default uses,
		// flat (item, command) pairs. Both ids are record-token-safe (no spaces).
		if (!c.useDefaults.empty()) {
			t += std::format("usedef {}", i);
			for (const auto& [item, cmd] : c.useDefaults)
				t += std::format(" {} {}", item, cmd);
			t += '\n';
		}
		// "learned <i> <spell> ..." — the spells this member has cast at least
		// once (spells.cat ids, token-safe).
		if (!c.learnedSpells.empty()) {
			t += std::format("learned {}", i);
			for (const std::string& id : c.learnedSpells) t += " " + id;
			t += '\n';
		}
		// "mru <i> <spell> ..." — most-recently-cast, newest first (order is
		// the payload: the quick-cast list shows the head).
		if (!c.mruSpells.empty()) {
			t += std::format("mru {}", i);
			for (const std::string& id : c.mruSpells) t += " " + id;
			t += '\n';
		}
		// "effect <i> <kind> <school> <time> <duration> <magnitude> <nameKey>"
		// — one line per active status effect (v14; "-" pads an empty token).
		for (const SaveData::CharState::EffectState& e : c.effects)
			t += std::format("effect {} {} {} {:.3f} {:.3f} {:.3f} {}\n", i,
							 itemTok(e.kind), itemTok(e.school), e.time,
							 e.duration, e.magnitude, itemTok(e.nameKey));
		// "skill <i> <id> <xp> ..." / "statxp <i> <stat> <progress> ..." — the
		// member's skill XP and stat-creep pools, flat pairs (v15).
		if (!c.skills.empty()) {
			t += std::format("skill {}", i);
			for (const auto& [id, xp] : c.skills)
				t += std::format(" {} {:.3f}", id, xp);
			t += '\n';
		}
		if (!c.statProgress.empty()) {
			t += std::format("statxp {}", i);
			for (const auto& [stat, progress] : c.statProgress)
				t += std::format(" {} {:.3f}", stat, progress);
			t += '\n';
		}
		// "attr <i> <str> <dex> <vit> <will> <int>" — the five attributes
		// (v15; stat creep grows them, so the archetype defaults don't hold).
		t += std::format("attr {} {} {} {} {} {}\n", i, c.strength, c.dexterity,
						 c.vitality, c.willpower, c.intelligence);
	}

	// One block per visited level: a "level <stem>" header, then its entity
	// diff/spawn list and revealed cells. Each EntityState serializes by kind +
	// mode (see SaveData::EntityState): a baseline diff is keyed by id; a spawn
	// (id < 0) carries everything needed to recreate the entity.
	for (const SaveData::LevelState& lvl : data.levels) {
		t += std::format("level {}\n", lvl.stem);
		for (const SaveData::EntityState& e : lvl.entities) {
			switch (e.kind) {
			case EntityKind::Monster:
				if (e.id >= 0) // baseline .ent monster: store the diff, keyed by id
					t += std::format("ent {} {} {} {} {:.3f} {} {}\n", e.id, e.x, e.z,
									 e.announced ? 1 : 0, e.hp, e.aware ? 1 : 0, e.slot);
				else // editor-placed monster (no baseline): store it whole to recreate
					t += std::format("monster {} {} {} {} {} {:.3f} {} {} {} {}\n", e.type,
									 e.x, e.z, e.facing, e.announced ? 1 : 0, e.hp,
									 e.spawnX, e.spawnZ, e.aware ? 1 : 0, e.slot);
				break;
			case EntityKind::Item:
				if (e.id >= 0) // baseline rune lifted off the floor: a one-bit diff
					t += std::format("item {}\n", e.id);
				else // dropped tablet (no baseline): store it whole at its cell + slot
					t += std::format("drop {} {} {} {}\n", e.type, e.x, e.z, e.slot);
				break;
			case EntityKind::Button: // baseline button toggle, keyed by id
				t += std::format("button {} {}\n", e.id, e.activated ? 1 : 0);
				break;
			case EntityKind::Door: // door open-state diff, keyed by id
				t += std::format("door {} {}\n", e.id, e.activated ? 1 : 0);
				break;
			default: break; // decorations are static (.map) — never saved
			}
		}
		if (!lvl.seen.empty()) {
			t += "seen";
			for (const auto& [x, z] : lvl.seen) t += std::format(" {},{}", x, z);
			t += '\n';
		}
	}

	if (!assets::WriteBinaryFile(path, t.data(), t.size())) {
		log::Warn("Could not write save {}", path);
		return false;
	}
	log::Info("Saved game to {}", path);
	return true;
}

std::optional<SaveData> ReadSave(const std::string& path) {
	auto bytes = assets::ReadBinaryFile(path);
	if (!bytes) return std::nullopt;

	SaveData data;
	// The level block "ent"/"seen" lines attach to; created lazily so a legacy
	// v1 save (top-level seen/ent, no "level" record) folds into one block named
	// for the save's current level.
	SaveData::LevelState* cur = nullptr;
	auto currentBlock = [&]() -> SaveData::LevelState& {
		if (!cur) {
			data.levels.push_back({data.currentLevel.empty() ? "level1"
															  : data.currentLevel, {}, {}});
			cur = &data.levels.back();
		}
		return *cur;
	};

	for (const std::string& line : ReadLevelLines(*bytes)) {
		// "save key=value" header lines (value may contain spaces, so don't
		// tokenize — take everything after '=').
		if (line.starts_with("save ")) {
			const size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			const std::string key = line.substr(5, eq - 5);
			const std::string val = line.substr(eq + 1);
			if (key == "version")      data.version = std::atoi(val.c_str());
			else if (key == "name")    data.name = val;
			else if (key == "current") data.currentLevel = val;
			else if (key == "level")   data.currentLevel = val; // v1 legacy key
			else if (key == "time")    data.timestamp = val;
			continue;
		}

		const std::vector<std::string_view> tok = SplitRecordTokens(line);
		if (tok.empty()) continue;
		const std::string_view kw = tok[0];

		if (kw == "party" && tok.size() >= 4) {
			data.partyX = IntOf(tok[1]);
			data.partyZ = IntOf(tok[2]);
			data.partyFacing = IntOf(tok[3]);
		} else if (kw == "look" && tok.size() >= 4) {
			data.lookYaw = FloatOf(tok[1]);
			data.lookPitch = FloatOf(tok[2]);
			data.looking = IntOf(tok[3]) != 0;
		} else if (kw == "held" && tok.size() >= 2) {
			data.heldItem = std::string(tok[1]);
		} else if (kw == "torch" && tok.size() >= 2) {
			data.torchPalette = IntOf(tok[1]);
		} else if (kw == "char" && tok.size() >= 8) {
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			c.health = FloatOf(tok[2]);    c.maxHealth = FloatOf(tok[3]);
			c.stamina = FloatOf(tok[4]);   c.maxStamina = FloatOf(tok[5]);
			c.mana = FloatOf(tok[6]);      c.maxMana = FloatOf(tok[7]);
			if (tok.size() >= 9) // older saves omit the spell mask
				c.knownSymbols = static_cast<u32>(IntOf(tok[8]));
		} else if (kw == "equip" && tok.size() >= 2) {
			// Worn doll + the two weapon hands (EquipSlot order); "-" = empty.
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			auto detok = [](std::string_view sv) {
				return sv == "-" ? std::string() : std::string(sv);
			};
			for (size_t i = 2; i < tok.size(); ++i) c.equipment.push_back(detok(tok[i]));
		} else if (kw == "packs" && tok.size() >= 3) {
			// Pack-row containers + selection: "packs <i> <selected> <types...>".
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			c.selectedPack = IntOf(tok[2]);
			auto detok = [](std::string_view sv) {
				return sv == "-" ? std::string() : std::string(sv);
			};
			for (size_t i = 3; i < tok.size(); ++i) c.packTypes.push_back(detok(tok[i]));
			c.packContents.resize(c.packTypes.size());
		} else if (kw == "usedef" && tok.size() >= 4) {
			// Default-use pairs: "usedef <i> <item> <cmd> ..." (v10).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			for (size_t i = 2; i + 1 < tok.size(); i += 2)
				c.useDefaults.emplace_back(std::string(tok[i]), std::string(tok[i + 1]));
		} else if (kw == "learned" && tok.size() >= 3) {
			// Learned spells: "learned <i> <spell> ..." (v11).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			for (size_t i = 2; i < tok.size(); ++i)
				c.learnedSpells.emplace_back(tok[i]);
		} else if (kw == "effect" && tok.size() >= 7) {
			// Status effect: "effect <i> <kind> <school> <time> <duration>
			// <magnitude> <nameKey>" (v14); "-" = empty token.
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			auto detok = [](std::string_view sv) {
				return sv == "-" ? std::string() : std::string(sv);
			};
			SaveData::CharState::EffectState e;
			e.kind = detok(tok[2]);
			e.school = detok(tok[3]);
			e.time = FloatOf(tok[4]);
			e.duration = FloatOf(tok[5]);
			e.magnitude = FloatOf(tok[6]);
			if (tok.size() >= 8) e.nameKey = detok(tok[7]);
			c.effects.push_back(std::move(e));
		} else if (kw == "shield" && tok.size() >= 5) {
			// v13 ward line: "shield <i> <school> <time> <power>". Loads as a
			// ward effect — duration = the remaining time (the HUD fraction
			// starts full), display name derived from the school (the four
			// Protect shields are the only wards a v13 save can carry).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			SaveData::CharState::EffectState e;
			e.kind = "ward";
			e.school = std::string(tok[2]);
			e.time = FloatOf(tok[3]);
			e.duration = e.time;
			e.magnitude = FloatOf(tok[4]);
			e.nameKey = e.school == "earth"   ? "spell.stoneskin"
						: e.school == "fire"  ? "spell.fireshield"
						: e.school == "water" ? "spell.waterveil"
											  : "spell.windward";
			c.effects.push_back(std::move(e));
		} else if (kw == "attr" && tok.size() >= 7) {
			// The five attributes: "attr <i> <str> <dex> <vit> <will> <int>" (v15).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			c.hasAttrs = true;
			c.strength = IntOf(tok[2]);
			c.dexterity = IntOf(tok[3]);
			c.vitality = IntOf(tok[4]);
			c.willpower = IntOf(tok[5]);
			c.intelligence = IntOf(tok[6]);
		} else if (kw == "skill" && tok.size() >= 4) {
			// Skill XP pairs: "skill <i> <id> <xp> ..." (v15).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			for (size_t i = 2; i + 1 < tok.size(); i += 2)
				c.skills.emplace_back(std::string(tok[i]), FloatOf(tok[i + 1]));
		} else if (kw == "statxp" && tok.size() >= 4) {
			// Stat-creep pools: "statxp <i> <stat> <progress> ..." (v15).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			for (size_t i = 2; i + 1 < tok.size(); i += 2)
				c.statProgress.emplace_back(std::string(tok[i]), FloatOf(tok[i + 1]));
		} else if (kw == "mru" && tok.size() >= 3) {
			// Spell MRU: "mru <i> <spell> ..." newest first (v12).
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			for (size_t i = 2; i < tok.size(); ++i)
				c.mruSpells.emplace_back(tok[i]);
		} else if (kw == "packc" && tok.size() >= 3) {
			// One pack's contents: "packc <i> <packIdx> <items...>".
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			const size_t p = static_cast<size_t>(IntOf(tok[2]));
			if (p >= c.packContents.size()) c.packContents.resize(p + 1);
			auto detok = [](std::string_view sv) {
				return sv == "-" ? std::string() : std::string(sv);
			};
			for (size_t i = 3; i < tok.size(); ++i)
				c.packContents[p].push_back(detok(tok[i]));
		} else if (kw == "level" && tok.size() >= 2) {
			data.levels.push_back({std::string(tok[1]), {}, {}});
			cur = &data.levels.back();
		} else if (kw == "ent" && tok.size() >= 4) {
			// Baseline monster diff: id x z [announced] [hp] [aware] [slot].
			SaveData::EntityState e;
			e.kind = EntityKind::Monster;
			e.id = IntOf(tok[1]);
			e.x = IntOf(tok[2]);
			e.z = IntOf(tok[3]);
			if (tok.size() >= 5) e.announced = IntOf(tok[4]) != 0; // older saves omit it
			if (tok.size() >= 6) e.hp = FloatOf(tok[5]);           // older saves omit it
			if (tok.size() >= 7) e.aware = IntOf(tok[6]) != 0;     // older saves omit it
			if (tok.size() >= 8) e.slot = IntOf(tok[7]);           // older saves omit it
			currentBlock().entities.push_back(e);
		} else if (kw == "monster" && tok.size() >= 9) {
			// Whole editor-placed monster:
			// type x z facing announced hp spawnX spawnZ [aware] [slot].
			SaveData::EntityState e;
			e.kind = EntityKind::Monster;
			e.id = -1; // editor-placed (no .ent baseline)
			e.type = std::string(tok[1]);
			e.x = IntOf(tok[2]);
			e.z = IntOf(tok[3]);
			e.facing = IntOf(tok[4]);
			e.announced = IntOf(tok[5]) != 0;
			e.hp = FloatOf(tok[6]);
			e.spawnX = IntOf(tok[7]);
			e.spawnZ = IntOf(tok[8]);
			if (tok.size() >= 10) e.aware = IntOf(tok[9]) != 0;    // older saves omit it
			if (tok.size() >= 11) e.slot = IntOf(tok[10]);         // older saves omit it
			currentBlock().entities.push_back(e);
		} else if (kw == "item" && tok.size() >= 2) {
			// Baseline rune lifted off the floor (v7 diff): just the id.
			SaveData::EntityState e;
			e.kind = EntityKind::Item;
			e.id = IntOf(tok[1]);
			e.collected = true;
			currentBlock().entities.push_back(e);
		} else if (kw == "drop" && tok.size() >= 4) {
			// Dropped tablet (v7 spawn): type x z [slot], no baseline.
			SaveData::EntityState e;
			e.kind = EntityKind::Item;
			e.id = -1;
			e.type = std::string(tok[1]);
			e.x = IntOf(tok[2]);
			e.z = IntOf(tok[3]);
			if (tok.size() >= 5) e.slot = IntOf(tok[4]); // older saves omit it
			currentBlock().entities.push_back(e);
		} else if (kw == "button" && tok.size() >= 3) {
			// Baseline button toggle (v7 diff): id activated.
			SaveData::EntityState e;
			e.kind = EntityKind::Button;
			e.id = IntOf(tok[1]);
			e.activated = IntOf(tok[2]) != 0;
			currentBlock().entities.push_back(e);
		} else if (kw == "door" && tok.size() >= 3) {
			// Door open-state diff: id open (activated carries "open").
			SaveData::EntityState e;
			e.kind = EntityKind::Door;
			e.id = IntOf(tok[1]);
			e.activated = IntOf(tok[2]) != 0;
			currentBlock().entities.push_back(e);
		} else if (kw == "floor" && tok.size() >= 4) {
			// v6 compat: a whole floor snapshot (type x z per uncollected item).
			// Land it as an Item spawn and flag the block so apply REPLACES the
			// floor wholesale (v6 had no per-item diff).
			SaveData::EntityState e;
			e.kind = EntityKind::Item;
			e.id = -1;
			e.x = IntOf(tok[1]);
			e.z = IntOf(tok[2]);
			e.type = std::string(tok[3]);
			SaveData::LevelState& lvl = currentBlock();
			lvl.entities.push_back(e);
			lvl.fullFloorSnapshot = true;
		} else if (kw == "seen") {
			SaveData::LevelState& lvl = currentBlock();
			for (size_t i = 1; i < tok.size(); ++i) {
				const size_t comma = tok[i].find(',');
				if (comma == std::string_view::npos) continue;
				lvl.seen.emplace_back(IntOf(tok[i].substr(0, comma)),
									  IntOf(tok[i].substr(comma + 1)));
			}
		}
	}
	return data;
}

std::vector<SaveSlot> ListSaves() {
	std::vector<SaveSlot> slots;
	std::error_code ec;
	const std::filesystem::path dir(paths::SaveDir());
	if (!std::filesystem::is_directory(dir, ec)) return slots;

	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec) break;
		if (!entry.is_regular_file() || entry.path().extension() != ".dsav")
			continue;
		const std::string path = entry.path().string();
		if (auto data = ReadSave(path))
			slots.push_back({data->name, data->currentLevel, data->timestamp, path});
	}
	// Newest first — the timestamp strings sort lexicographically by time.
	std::ranges::sort(slots, std::ranges::greater{}, &SaveSlot::timestamp);
	return slots;
}

} // namespace dungeon::game
