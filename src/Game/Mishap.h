// ============================================================================
// Game/Mishap.h — what a FUMBLE costs the person who threw it.
//
// A fumble (a first d100 face at or below fumble_threshold) has always decided
// the exchange — the swing cannot land, whatever the bonus behind it — and then
// done nothing else. This is the vocabulary that gives it consequences.
//
// TWO IDEAS, and the second is the one worth stating:
//
//   THE CONSEQUENCE IS AUTHORED, LIKE A PROC. `fumble = recover 2.2` on a
//   weapon reads beside `on_hit = bleed 2 8`, and the shape is deliberately the
//   same — comma-separated entries of a token and its numbers. But a proc names
//   an EFFECT and lands it on the defender, while these are one-shot events
//   against the ATTACKER and the exchange, which no status effect can express
//   ("the weapon leaves your hand" is not a condition anyone is in). So the two
//   coexist rather than one subsuming the other: `on_fumble` for anything that
//   really is an effect, `fumble` for everything else.
//
//   HOW BADLY YOU FUMBLED IS HOW BADLY YOU ROLLED. Severity takes no second
//   random draw — the first face is already 1..fumble_threshold, and a 01 is a
//   worse slip than a 05. `Severe` reads that face against a balance knob, so
//   the mild table fires on every fumble and the severe one only at the bottom
//   of the band. A consequence of getting this free: widening
//   fumble_threshold produces proportionally MORE mild fumbles, which is the
//   right direction — a clumsier fighter flails more often without flailing
//   more catastrophically.
//
// PURE, like Roll.h and Defense.h, and for the same reason: tools/RollTest
// links the shipping parser and selection rule rather than a copy. Keep it that
// way — the EXECUTION of a consequence (finding the floor cell a dropped
// weapon lands in, choosing which ally a wild swing catches) needs the world
// and lives in DungeonWorld, exactly as the effects adapters do.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game::mishap {

// The authored vocabulary. Each is a thing that happens TO THE ATTACKER or to
// the exchange they just lost — never to the defender, who by definition was
// not hit.
enum class Kind : u8 {
	Recover, // the swinging hand's cooldown x value — you are off balance
	Stumble, // spend `value` stamina, billed as exertion (so it can reach HP)
	Drop,    // the held weapon falls to the floor of your own cell
	Fling,   // ...or into a random adjacent walkable cell
	SelfHit, // the assembled blow lands on YOU at `value` of its damage
	Wild,    // ...or on an ally beside you, at full force
};

// One authored entry: a token and the number it takes. Drop/Fling/Wild take
// none, and their `value` is ignored rather than an error — an author writing
// `drop 1` has said something harmless.
struct Entry {
	Kind kind = Kind::Recover;
	float value = 0.0f;
};

// Resolve a token to its Kind. False for anything unrecognised, which the
// parser reports rather than guessing at — a typo'd consequence that silently
// became `recover` would be a fumble table that looks authored and is not.
bool KindFromToken(std::string_view token, Kind& out);

// The token a Kind was authored as (round-trips KindFromToken); for warnings
// and for the editor.
std::string_view TokenFor(Kind kind);

// Parse an authored list: entries separated by commas or semicolons, each
// "<token> [value]". `where` names the catalog entry in any warning. An empty
// spec adds nothing and is not an error — most weapons want the default table.
void Parse(std::string_view spec, std::vector<Entry>& out,
		   std::string_view where);

// Was this fumble a BAD one? `face` is the attack roll's first die face and
// `severeFace` the balance knob (the face at or below which the severe table
// also fires).
//
// A face of 0 means "no fumble was recorded" — the field's default — and must
// never read as severe, which it otherwise would for any knob >= 0. That is
// the whole reason this is a function and not an inline comparison at each
// call site.
bool Severe(int face, int severeFace);

// THE DEFAULT TABLES, in C++ rather than in balance.cat, because a table is a
// vocabulary and not a number — the spells.cat rule ("the class recipe is
// identity, the catalog holds overrides"). Their NUMBERS are knobs and arrive
// as arguments, so the feel is still tuned in the Balance dialog.
//
// A weapon or monster authoring its own list REPLACES the default outright
// rather than adding to it: a table you cannot turn off is not a table.
std::vector<Entry> DefaultFumble(float recoverMul);
std::vector<Entry> DefaultSevere();

} // namespace dungeon::game::mishap
