// ============================================================================
// Game/SpellIdList.h - a bounded, ordered list of spell ids stored INLINE.
//
// Character keeps two kinds of spell list: the spells it has LEARNED (a set,
// grown by the first successful cast of each recipe) and each hand's
// most-recently-CAST list (front = newest). Both change on a cast, and a cast
// lands in a frame the steady-state allocation guard watches - from a hand
// click, the use menu or the spellbook. They were a flat_set<string> and a
// vector<string>, so a member's first cast of each spell cost a string per list
// it entered (a book cast enters three: learned plus both hands) and a vector
// or two growing. Same fix, same reason, as UseDefaults.h: no std::string, not
// even a short one, since the debug CRT allocates an iterator proxy for every
// std::string it constructs.
//
// Bounded on purpose. kSlots is well past the number of spells that exist (18
// today); a full list refuses a new id through Add (the caller says so) and
// forgets its OLDEST through Touch, which is what a recency list is for. An id
// longer than kIdCapacity is refused rather than cut, since a cut id would name
// a different spell.
// ============================================================================
#pragma once

#include <array>
#include <cstring>
#include <string_view>

namespace dungeon::game {

class SpellIdList {
public:
	static constexpr size_t kSlots = 32;      // distinct spells held
	static constexpr size_t kIdCapacity = 31; // bytes of a spell id

	bool Contains(std::string_view id) const { return IndexOf(id) < m_count; }

	// SET-style: appends `id` at the back. False (nothing stored) when it is
	// already held, empty, longer than kIdCapacity, or the list is full.
	bool Add(std::string_view id) {
		if (!Fits(id) || Contains(id) || m_count == kSlots) return false;
		m_slots[m_count++].Set(id);
		return true;
	}

	// RECENCY-style: moves `id` to the front, inserting it when absent (the
	// oldest - the back - is forgotten when full). False only for an id that
	// cannot be stored at all.
	bool Touch(std::string_view id) {
		if (!Fits(id)) return false;
		size_t i = IndexOf(id);
		if (i == m_count) { // new: it lands in a free slot, or over the oldest
			if (m_count < kSlots) ++m_count;
			i = m_count - 1;
			m_slots[i].Set(id); // copied BEFORE the shift below, which may move
								// the slot `id` views
		}
		const Slot moved = m_slots[i];
		for (; i > 0; --i) m_slots[i] = m_slots[i - 1];
		m_slots[0] = moved;
		return true;
	}

	void Clear() { m_count = 0; }
	bool Empty() const { return m_count == 0; }
	size_t Size() const { return m_count; }
	std::string_view operator[](size_t i) const { return m_slots[i].View(); }

	// f(id) for every id, front to back (a recency list's newest first; the
	// order a save writes and a load re-Adds).
	template <class F> void ForEach(F&& f) const {
		for (size_t i = 0; i < m_count; ++i) f(m_slots[i].View());
	}

private:
	struct Slot {
		char text[kIdCapacity + 1] = {};
		unsigned char len = 0;
		std::string_view View() const { return {text, len}; }
		void Set(std::string_view s) {
			std::memmove(text, s.data(), s.size()); // may alias its own text
			len = static_cast<unsigned char>(s.size());
		}
	};
	static bool Fits(std::string_view id) {
		return !id.empty() && id.size() <= kIdCapacity;
	}
	size_t IndexOf(std::string_view id) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_slots[i].View() == id) return i;
		return m_count;
	}

	std::array<Slot, kSlots> m_slots{};
	size_t m_count = 0;
};

} // namespace dungeon::game
