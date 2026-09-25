// ============================================================================
// Game/GenerateKnobs.h — the generator's knobs as ONE table.
//
// Every knob the level generator takes is a row here: its stable key, its label,
// the dialog tab it sits on, its range and granularity, and how to read and
// write it on a generate::Params. Everything that has to enumerate the knobs
// walks this table rather than naming them:
//
//   * GenerateDialog builds its tabs and widgets from it;
//   * the last-used knobs round-trip through settings.ini as one encoded line
//     (Encode/Decode), so the tweak-and-generate loop keeps its state across
//     runs;
//   * saved presets (docs/level-building.md, P4b) store the same encoding.
//
// So a new knob is a Params field plus ONE row — the kBalanceFields idiom —
// and cannot be exposed in the dialog yet forgotten by the round-trip.
//
// The accessors go through DOUBLE, not float: the seed is a u32, and a float
// holds only 24 bits of it exactly.
// ============================================================================
#pragma once

#include "Game/Generate.h"

#include <span>
#include <string>
#include <string_view>

namespace dungeon::game::generate {

enum class KnobKind {
	Int,   // a whole number on a slider
	Float, // a 0..1-ish fraction on a slider
	Seed,  // a u32 typed into a field (and rerolled by the dialog's Roll)
};

struct Knob {
	const char* key;   // stable: the encoded name, never localized or renamed
	const char* label; // loc key
	const char* tab;   // loc key of the dialog tab it lives on
	KnobKind kind;
	double lo, hi; // the clamp Decode and the dialog both apply
	double (*get)(const Params&);
	void (*set)(Params&, double);
};

std::span<const Knob> Knobs();

// The tabs, in display order (loc keys). Every Knob::tab is one of these.
std::span<const char* const> KnobTabs();

// Clamp and round `v` to what knob `k` accepts, then store it.
void SetKnob(const Knob& k, Params& p, double v);

// "rooms:8 branching:0.5 ..." — every knob, in table order. Space-separated
// key:value pairs so the line survives an ini file and a catalog field alike.
std::string Encode(const Params& p);

// The inverse. Unknown keys are ignored and absent ones keep `p`'s value, so a
// line written before a knob existed still loads — and a knob that is later
// retired simply stops being read.
void Decode(std::string_view text, Params& p);

} // namespace dungeon::game::generate
