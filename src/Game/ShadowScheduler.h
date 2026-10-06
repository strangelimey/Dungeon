// ============================================================================
// Game/ShadowScheduler.h — point-light shadow-cube budgeting.
//
// Split out of DungeonWorld. Given the frame's point lights and the camera eye
// it does two jobs and owns no rendering or world state:
//   1. AssignSlots — hands the kShadowSlots cube-shadow slots to the lights
//      NEAREST the eye, slot 0 (highest resolution + PCF) to the closest and
//      coarser slots outward. Assignment is HYSTERETIC (a slot holder resists
//      being bumped by a marginally-closer rival, matched by its stable id
//      since the light list is rebuilt every frame) and each slotted light gets
//      a shadowStrength that fades in over distance, so a shadow dissolves in
//      instead of popping the instant the light wins a slot.
//   2. ShouldRender — decides, per slot, whether the slot's CACHED cube can be
//      reused this frame or must be re-rendered: a new light in the slot, the
//      map geometry changed, a CASTER changed within the light (a monster
//      animating or a thrown item flying - the world's verdict - or a change
//      noted since the last pass, below), the light moved, or a flicker tick is
//      due. The caller does the actual cube draws when it says so.
//
// SLOT 0 is whichever shadow-casting light is nearest the eye. With one carried
// light (the party's torch) that is it; a second carried light - a torch AND a
// Firelight - competes with it by distance like any other (code-review C187).
//
// FLICKER is paced on WALL time, not frames: a wandering fire's cube may
// re-render at most FlickerHz times a second (25 by default), and at most
// FlickerBudget such re-renders happen in one frame (2), which staggers them
// (dev console `shadowrate`). Everything else above is correctness and is
// never budgeted away.
//
// CHANGED CASTERS (code-review C178). The world's verdict covers what moves
// every frame; what changes ONCE - a door leaf travelling, a lever thrown, a
// floor item lifted or set down, a corpse gone, a prop smashed, a wall torch
// taken - is NOTED here (NoteCasterChanged), and a cube whose light reaches a
// note re-renders on the next pass. A global revision would do too, at the
// price of re-rendering all eight cubes for a door anywhere on the level.
//
// The world (a) builds the light list, (b) supplies the live map revision, a
// moving-caster verdict and the notes, and (c) renders the cubes ShouldRender
// selects.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/Lights.h"
#include "Graphics/Renderer.h" // gfx::kShadowSlots

#include <array>
#include <chrono>
#include <span>
#include <utility>
#include <vector>

namespace dungeon::game {

class ShadowScheduler {
public:
	ShadowScheduler();

	// Assigns shadow slots to the lights nearest `eye`, setting each light's
	// shadowSlot (-1 = none) and shadowStrength fade in place. shadowsEnabled
	// false clears every slot (dev-console `shadows off`: lights stay lit).
	void AssignSlots(std::span<gfx::PointLight> lights, const Vec3& eye,
					 bool shadowsEnabled);

	// Call once at the start of the shadow pass (advances the flicker clock and
	// refills the frame's flicker budget).
	void BeginPass();

	// How often a WANDERING fire cube may re-render, in hertz, and how many such
	// re-renders one frame may spend on them.
	//
	// This used to be a FRAME count — every other frame — which is not a rate at
	// all: at 240 fps it re-rendered each fire cube 120 times a second, and the
	// flicker itself ran four times faster than at 60 fps. The look changed with
	// the hardware, and the cost per second scaled with frame rate for no visual
	// gain. Measured: 21 shadow faces a frame, about 5,000 a second.
	//
	// Live so the rate can be judged by eye rather than argued about — dev console
	// `shadowrate <hz> [budget]`.
	void SetFlickerHz(float hz, int perFrameBudget = -1);
	float FlickerHz() const { return m_flickerHz; }
	int FlickerBudget() const { return m_flickerBudget; }

	// Something the shadow pass draws changed at `center` (metres; `radius` the
	// sphere it can have changed in): a cube whose light reaches it re-renders on
	// the next pass. FIXED SIZE and allocation-free - a note lands in a guarded
	// frame - and a repeat of a note already held is not stored twice. Past
	// kMaxNotes the list gives up and the next pass re-renders every cube, which
	// is always correct, only slower (a frame with no shadow pass at all - the
	// full-screen editor, a headless run - just keeps collecting until one runs).
	void NoteCasterChanged(const Vec3& center, float radius);

	// Whether slot `light.shadowSlot`'s cube must be re-rendered this frame for
	// the light at list index `lightIndex`. `mapRevision` is the live geometry
	// revision; `casterMoving` is the world's verdict that a caster moves within
	// the light this frame (a monster animating, a thrown item in flight).
	// Returns true AND records the render in the slot cache; false means the
	// cube (still bound as an SRV) is reused.
	bool ShouldRender(const gfx::PointLight& light, size_t lightIndex,
					  u32 mapRevision, bool casterMoving);

	// Call once at the end of the shadow pass. The notes are spent - except that
	// a slot no light visited this pass still holds the cube of the light it
	// last rendered, which may come back to it next frame unchanged: if a note
	// fell inside that light's reach while it was away (culled off screen while
	// the door beside it opened), the cube is forgotten.
	void EndPass();

	// Forces every cached cube to re-render next frame (quality swap, level load).
	void InvalidateCubes();

	// --- the readout (dev console `shadows status`) -------------------------------
	// Why a cube re-rendered, the FIRST of these that held (in this order).
	enum class Reason : u8 { New, Geometry, Caster, Moved, Flicker, Count };
	static const char* ReasonName(Reason reason);
	struct SlotStats {
		u64 renders = 0;
		std::array<u64, static_cast<size_t>(Reason::Count)> by{};
		u32 lightId = 0;   // the light it last rendered for
		u64 lastPass = 0;  // the pass (Stats::passes) of its last render
		// The farthest the light has stood from where its cube was rendered, at a
		// pass that REUSED the cube, since this light took the slot - in metres.
		// What a walking light's shadow lags it by, measured from positions, so
		// a slack computed wrong shows here whatever the frame rate.
		float lag = 0.0f;
	};
	struct Stats {
		u64 passes = 0;
		u64 notes = 0;     // caster changes noted (repeats within a pass not counted)
		u64 overflows = 0; // passes whose notes overflowed: every cube re-rendered
		u64 swept = 0;     // slots forgotten at a pass's end (see EndPass)
		std::array<SlotStats, gfx::kShadowSlots> slots{};
	};
	const Stats& GetStats() const { return m_stats; }

	// The harness's MUTATIONS (AllocTest -Lights -ShadowSelfTest): `notes` drops
	// every NoteCasterChanged, `moves` stops a wandering light's move counting
	// (the rule before code-review C187). Each must make its check fail, or the
	// check could pass on a cache that never re-renders. Dev `shadows ignore`.
	void Ignore(bool notes, bool moves) {
		m_ignoreNotes = notes;
		m_ignoreMoves = moves;
	}
	bool IgnoresNotes() const { return m_ignoreNotes; }
	bool IgnoresMoves() const { return m_ignoreMoves; }

private:
	// Whether any note's sphere meets the sphere (`center`, `radius`).
	bool NotedNear(const Vec3& center, float radius) const;

	// Scratch for AssignSlots: (distance, light index), sorted nearest-first into
	// retained capacity each frame.
	std::vector<std::pair<float, size_t>> m_candidates;
	// The lights that held slots last frame - the hysteresis anchor. Matched by
	// the light's stable id (gfx::PointLight::id); a light with none falls back
	// to its position (a fire only wanders a few cm).
	struct Incumbent {
		u32 id = 0;
		Vec3 pos{};
	};
	std::array<Incumbent, gfx::kShadowSlots> m_incumbents{};
	size_t m_incumbentCount = 0;

	// Per-slot cube cache: the slot's cube is reused unless a ShouldRender
	// condition trips. Keyed by the light's stable id (gfx::PointLight::id), or
	// its list index for a light with none - the budget ranking reorders the
	// list, so an index would invalidate cubes that never changed.
	static constexpr u32 kNoLight = 0xFFFFFFFFu;
	struct SlotCache {
		u32 lightId = kNoLight;     // identity that last rendered this slot
		Vec3 pos{};                 // light position at that render
		float radius = 0.0f;        // ...and its reach (EndPass's sweep)
		u32 revision = 0xFFFFFFFFu; // map geometry revision at that render
		// When this slot last re-rendered for FLICKER. Kept apart from the other
		// fields because they are reset together on any render and this one paces
		// only the aesthetic re-renders.
		f64 lastFlickerSec = -1.0e9;
		bool visited = false;       // a light asked about this slot this pass
	};
	SlotCache m_cache[gfx::kShadowSlots];

	// The changed casters noted since the last pass (NoteCasterChanged).
	struct Note {
		Vec3 center{};
		float radius = 0.0f;
	};
	static constexpr u32 kMaxNotes = 32;
	std::array<Note, kMaxNotes> m_notes{};
	u32 m_noteCount = 0;
	bool m_noteOverflow = false;
	bool m_ignoreNotes = false;
	bool m_ignoreMoves = false;
	Stats m_stats;

	// Wall seconds since the first pass. The scheduler keeps its own clock rather
	// than having dt threaded down the render path: a flicker cadence is about
	// wall time, not about simulation time or frames.
	f64 m_nowSec = 0.0;
	std::chrono::steady_clock::time_point m_epoch{};
	bool m_haveEpoch = false;
	// 25 Hz: CHOSEN BY EYE, not derived. Michael compared 8 / 14 / 25 / 40 in
	// the live `shadowrate` knob and this is where the fire stopped looking
	// slowed down without costing what the old frame-counted pacing did.
	f32 m_flickerHz = 25.0f;
	int m_flickerBudget = 2; // cubes per frame, which also staggers them
	int m_flickerLeft = 0;   // budget remaining this pass
};

} // namespace dungeon::game
