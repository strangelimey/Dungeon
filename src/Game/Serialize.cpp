// ============================================================================
// Game/Serialize.cpp — see Serialize.h.
// ============================================================================
#include "Game/Serialize.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>

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

void Remove(std::vector<Field>& fields, std::string_view key) {
	// The field's own lead comments go with it — they describe the field, and
	// left behind they would attach to whatever happened to follow.
	std::erase_if(fields, [&](const Field& f) { return f.key == key; });
}

std::string NormalizeEol(std::string text) {
	// Strip every '\r' that belongs to a "\r\n", then expand every '\n' — so any
	// mix lands on kEol exactly once and a second pass changes nothing. A lone
	// '\r' (old-Mac) is not a line ending this format has ever produced and is
	// left alone rather than guessed at.
	std::string out;
	out.reserve(text.size() + text.size() / 8);
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
		if (text[i] == '\n')
			out += kEol;
		else
			out += text[i];
	}
	return out;
}

std::vector<Block> ParseBlocks(std::string_view text) {
	std::vector<Block> blocks;
	blocks.push_back({}); // the leading unnamed block (manifest fields)

	size_t pos = 0;
	std::vector<std::string> pendingComments; // attach to the next block header
	int pendingBlanks = 0; // blank lines seen since the last field/header
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos) end = text.size();
		const std::string_view line = Trim(text.substr(pos, end - pos));
		pos = end + 1;

		if (line.empty()) {
			// BLANK LINES ARE PART OF THE FILE'S SHAPE and are kept. They are
			// what separates one group of settings from the next — a paragraph
			// break in a hand-authored sheet — and rebuilding the file without
			// them ran the balance knobs and the manifest's sections together a
			// little more on every editor save.
			//
			// Inside a comment run they go straight in; otherwise they are HELD
			// until whatever follows says what they belong to (below).
			if (!pendingComments.empty()) pendingComments.emplace_back();
			else ++pendingBlanks;
			continue;
		}
		if (line.front() == ';') { // a comment belongs to the block it introduces
			for (int i = 0; i < pendingBlanks; ++i) pendingComments.emplace_back();
			pendingBlanks = 0;
			pendingComments.emplace_back(line);
			continue;
		}
		// The held blanks belong to whatever this line is. A comment run already
		// swallowed them above, so this is the no-comment case: N blank lines
		// then a field, or then a header.
		std::vector<std::string> lead;
		lead.reserve(static_cast<size_t>(pendingBlanks) + pendingComments.size());
		for (int i = 0; i < pendingBlanks; ++i) lead.emplace_back();
		lead.insert(lead.end(), std::make_move_iterator(pendingComments.begin()),
					std::make_move_iterator(pendingComments.end()));
		pendingBlanks = 0;
		pendingComments.clear();

		if (line.front() == '[') {
			const size_t close = line.find(']');
			if (close == std::string_view::npos) continue; // malformed header
			Block b;
			b.id = std::string(Trim(line.substr(1, close - 1)));
			b.lead = std::move(lead);
			// WriteBlocks already separates blocks with one blank line, so a
			// leading blank here would grow the gap by one on every save.
			while (!b.lead.empty() && b.lead.front().empty())
				b.lead.erase(b.lead.begin());
			blocks.push_back(std::move(b));
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) continue; // not a field
		// Append verbatim (don't dedupe) so a load → save round-trip is faithful.
		blocks.back().fields.push_back({std::string(Trim(line.substr(0, eq))),
										std::string(Trim(line.substr(eq + 1))),
										std::move(lead)});
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
		if (!first) out += kEol;
		first = false;
		for (const std::string& comment : b.lead) out += std::format("{}{}", comment, kEol);
		if (!b.id.empty()) out += std::format("[{}]{}", b.id, kEol);
		for (const Field& f : b.fields) {
			for (const std::string& comment : f.lead)
				out += std::format("{}{}", comment, kEol);
			out += std::format("{} = {}{}", f.key, f.value, kEol);
		}
	}
	return out;
}

} // namespace dungeon::game::serialize
