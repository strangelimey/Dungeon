// ============================================================================
// Game/AmbientDirector.cpp — see AmbientDirector.h.
// ============================================================================
#include "Game/AmbientDirector.h"

#include "Core/Log.h"
#include "Game/AssetUtil.h"
#include "Game/Project.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {

// "min max" out of one field, tolerating a single value meaning both.
void ReadRange(const CatalogEntry& entry, std::string_view key, float& lo, float& hi) {
	const std::string raw = entry.Get(key);
	if (raw.empty()) return;
	float a = 0.0f, b = 0.0f;
	const int got = std::sscanf(raw.c_str(), "%f %f", &a, &b);
	if (got >= 1) lo = a;
	hi = got >= 2 ? b : a;
	if (hi < lo) std::swap(lo, hi);
}

} // namespace

void AmbientDirector::Load(const Project& project, audio::AudioEngine& audio) {
	// Voices reference sample memory in m_kinds, so they must stop BEFORE the
	// old buffers go — playback is zero copy (Audio/AudioEngine.h).
	Clear(audio);
	m_kinds.clear();

	for (const CatalogEntry& entry : project.ambience.Entries()) {
		Kind kind;
		kind.id = entry.id;
		kind.loop = entry.GetBool("loop", false);
		kind.reach = entry.GetFloat("reach", 14.0f);
		ReadRange(entry, "interval", kind.intervalMin, kind.intervalMax);
		ReadRange(entry, "gain", kind.gainMin, kind.gainMax);
		ReadRange(entry, "pitch", kind.pitchMin, kind.pitchMax);

		const std::string stem = entry.Get("sound", entry.id);
		const int variants = static_cast<int>(entry.GetFloat("variants", 0.0f));
		if (variants > 1) {
			for (int i = 1; i <= variants; ++i) {
				auto take = LoadSound(std::format("{}_{}.wav", stem, i));
				if (!take.samples.empty()) kind.takes.push_back(std::move(take));
			}
		} else {
			auto take = LoadSound(stem + ".wav");
			if (!take.samples.empty()) kind.takes.push_back(std::move(take));
		}

		// A kind whose audio is missing is kept but inert. The wavs are
		// gitignored imports, so an unprovisioned worktree hits this for every
		// entry — and it must stay a warning, or the game would refuse to start
		// over an asset the player can simply not hear.
		if (kind.takes.empty())
			log::Warn("ambience '{}': no audio ({}.wav) — silent", kind.id, stem);
		m_kinds.push_back(std::move(kind));
	}
	log::Info("Ambience: {} kinds", m_kinds.size());
}

void AmbientDirector::Clear(audio::AudioEngine& audio) {
	for (const Spot& spot : m_spots) audio.Stop(spot.voice);
	m_spots.clear();
}

const AmbientDirector::Kind* AmbientDirector::FindKind(const std::string& id) const {
	const auto it = std::ranges::find(m_kinds, id, &Kind::id);
	return it == m_kinds.end() ? nullptr : &*it;
}

float AmbientDirector::Roll(float lo, float hi) {
	if (hi <= lo) return lo;
	return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

bool AmbientDirector::AddSpot(const std::string& kindId, const Vec3& position) {
	const Kind* kind = FindKind(kindId);
	if (!kind) return false;

	Spot spot;
	spot.kind = kind;
	spot.position = position;
	// STAGGER the first firing across the full interval instead of starting the
	// clock at zero. Otherwise every spot in the level fires on load and then
	// again together for as long as their intervals stay in step — the one
	// moment guaranteed to sound like a machine rather than a place.
	spot.untilNext = Roll(0.0f, kind->intervalMax);
	m_spots.push_back(spot);
	return true;
}

void AmbientDirector::Fire(Spot& spot, audio::AudioEngine& audio) {
	const Kind& kind = *spot.kind;
	if (kind.takes.empty()) return;

	const size_t take =
		kind.takes.size() == 1
			? 0
			: std::uniform_int_distribution<size_t>(0, kind.takes.size() - 1)(m_rng);

	audio::Emitter emitter;
	emitter.position = spot.position;
	emitter.minDistance = 1.0f;
	emitter.maxDistance = kind.reach;

	audio::PlayParams params;
	params.bus = audio::Bus::Ambience;
	params.emitter = &emitter;
	params.loop = kind.loop;
	params.volume = std::clamp(Roll(kind.gainMin, kind.gainMax) * m_gainScale, 0.0f, 1.0f);
	params.pitch = Roll(kind.pitchMin, kind.pitchMax);

	const audio::VoiceHandle voice = audio.Play(kind.takes[take], params);
	if (kind.loop) spot.voice = voice; // only a loop needs holding onto
}

void AmbientDirector::Update(float dt, audio::AudioEngine& audio) {
	for (Spot& spot : m_spots) {
		if (!spot.kind) continue;

		if (spot.kind->loop) {
			// Start it once, and restart it if it ever stops (a voice can be
			// dropped when the pool is saturated, and a silent fire would then
			// stay silent for the rest of the level).
			if (!audio.IsPlaying(spot.voice)) Fire(spot, audio);
			continue;
		}

		spot.untilNext -= dt;
		if (spot.untilNext > 0.0f) continue;
		Fire(spot, audio);
		spot.untilNext =
			Roll(spot.kind->intervalMin, spot.kind->intervalMax) * m_intervalScale;
	}
}

} // namespace dungeon::game
