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
	t += std::format("save level={}\n", data.level);
	t += std::format("save time={}\n", data.timestamp);
	t += std::format("party {} {} {}\n", data.partyX, data.partyZ, data.partyFacing);
	t += std::format("torch {}\n", data.torchPalette);

	for (size_t i = 0; i < data.characters.size(); ++i) {
		const SaveData::CharState& c = data.characters[i];
		t += std::format("char {} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}\n", i,
						 c.health, c.maxHealth, c.stamina, c.maxStamina, c.mana,
						 c.maxMana);
	}
	for (const SaveData::EntityState& e : data.entities)
		t += std::format("ent {} {} {}\n", e.id, e.x, e.z);

	if (!data.seen.empty()) {
		t += "seen";
		for (const auto& [x, z] : data.seen) t += std::format(" {},{}", x, z);
		t += '\n';
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
	for (const std::string& line : ReadLevelLines(*bytes)) {
		// "save key=value" header lines (value may contain spaces, so don't
		// tokenize — take everything after '=').
		if (line.starts_with("save ")) {
			const size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			const std::string key = line.substr(5, eq - 5);
			const std::string val = line.substr(eq + 1);
			if (key == "version")    data.version = std::atoi(val.c_str());
			else if (key == "name")  data.name = val;
			else if (key == "level") data.level = val;
			else if (key == "time")  data.timestamp = val;
			continue;
		}

		const std::vector<std::string_view> tok = SplitRecordTokens(line);
		if (tok.empty()) continue;
		const std::string_view kw = tok[0];

		if (kw == "party" && tok.size() >= 4) {
			data.partyX = IntOf(tok[1]);
			data.partyZ = IntOf(tok[2]);
			data.partyFacing = IntOf(tok[3]);
		} else if (kw == "torch" && tok.size() >= 2) {
			data.torchPalette = IntOf(tok[1]);
		} else if (kw == "char" && tok.size() >= 8) {
			const size_t idx = static_cast<size_t>(IntOf(tok[1]));
			if (idx >= data.characters.size()) data.characters.resize(idx + 1);
			SaveData::CharState& c = data.characters[idx];
			c.health = FloatOf(tok[2]);    c.maxHealth = FloatOf(tok[3]);
			c.stamina = FloatOf(tok[4]);   c.maxStamina = FloatOf(tok[5]);
			c.mana = FloatOf(tok[6]);      c.maxMana = FloatOf(tok[7]);
		} else if (kw == "ent" && tok.size() >= 4) {
			SaveData::EntityState e;
			e.id = IntOf(tok[1]);
			e.x = IntOf(tok[2]);
			e.z = IntOf(tok[3]);
			data.entities.push_back(e);
		} else if (kw == "seen") {
			for (size_t i = 1; i < tok.size(); ++i) {
				const size_t comma = tok[i].find(',');
				if (comma == std::string_view::npos) continue;
				data.seen.emplace_back(IntOf(tok[i].substr(0, comma)),
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
			slots.push_back({data->name, data->level, data->timestamp, path});
	}
	// Newest first — the timestamp strings sort lexicographically by time.
	std::ranges::sort(slots, std::ranges::greater{}, &SaveSlot::timestamp);
	return slots;
}

} // namespace dungeon::game
