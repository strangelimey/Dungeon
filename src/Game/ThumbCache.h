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
//   - EVICTION is least-recently-seen past the cap, NEVER A TILE ON SCREEN, and
//     it DRAINS THE GPU FIRST - in-flight frames may still sample a texture
//     being freed (the SRV recycling rule). It evicts down to a low-water mark
//     below the cap, so a long scroll pays that drain every so often rather
//     than on every frame once it is over the cap.
//
// WHAT "ON SCREEN" MEANS, and why the owner must say it (code-review C111).
// The order of a frame is Update (Tick, loads, Evict) and THEN Draw, so what a
// draw marked seen (Touch) carries the PREVIOUS frame's stamp by the time
// Evict looks - indistinguishable from any other aged entry. Only fresh loads
// were protected, and tiles on screen survived only by being the newest: past a
// screenful larger than the low-water mark (a 5K window), eviction took tiles
// that were showing, they blanked, reloaded and were taken again. So the owner
// KEEPs every tile it shows in Update, before Evict; BeginLoad stamps whatever
// it is asked about too, loaded or not. And the cap GROWS with what is on
// screen (twice it), so a screen bigger than the cap still keeps a screenful
// of scroll-back rather than draining every frame. A draw must Touch only what
// SHOWS: the asset picker's tiles used to be drawn (under the scissor) and so
// marked seen all of them, every frame - over the cap, eviction then dropped
// and the draw re-made the off-screen ones each frame, a GPU drain every time.
//
// THE HEAP HAS THE LAST SAY. Every thumbnail is an SRV slot, and running the
// heap out is an abort, not a slowdown - so a cap that grows with the screen
// must not grow past it (a 6K window of portraits shows ~450 tiles; twice that
// plus what the game already holds is past 1024). No thumbnail loads once the
// heap is at kHeapLine (an eighth of it left for whatever else loads while a
// picker is open - its preview's model and maps - and under the 90% warning);
// the load is asked for again next frame (it is not "tried"), and Evict, told a
// load went without, bounds the cap by the room under the line and drops what
// it can of the OFF-screen ones to make room. A screen with more tiles than the
// line leaves room for (an 8K window) shows the rest blank rather than aborting.
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace dungeon::game {

// The dev knob `thumbcap` (code-review C111): when non-zero, EVERY ThumbCache
// evicts against this cap instead of its own, and does NOT grow it to what is
// on screen - so a cap below the visible count can be forced, and the rule that
// no tile on screen is evicted is what is left holding the grid up. The heap
// line still bounds it.
struct ThumbCacheKnobs {
	static inline size_t capOverride = 0;
	// `thumbcap heap <n>`: the SRV count thumbnails stop loading at, in place
	// of kHeapLine - so a check can put the line just above what is live and
	// watch the caches reach it (0 = the real line).
	static inline u32 heapLineOverride = 0;

	// The SRV count no thumbnail loads past (see the header): 896 of 1024.
	static constexpr u32 kHeapLine = gfx::kSrvHeapCapacity - gfx::kSrvHeapCapacity / 8;
	static u32 HeapLine() { return heapLineOverride ? heapLineOverride : kHeapLine; }
};

// What a ThumbCache did since its last Clear (the pickers' `status` lines).
struct ThumbCounts {
	size_t onScreen = 0; // entries kept this frame, at the last Evict
	size_t cap = 0;      // the cap that Evict used
	size_t evicted = 0;  // entries evicted
	// Loads granted to a key that had been EVICTED: a tile that went and was
	// wanted again. Scrolling back does it honestly; with the view standing
	// still, every one is a tile evicted while it was on screen.
	size_t reloads = 0;
	// Loads turned away because the heap was at the line (asked again later).
	size_t refused = 0;
	u32 heapLine = 0; // the line in force at the last Evict
	// The most SRVs live at any Evict (after that frame's loads) under that line.
	u32 heapTop = 0;
};

template <typename Payload>
class ThumbCache {
public:
	struct Entry {
		Payload data{};
		u64 lastSeen = 0;
		bool tried = false;
	};

	ThumbCache(gfx::GraphicsDevice& device, size_t cap) : m_device(device), m_cap(cap) {}

	// One per Update: ages everything not touched since.
	void Tick() { ++m_frame; }

	// DRAW TIME: the entry for `key`, marked seen. Never loads.
	Entry& Touch(const std::string& key) {
		Entry& e = m_entries[key];
		e.lastSeen = m_frame;
		return e;
	}

	// UPDATE TIME: `key` is on screen this frame - Evict leaves it alone. Every
	// tile the grid shows, before Evict (see the header). A key with no entry yet
	// has nothing to protect.
	void Keep(const std::string& key) {
		if (const auto it = m_entries.find(key); it != m_entries.end())
			it->second.lastSeen = m_frame;
	}

	// UPDATE TIME: the entry to load into, or null when `key` has had its one
	// attempt (loaded or failed) - or when the heap is at the line, in which
	// case it is NOT tried, is asked for again, and the next Evict makes room.
	// Marks it seen either way.
	Entry* BeginLoad(const std::string& key) {
		Entry& e = m_entries[key];
		e.lastSeen = m_frame;
		if (e.tried) return nullptr;
		if (m_device.SrvLive() >= ThumbCacheKnobs::HeapLine()) {
			++m_counts.refused;
			m_starved = true;
			return nullptr;
		}
		e.tried = true;
		if (m_gone.erase(key) > 0) ++m_counts.reloads;
		return &e;
	}

	Entry* Find(const std::string& key) {
		const auto it = m_entries.find(key);
		return it == m_entries.end() ? nullptr : &it->second;
	}
	// A look that does not count as seen (a reader outside the grid).
	const Entry* Find(const std::string& key) const {
		const auto it = m_entries.find(key);
		return it == m_entries.end() ? nullptr : &it->second;
	}

	// Least-recently-seen first, down to the low-water mark, once over the cap -
	// or once a load went without for want of heap. What was kept or loaded THIS
	// frame is on screen and never a candidate.
	void Evict() {
		std::vector<std::pair<u64, const std::string*>> aged;
		aged.reserve(m_entries.size());
		size_t onScreen = 0;
		for (const auto& [key, e] : m_entries) {
			if (e.lastSeen == m_frame) ++onScreen;
			else aged.emplace_back(e.lastSeen, &key);
		}
		size_t cap = ThumbCacheKnobs::capOverride ? ThumbCacheKnobs::capOverride
												  : std::max(m_cap, 2 * onScreen);
		// The heap bounds it: no more than is held now plus the room under the
		// line. That alone never asks for an eviction (what is held is within
		// it); it is what makes a STARVED cache's low-water mark one it can reach.
		const u32 live = m_device.SrvLive();
		const u32 line = ThumbCacheKnobs::HeapLine();
		cap = std::min(cap, m_entries.size() + (live < line ? line - live : 0));
		const bool starved = std::exchange(m_starved, false);
		m_counts.onScreen = onScreen;
		m_counts.cap = cap;
		if (line != m_counts.heapLine) m_counts.heapTop = 0; // a new line, its own top
		m_counts.heapLine = line;
		m_counts.heapTop = std::max(m_counts.heapTop, live);
		if ((m_entries.size() <= cap && !starved) || aged.empty()) return;
		const size_t lowWater = cap - cap / 4;
		std::ranges::sort(aged, {}, &std::pair<u64, const std::string*>::first);
		std::vector<std::string> doomed;
		for (const auto& [when, key] : aged) {
			if (m_entries.size() - doomed.size() <= lowWater) break;
			doomed.push_back(*key);
		}
		if (doomed.empty()) return;
		m_device.WaitIdle();
		for (std::string& key : doomed) {
			// Only an entry that had its load counts as GONE: one a draw marked
			// and no load reached yet held nothing to lose.
			const auto it = m_entries.find(key);
			if (it == m_entries.end()) continue;
			const bool held = it->second.tried;
			m_entries.erase(it);
			if (held) m_gone.insert(std::move(key));
		}
		m_counts.evicted += doomed.size();
	}

	// Everything, drained first for the same reason as Evict. The counts start
	// again: they describe one browse.
	void Clear() {
		m_counts = {};
		m_starved = false;
		m_gone.clear();
		if (m_entries.empty()) return;
		m_device.WaitIdle();
		m_entries.clear();
	}

	template <typename F>
	void ForEach(F&& f) const {
		for (const auto& [key, e] : m_entries) f(key, e);
	}
	template <typename F>
	void ForEach(F&& f) {
		for (auto& [key, e] : m_entries) f(key, e);
	}
	size_t Size() const { return m_entries.size(); }
	const ThumbCounts& Counts() const { return m_counts; }
	// The Tick count: a client that must hold something for a few frames (a
	// bake's source, until the GPU has drawn it) dates it with this.
	u64 Frame() const { return m_frame; }

private:
	gfx::GraphicsDevice& m_device;
	size_t m_cap;
	u64 m_frame = 0;
	std::unordered_map<std::string, Entry> m_entries;
	// Keys evicted since the last Clear, so a load of one again counts as a
	// reload. Bounded by the keys the grid can show.
	std::unordered_set<std::string> m_gone;
	ThumbCounts m_counts;
	bool m_starved = false; // a load went without since the last Evict (the heap line)
};

} // namespace dungeon::game
