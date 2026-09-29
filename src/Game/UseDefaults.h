// ============================================================================
// Game/UseDefaults.h - one hand's remembered default uses (item type -> command).
//
// Character::useDefaults holds one per hand. A pick is recorded from the hand
// use menu mid-game, in a frame the steady-state allocation guard watches, so
// this stores its text INLINE in a fixed number of slots: no std::string, not
// even a short one, because the debug CRT allocates an iterator proxy for
// every std::string it constructs. It was a flat_map<string, string>, and the
// first pick for each item type cost six allocations there.
//
// Bounded on purpose. Past kSlots item types a hand forgets its OLDEST pick
// (the item goes back to UNSET, as a never-picked one is, and a left click
// falls back to its first command). An
// id longer than kTextCapacity is refused rather than cut, since a cut id
// would name a different item; every catalog id today is under 16 characters.
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace dungeon::game {

class UseDefaults {
public:
	static constexpr size_t kSlots = 32;         // item types remembered per hand
	static constexpr size_t kTextCapacity = 47;  // bytes of an item id or command

	// The command picked for `item`, or empty when there is none.
	std::string_view Find(std::string_view item) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_slots[i].Item() == item) return m_slots[i].Cmd();
		return {};
	}

	// Records `cmd` for `item`, replacing an earlier pick. False (nothing
	// stored) when either is empty or longer than kTextCapacity.
	bool Set(std::string_view item, std::string_view cmd) {
		if (item.empty() || cmd.empty() || item.size() > kTextCapacity ||
			cmd.size() > kTextCapacity)
			return false;
		for (size_t i = 0; i < m_count; ++i)
			if (m_slots[i].Item() == item) {
				m_slots[i].SetCmd(cmd);
				return true;
			}
		// A new type. Copy first: `item` or `cmd` may view a slot the shift
		// below is about to move.
		Slot incoming;
		incoming.SetItem(item);
		incoming.SetCmd(cmd);
		if (m_count == kSlots) {
			std::move(m_slots.begin() + 1, m_slots.end(), m_slots.begin());
			--m_count;
		}
		m_slots[m_count++] = incoming;
		return true;
	}

	// Forgets the pick for `item`, so that hand holding it is UNSET again (the
	// hand menu's Clear). The survivors keep their order, which is the save's.
	// False when there was nothing to forget.
	bool Remove(std::string_view item) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_slots[i].Item() == item) {
				std::move(m_slots.begin() + i + 1, m_slots.begin() + m_count,
						  m_slots.begin() + i);
				--m_count;
				return true;
			}
		return false;
	}

	void Clear() { m_count = 0; }
	bool Empty() const { return m_count == 0; }
	size_t Size() const { return m_count; }

	// f(item, cmd) for every remembered pick, oldest first (the save's order).
	template <class F> void ForEach(F&& f) const {
		for (size_t i = 0; i < m_count; ++i) f(m_slots[i].Item(), m_slots[i].Cmd());
	}

private:
	struct Slot {
		char item[kTextCapacity + 1] = {};
		char cmd[kTextCapacity + 1] = {};
		unsigned char itemLen = 0;
		unsigned char cmdLen = 0;
		std::string_view Item() const { return {item, itemLen}; }
		std::string_view Cmd() const { return {cmd, cmdLen}; }
		void SetItem(std::string_view s) {
			std::memcpy(item, s.data(), s.size());
			itemLen = static_cast<unsigned char>(s.size());
		}
		void SetCmd(std::string_view s) {
			std::memmove(cmd, s.data(), s.size()); // may alias its own text
			cmdLen = static_cast<unsigned char>(s.size());
		}
	};
	std::array<Slot, kSlots> m_slots{};
	size_t m_count = 0;
};

} // namespace dungeon::game
