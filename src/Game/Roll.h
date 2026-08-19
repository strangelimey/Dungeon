// ============================================================================
// Game/Roll.h — the dice: open-ended d100 rolls and the opposed comparison.
//
// The resolution model (docs/damage-system.md) is Rolemaster-shaped: attacker
// and defender each assemble a bonus, each ADDS a d100, and the higher total
// wins. A roll of >= critThreshold is OPEN-ENDED — roll again and add, and a
// further high roll re-triggers, so there is no ceiling on a lucky swing. A
// roll of <= fumbleThreshold is a FUMBLE.
//
// This module is deliberately PURE: no game state, no character, no world, no
// balance struct. It takes numbers and an rng and returns numbers, which is
// what lets tools/RollTest exercise the SHIPPING code rather than a copy of it
// (the Bc7Test pattern), and what lets the distribution be measured before a
// single balance value is chosen. Everything above it — how a bonus is
// assembled, what a margin does to damage — lives in Combat/the effects
// pipeline, not here.
//
// TWO RULES THAT ARE DECISIONS, not arithmetic, and are therefore stated
// rather than left to be inferred from the code:
//
//   ONLY THE FIRST ROLL CAN FUMBLE. An escalation that comes up 03 is just a
//   small addition to an already-great roll — it does not turn a critical into
//   a disaster. Checking every roll would make a fumble MORE likely the better
//   you rolled, which is precisely backwards.
//
//   A TIE GOES TO THE DEFENDER. Michael's two statements of the rule differ on
//   this ("if the defense number is higher" vs "if the attacker's total is
//   higher"), so the strict reading is taken: an attack lands only when it
//   BEATS the defense. It matters about 1% of the time.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <random>
#include <vector>

namespace dungeon::game {

// The knobs. Every one is authored (balance.cat) rather than compiled in —
// dump 2's "I want knobs to tweak the formula" applies to the dice first,
// because these numbers decide the shape of every fight in the game.
struct RollRules {
	int sides = 100;          // the die
	int critThreshold = 95;   // >= this re-rolls and ADDS (open-ended)
	int fumbleThreshold = 5;  // <= this on the FIRST roll is a fumble
	bool openEndedDefense = true; // defenders crit and fumble too

	// TERMINATION GUARD, NOT A BALANCE KNOB. The escalation is meant to be
	// unbounded and at the default threshold it effectively is (each further
	// trigger is 6% likely, so 20 deep is a 1-in-10^24 event). But the
	// threshold is DATA: author critThreshold = 1 and every roll re-triggers
	// forever. The cap is what makes a bad number a bad game instead of a
	// hung process. If a run ever reaches it, that is a content bug and the
	// harness says so.
	int maxEscalations = 20;
};

// One assembled d100, escalations included.
struct Roll {
	int total = 0;       // the sum of every face rolled
	int first = 0;       // the initial face (what fumbles are judged on)
	int escalations = 0; // how many times it re-triggered
	bool crit = false;   // it went open-ended at least once
	bool fumble = false; // the first face was <= fumbleThreshold
	bool capped = false; // hit maxEscalations — a content bug, see above
};

// One opposed resolution: both sides' rolls, their totals with bonuses, and
// the margin that the damage side multiplies by.
struct Opposed {
	Roll attack;
	Roll defense;
	int attackTotal = 0;  // attack bonus + attack roll
	int defenseTotal = 0; // defense bonus + defense roll
	int margin = 0;       // attackTotal - defenseTotal (negative on a miss)
	bool hit = false;     // margin > 0 — a tie goes to the defender
};

// Roll one open-ended die under `rules`.
Roll RollOpenEnded(const RollRules& rules, std::mt19937& rng);

// The full opposed roll. The bonuses are whatever the callers assembled —
// weapon x skill x stat on one side, AG + dodge/armor skill on the other —
// and this module neither knows nor cares how.
Opposed Resolve(int attackBonus, int defenseBonus, const RollRules& rules,
				std::mt19937& rng);

} // namespace dungeon::game
