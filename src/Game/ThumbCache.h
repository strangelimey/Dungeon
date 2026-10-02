// ============================================================================
// Game/ThumbCache.h - the rules for a grid of thumbnails that cannot all be
// loaded at once, kept in ONE place.
//
// Two pickers browse more images than the SRV heap holds (kSrvHeapCapacity =
// 1024): the editor's AssetPicker over the asset pool, and the PortraitPicker
// over the party portraits (thousands). Both follow the same rules, which used
// to live inside the AssetPicker alone:
//   - DRAWING NEVER LOADS. Draw only marks an entry seen this frame (Touch); a
//     load reads a file and uploads it, which drains the GPU, so it happens in
//     Update, a few a frame, for the tiles on screen (BeginLoad).
//   - ONE ATTEMPT PER KEY: a failed load is not retried every frame.
//   - EVICTION is least-recently-seen past the cap, never this frame's entries
//     (they are on screen), and it DRAINS THE GPU FIRST - in-flight frames may
//     still sample a texture being freed (the SRV recycling rule). It evicts
//     down to a low-water mark below the cap, so a long scroll pays that drain
//     every so often rather than on every frame once it is over the cap.
//
// The PAYLOAD is the owner's: whatever a tile needs (the texture; for a model
// tile also the mesh its icon is baked from, which must die with it).
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/GraphicsDevice.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dungeon::game {

template <typename Payload>
class ThumbCache {
public:
	struct Entry {
		Payload data{};
		u64 lastSeen = 0;
		bool tried = false;
	};

	ThumbCache(gfx::GraphicsDevice& device, size_t cap)
		: m_device(device), m_cap(cap), m_lowWater(cap - cap / 4) {}

	// One per Update: ages everything not touched since.
	void Tick() { ++m_frame; }

	// DRAW TIME: the entry for `key`, marked seen. Never loads.
	Entry& Touch(const std::string& key) {
		Entry& e = m_entries[key];
		e.lastSeen = m_frame;
		return e;
	}

	// UPDATE TIME: the entry to load into, or null when `key` has had its one
	// attempt (loaded or failed). Marks it tried and seen either way.
	Entry* BeginLoad(const std::string& key) {
		Entry& e = m_entries[key];
		if (e.tried) return nullptr;
		e.tried = true;
		e.lastSeen = m_frame;
		return &e;
	}

	Entry* Find(const std::string& key) {
		const auto it = m_entries.find(key);
		return it == m_entries.end() ? nullptr : &it->second;
	}

	// Least-recently-seen first, down to the low-water mark, once over the cap.
	void Evict() {
		if (m_entries.size() <= m_cap) return;
		std::vector<std::pair<u64, const std::string*>> aged;
		aged.reserve(m_entries.size());
		for (const auto& [key, e] : m_entries)
			if (e.lastSeen != m_frame) aged.emplace_back(e.lastSeen, &key);
		if (aged.empty()) return;
		std::ranges::sort(aged, {}, &std::pair<u64, const std::string*>::first);
		std::vector<std::string> doomed;
		for (const auto& [when, key] : aged) {
			if (m_entries.size() - doomed.size() <= m_lowWater) break;
			doomed.push_back(*key);
		}
		m_device.WaitIdle();
		for (const std::string& key : doomed) m_entries.erase(key);
	}

	// Everything, drained first for the same reason as Evict.
	void Clear() {
		if (m_entries.empty()) return;
		m_device.WaitIdle();
		m_entries.clear();
	}

	template <typename F>
	void ForEach(F&& f) const {
		for (const auto& [key, e] : m_entries) f(key, e);
	}
	size_t Size() const { return m_entries.size(); }

private:
	gfx::GraphicsDevice& m_device;
	size_t m_cap, m_lowWater;
	u64 m_frame = 0;
	std::unordered_map<std::string, Entry> m_entries;
};

} // namespace dungeon::game
