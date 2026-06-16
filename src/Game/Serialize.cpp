// ============================================================================
// Game/Serialize.cpp — see Serialize.h.
// ============================================================================
#include "Game/Serialize.h"

#include <charconv>
#include <format>

namespace dungeon::game::serialize {

namespace {

std::string_view Trim(std::string_view s) {
	const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
	while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
	return s;
}

} // namespace

const std::string* Find(const std::vector<Field>& fields, std::string_view key) {
	for (const Field& f : fields)
		if (f.key == key) return &f.value;
	return nullptr;
}

std::string Get(const std::vector<Field>& fields, std::string_view key,
				std::string_view fallback) {
	const std::string* v = Find(fields, key);
	return v ? *v : std::string(fallback);
}

float GetFloat(const std::vector<Field>& fields, std::string_view key, float fallback) {
	const std::string* v = Find(fields, key);
	if (!v) return fallback;
	float out = fallback;
	std::from_chars(v->data(), v->data() + v->size(), out);
	return out;
}

bool GetBool(const std::vector<Field>& fields, std::string_view key, bool fallback) {
	const std::string* v = Find(fields, key);
	if (!v || v->empty()) return fallback;
	return v->front() != '0' && v->front() != 'f' && v->front() != 'F';
}

void Set(std::vector<Field>& fields, std::string key, std::string value) {
	for (Field& f : fields)
		if (f.key == key) {
			f.value = std::move(value);
			return;
		}
	fields.push_back({std::move(key), std::move(value)});
}

std::vector<Block> ParseBlocks(std::string_view text) {
	std::vector<Block> blocks;
	blocks.push_back({}); // the leading unnamed block (manifest fields)

	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos) end = text.size();
		const std::string_view line = Trim(text.substr(pos, end - pos));
		pos = end + 1;

		if (line.empty() || line.front() == ';') continue;
		if (line.front() == '[') {
			const size_t close = line.find(']');
			if (close == std::string_view::npos) continue; // malformed header
			Block b;
			b.id = std::string(Trim(line.substr(1, close - 1)));
			blocks.push_back(std::move(b));
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) continue; // not a field
		// Append verbatim (don't dedupe) so a load → save round-trip is faithful.
		blocks.back().fields.push_back({std::string(Trim(line.substr(0, eq))),
										std::string(Trim(line.substr(eq + 1)))});
	}

	// Drop the leading unnamed block when it carried nothing, so catalogs (which
	// never use it) don't grow a stray empty block on round-trip.
	if (blocks.front().fields.empty()) blocks.erase(blocks.begin());
	return blocks;
}

std::string WriteBlocks(const std::vector<Block>& blocks) {
	std::string out;
	bool first = true;
	for (const Block& b : blocks) {
		if (!first) out += '\n';
		first = false;
		if (!b.id.empty()) out += std::format("[{}]\n", b.id);
		for (const Field& f : b.fields)
			out += std::format("{} = {}\n", f.key, f.value);
	}
	return out;
}

} // namespace dungeon::game::serialize
