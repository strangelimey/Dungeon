// ============================================================================
// Game/Catalog.cpp — see Catalog.h.
// ============================================================================
#include "Game/Catalog.h"

#include "Assets/File.h"
#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>

namespace dungeon::game {

// CatalogEntry's field accessors are inline (delegating to serialize::); only
// the display fallback needs an out-of-line definition.
bool CatalogColor(const CatalogEntry* e, std::string_view key, Vec4& out) {
	if (!e) return false;
	const std::string s = e->Get(key, "");
	// Whitespace- and/or comma-separated, like every other list field here.
	std::vector<std::string> tok;
	for (size_t i = 0; i < s.size();) {
		while (i < s.size() &&
			   (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ','))
			++i;
		const size_t start = i;
		while (i < s.size() &&
			   !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != ',')
			++i;
		if (i > start) tok.push_back(s.substr(start, i - start));
	}
	if (tok.size() < 3) return false;
	Vec4 c{0, 0, 0, 1};
	float* dst[4] = {&c.x, &c.y, &c.z, &c.w};
	for (size_t i = 0; i < tok.size() && i < 4; ++i) {
		char* end = nullptr;
		*dst[i] = std::strtof(tok[i].c_str(), &end);
		if (end == tok[i].c_str()) return false; // leave `out` alone
	}
	out = c;
	return true;
}

std::string CatalogEntry::Display() const {
	return std::string(DisplayView());
}

std::string_view CatalogEntry::DisplayView() const {
	const std::string* v = Find("display");
	return v && !v->empty() ? std::string_view(*v) : std::string_view(id);
}

// --- tags --------------------------------------------------------------------
// Whitespace-tokenised and lowercased, so `Undead` and `undead` are one tag and
// a hand-authored list can be spaced however reads best. Commas are treated as
// whitespace too: `undead, animal` is what a person writes without thinking, and
// silently keeping "undead," as a tag that matches nothing is the sort of defect
// you only find by wondering why a generator ignored half its content.
std::vector<std::string> ParseTags(std::string_view value) {
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&] {
		if (!cur.empty()) out.push_back(std::exchange(cur, {}));
	};
	for (const char ch : value) {
		const unsigned char u = static_cast<unsigned char>(ch);
		if (std::isspace(u) || ch == ',')
			flush();
		else
			cur.push_back(static_cast<char>(std::tolower(u)));
	}
	flush();
	return out;
}

std::vector<std::string> CatalogTags(const CatalogEntry* e) {
	return e ? ParseTags(e->Get("tags", "")) : std::vector<std::string>{};
}

bool CatalogMatchesTags(const CatalogEntry* e, const std::vector<std::string>& wanted) {
	if (wanted.empty()) return true; // no tags picked: nothing is off-tag
	const std::vector<std::string> mine = CatalogTags(e);
	if (mine.empty()) return true; // untagged content fits anywhere (Catalog.h)
	for (const std::string& t : mine)
		if (std::find(wanted.begin(), wanted.end(), t) != wanted.end()) return true;
	return false;
}

// --- Catalog ----------------------------------------------------------------

void Catalog::Load(const std::string& path) {
	m_entries.clear();
	m_trailer.clear();
	auto bytes = assets::ReadBinaryFile(path);
	if (!bytes) return; // optional category: a missing file is just empty
	LoadText(std::string(bytes->begin(), bytes->end()));
}

void Catalog::LoadText(std::string_view text) {
	m_entries.clear();
	for (serialize::Block& b : serialize::ParseBlocks(text, &m_trailer)) {
		if (b.id.empty()) continue; // catalogs use only [id] blocks
		m_entries.push_back({std::move(b.id), std::move(b.lead), std::move(b.fields)});
	}
}

std::string Catalog::Serialize(std::string_view headerComment) const {
	std::vector<serialize::Block> blocks;
	blocks.reserve(m_entries.size());
	for (const CatalogEntry& e : m_entries)
		blocks.push_back({e.id, e.lead, e.fields});

	std::string text;
	// The file's own header wins: the comments that introduced its first entry,
	// or - in a catalog with no entry yet - the comments that are the whole file
	// (the trailer). It is hand-written documentation of that category's fields,
	// and prepending the generic line as well would duplicate it a little more
	// on every write.
	const bool hasOwnHeader =
		m_entries.empty() ? !m_trailer.empty() : !m_entries.front().lead.empty();
	if (!headerComment.empty() && !hasOwnHeader) {
		text += std::format("; {}{}", headerComment, serialize::kEol);
		// The blank BELOW the header separates it from the first entry, so an
		// EMPTY catalog does not get one — a file whose whole content is one
		// comment line should not grow a trailing blank the first time anything
		// saves the project. (A file that is just that one line reads back as
		// its own trailer, and then wins above; the two come out identical.)
		if (!blocks.empty()) text += serialize::kEol;
	}
	text += serialize::WriteBlocks(blocks, m_trailer);
	return text;
}

bool Catalog::Save(const std::string& path, std::string_view headerComment) const {
	const std::string text = Serialize(headerComment);
	if (!assets::WriteBinaryFile(path, text.data(), text.size())) {
		log::Warn("Could not write catalog {}", path);
		return false;
	}
	return true;
}

const CatalogEntry* Catalog::Find(std::string_view id) const {
	for (const CatalogEntry& e : m_entries)
		if (e.id == id) return &e;
	return nullptr;
}

CatalogEntry& Catalog::Add(CatalogEntry entry) {
	for (CatalogEntry& e : m_entries)
		if (e.id == entry.id) {
			e = std::move(entry);
			return e;
		}
	// THE FIRST ENTRY OF A HEADER-ONLY FILE takes the file's comments as its
	// lead, so the documentation stays ABOVE it - left as the trailer it would
	// be written under the entry, at the bottom of the file.
	if (m_entries.empty() && !m_trailer.empty()) {
		std::vector<std::string> lead = std::exchange(m_trailer, {});
		if (!lead.back().empty()) lead.emplace_back(); // a blank above the [id]
		lead.insert(lead.end(), entry.lead.begin(), entry.lead.end());
		entry.lead = std::move(lead);
	}
	m_entries.push_back(std::move(entry));
	return m_entries.back();
}

bool Catalog::Rename(std::string_view id, std::string newId) {
	if (id == newId || Contains(newId)) return false;
	for (CatalogEntry& e : m_entries)
		if (e.id == id) {
			e.id = std::move(newId);
			return true;
		}
	return false;
}

void Catalog::Remove(std::string_view id) {
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
								 [&](const CatalogEntry& e) { return e.id == id; });
	if (it == m_entries.end()) return;
	// THE FIRST ENTRY'S LEAD IS THE FILE'S HEADER (code-review C323): the
	// comments documenting the category's fields sit above the first [id], and
	// deleting that entry deleted them. They are handed on instead - to the
	// entry that is first now, above its own comments, or with none left to
	// the trailer, where a header-only file keeps its documentation. The parser
	// cannot tell where a header ends and a note on the entry itself begins, so
	// the whole lead goes: a stale line about a deleted entry is a smaller loss
	// than the file's documentation. Any other entry's lead goes with it, as a
	// field's does.
	if (it == m_entries.begin() && !it->lead.empty()) {
		std::vector<std::string> lead = std::move(it->lead);
		std::vector<std::string>& next =
			m_entries.size() > 1 ? m_entries[1].lead : m_trailer;
		if (!next.empty() && !lead.back().empty()) lead.emplace_back();
		lead.insert(lead.end(), next.begin(), next.end());
		next = std::move(lead);
	}
	m_entries.erase(it);
}

void Catalog::ClearEntries() {
	// THE HEADER STAYS (code-review C323). A new world drops the source world's
	// dungeons, quests and flags, and assigning a fresh Catalog there took the
	// file's documentation too - the template's flags.cat, quests.cat and
	// dungeons.cat are nothing BUT documentation. What goes is what describes the
	// ENTRIES: notes on them, and comments under the last one, which are about
	// them (the reason a new world drops its source's manifest comments).
	if (!m_entries.empty()) m_trailer = std::move(m_entries.front().lead);
	m_entries.clear();
}

} // namespace dungeon::game
