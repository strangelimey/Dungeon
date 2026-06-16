// ============================================================================
// Game/Catalog.cpp — see Catalog.h.
// ============================================================================
#include "Game/Catalog.h"

#include "Assets/File.h"
#include "Core/Log.h"

#include <format>

namespace dungeon::game {

// CatalogEntry's field accessors are inline (delegating to serialize::); only
// the display fallback needs an out-of-line definition.
std::string CatalogEntry::Display() const {
	const std::string* v = Find("display");
	return v && !v->empty() ? *v : id;
}

// --- Catalog ----------------------------------------------------------------

void Catalog::Load(const std::string& path) {
	m_entries.clear();
	auto bytes = assets::ReadBinaryFile(path);
	if (!bytes) return; // optional category: a missing file is just empty

	const std::string text(bytes->begin(), bytes->end());
	for (serialize::Block& b : serialize::ParseBlocks(text)) {
		if (b.id.empty()) continue; // catalogs use only [id] blocks
		m_entries.push_back({std::move(b.id), std::move(b.fields)});
	}
}

bool Catalog::Save(const std::string& path, std::string_view headerComment) const {
	std::vector<serialize::Block> blocks;
	blocks.reserve(m_entries.size());
	for (const CatalogEntry& e : m_entries)
		blocks.push_back({e.id, e.fields});

	std::string text;
	if (!headerComment.empty()) text += std::format("; {}\n\n", headerComment);
	text += serialize::WriteBlocks(blocks);

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
	m_entries.push_back(std::move(entry));
	return m_entries.back();
}

void Catalog::Remove(std::string_view id) {
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
		if (it->id == id) {
			m_entries.erase(it);
			return;
		}
}

} // namespace dungeon::game
