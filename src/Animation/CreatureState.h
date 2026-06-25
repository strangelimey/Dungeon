// ============================================================================
// Animation/CreatureState.h — the canonical creature animation states.
//
// These are the ROWS of the per-monster state→clip table (the data-driven
// animation system). The host maps live simulation (aware / moving / swinging /
// hp / spawned) onto a CreatureState each frame; the table then resolves that
// state to an actual clip NAME (with variations) for the chosen monster type.
//
// Not every state is driven yet — Defend, Run, and Taunt have table rows but no
// behaviour wired up (they stay unused until the AI/combat supports them). The
// enum is the shared vocabulary so the simulation, the catalog, and the editor
// all agree on the same set.
// ============================================================================
#pragma once

#include <optional>
#include <string_view>

namespace dungeon::anim {

// Ordered roughly by lifecycle. Keep `Count` last.
enum class CreatureState {
	Spawn,   // appearing / rising into the world      (one-shot, on creation)
	Idle,    // alive, unaware, at rest                 (loop)
	Alert,   // alive, aware of the party, holding      (loop)
	Walk,    // moving at normal pace                   (loop)
	Run,     // moving fast / charging                  (loop)
	Attack,  // melee swing                             (one-shot)
	Defend,  // blocking / bracing                      (loop)
	Hit,     // stagger / flinch from damage            (one-shot)
	Taunt,   // provoke / battle-cry                    (one-shot, flavour)
	Die,     // dying                                   (one-shot)
	Dead,    // settled corpse                          (loop / held)
	Count,
};

inline constexpr int kCreatureStateCount = static_cast<int>(CreatureState::Count);

// Lowercase token for a state: the catalog key for its table row AND the default
// clip name a monster falls back to when the table doesn't override it (so a clip
// simply named "attack"/"idle"/... is picked up with zero config).
constexpr std::string_view StateName(CreatureState s) {
	switch (s) {
	case CreatureState::Spawn:  return "spawn";
	case CreatureState::Idle:   return "idle";
	case CreatureState::Alert:  return "alert";
	case CreatureState::Walk:   return "walk";
	case CreatureState::Run:    return "run";
	case CreatureState::Attack: return "attack";
	case CreatureState::Defend: return "defend";
	case CreatureState::Hit:    return "hit";
	case CreatureState::Taunt:  return "taunt";
	case CreatureState::Die:    return "die";
	case CreatureState::Dead:   return "dead";
	default:                    return "idle";
	}
}

// Whether the state holds/loops vs plays once and resolves to a follow-up state.
constexpr bool IsLooping(CreatureState s) {
	switch (s) {
	case CreatureState::Spawn:
	case CreatureState::Attack:
	case CreatureState::Hit:
	case CreatureState::Taunt:
	case CreatureState::Die:
		return false; // one-shots
	default:
		return true;  // Idle / Alert / Walk / Run / Defend / Dead
	}
}

// Parse a catalog/editor token back to a state (case-sensitive, lowercase).
constexpr std::optional<CreatureState> ParseState(std::string_view token) {
	for (int i = 0; i < kCreatureStateCount; ++i) {
		const auto s = static_cast<CreatureState>(i);
		if (StateName(s) == token) return s;
	}
	return std::nullopt;
}

} // namespace dungeon::anim
