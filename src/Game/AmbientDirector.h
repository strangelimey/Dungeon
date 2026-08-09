// ============================================================================
// Game/AmbientDirector.h — sparse, placed, intermittent ambient sound.
//
// THE UNIT OF AMBIENCE IS AN EVENT WITH SILENCE AROUND IT. The first version of
// this was a continuous bed plus a continuous loop on every fire, and it was
// rejected on hearing as "all encompassing" — correctly, because nothing in it
// ever stopped. An ambience that is always sounding has already spent the
// silence that makes a drip land, and turning it down only makes a quieter
// wash. Myst is mostly quiet; that is *why* its drips work.
//
// So a spot here is a place that makes a sound EVERY SO OFTEN: fixed position,
// stochastic timing, long gaps. `loop = 1` is the exception for the few things
// that genuinely never stop.
//
// The director knows nothing about the world beyond the positions it is handed
// — the walled-off pattern MonsterAI and MagicSystem use. It owns its sample
// memory, which matters because playback is ZERO COPY: the SoundData it loads
// must outlive every voice referencing it, so a reload stops its own voices
// first.
// ============================================================================
#pragma once

#include "Assets/Wav.h"
#include "Audio/AudioEngine.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <random>
#include <string>
#include <vector>

namespace dungeon::game {

class Project;

class AmbientDirector {
public:
	// Reads ambience.cat and loads every sound it names. Safe to call again —
	// it stops its own voices first, since they point into the old buffers.
	void Load(const Project& project, audio::AudioEngine& audio);

	// Drop every placed spot and silence anything playing. A level swap.
	void Clear(audio::AudioEngine& audio);

	// Place one. Unknown ids are reported by the caller, not guessed at.
	bool AddSpot(const std::string& kindId, const Vec3& position);
	bool HasKind(const std::string& kindId) const { return FindKind(kindId) != nullptr; }

	// Fire whatever is due. Allocation-free in the steady state: the spot list
	// is fixed between placements and nothing here builds a container.
	void Update(float dt, audio::AudioEngine& audio);

	size_t SpotCount() const { return m_spots.size(); }
	// Live scale over every spot's authored gain, for tuning by ear.
	void SetGainScale(float scale) { m_gainScale = scale; }
	float GainScale() const { return m_gainScale; }
	// Live scale over every interval: 0.1 = ten times as busy, for auditioning
	// a palette without waiting out its real pacing.
	void SetIntervalScale(float scale) { m_intervalScale = scale; }
	float IntervalScale() const { return m_intervalScale; }

private:
	struct Kind {
		std::string id;
		std::vector<assets::SoundData> takes; // >1 = variants, picked at random
		float intervalMin = 15.0f, intervalMax = 40.0f;
		float gainMin = 0.4f, gainMax = 0.6f;
		float pitchMin = 1.0f, pitchMax = 1.0f;
		float reach = 14.0f;
		bool loop = false;
	};
	struct Spot {
		const Kind* kind = nullptr;
		Vec3 position;
		float untilNext = 0.0f;    // seconds; intermittent spots only
		audio::VoiceHandle voice;  // looping spots hold theirs
	};

	const Kind* FindKind(const std::string& id) const;
	float Roll(float lo, float hi);
	void Fire(Spot& spot, audio::AudioEngine& audio);

	std::vector<Kind> m_kinds;
	std::vector<Spot> m_spots;
	// Seeded fixed: an ambience that differs between two runs of the same level
	// cannot be judged, and "was that the same gap as last time?" is exactly the
	// question a tuning session needs to be able to answer.
	std::mt19937 m_rng{20260808};
	float m_gainScale = 1.0f;
	float m_intervalScale = 1.0f;
};

} // namespace dungeon::game
