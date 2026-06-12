#include "Game/Entity.h"

#include "Core/Assert.h"
#include "Core/MathTypes.h"

#include <charconv>
#include <format>

namespace dungeon::game {

int DirDX(Direction d) {
	switch (d) {
	case Direction::East: return 1;
	case Direction::West: return -1;
	default: return 0;
	}
}

int DirDZ(Direction d) {
	switch (d) {
	case Direction::North: return -1;
	case Direction::South: return 1;
	default: return 0;
	}
}

float DirYaw(Direction d) {
	switch (d) {
	case Direction::North: return kPi;
	case Direction::East: return kPi * 0.5f;
	case Direction::West: return -kPi * 0.5f;
	default: return 0.0f; // south
	}
}

const std::string* Entity::Param(std::string_view key) const {
	for (const auto& [k, v] : params)
		if (k == key) return &v;
	return nullptr;
}

namespace {

std::vector<std::string_view> SplitTokens(std::string_view line) {
	std::vector<std::string_view> tokens;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
		size_t start = i;
		while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
		if (i > start) tokens.push_back(line.substr(start, i - start));
	}
	return tokens;
}

bool ParseDirection(std::string_view token, Direction& out) {
	if (token == "north") out = Direction::North;
	else if (token == "east") out = Direction::East;
	else if (token == "south") out = Direction::South;
	else if (token == "west") out = Direction::West;
	else return false;
	return true;
}

} // namespace

Entity ParseEntityRecord(std::string_view line, std::string_view where) {
	const std::vector<std::string_view> tokens = SplitTokens(line);
	DN_ASSERT(tokens.size() >= 4,
			  std::format("entity record needs <kind> <type> <x> <z>: \"{}\" in {}",
						  line, where));

	Entity e;
	if (tokens[0] == "monster") e.kind = EntityKind::Monster;
	else if (tokens[0] == "item") e.kind = EntityKind::Item;
	else if (tokens[0] == "button") e.kind = EntityKind::Button;
	else if (tokens[0] == "decoration") e.kind = EntityKind::Decoration;
	else
		DN_ASSERT(false, std::format("unknown entity kind \"{}\" in {}", tokens[0], where));
	e.type = tokens[1];

	const auto coord = [&](std::string_view token) {
		int value = 0;
		const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
		DN_ASSERT(ec == std::errc{} && end == token.data() + token.size(),
				  std::format("bad coordinate \"{}\": \"{}\" in {}", token, line, where));
		return value;
	};
	e.x = coord(tokens[2]);
	e.z = coord(tokens[3]);

	for (size_t i = 4; i < tokens.size(); ++i) {
		if (ParseDirection(tokens[i], e.facing)) continue;
		const size_t eq = tokens[i].find('=');
		DN_ASSERT(eq != std::string_view::npos && eq > 0,
				  std::format("expected facing or key=value, got \"{}\": \"{}\" in {}",
							  tokens[i], line, where));
		e.params.emplace_back(std::string(tokens[i].substr(0, eq)),
							  std::string(tokens[i].substr(eq + 1)));
	}
	return e;
}

std::vector<std::string> ReadLevelLines(const std::vector<u8>& bytes) {
	std::vector<std::string> lines;
	std::string line;
	auto flush = [&] {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty() && line[0] != ';') lines.push_back(line);
		line.clear();
	};
	for (const u8 byte : bytes) {
		if (static_cast<char>(byte) == '\n') flush();
		else line.push_back(static_cast<char>(byte));
	}
	flush();
	return lines;
}

} // namespace dungeon::game
