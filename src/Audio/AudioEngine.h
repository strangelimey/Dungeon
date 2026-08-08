// ============================================================================
// Audio/AudioEngine.h — positional sound over XAudio2 + X3DAudio.
//
// THREADING: XAudio2 runs its own mixing thread. Voice-state flags are
// written from its callbacks, hence the atomics in PooledVoice. Everything
// public here is called from the game thread only.
//
// Failure is graceful at every level: with no audio device the engine logs a
// warning and every Play becomes a no-op (the game runs silent rather than
// crashing); with an unsupported speaker layout the reverb bus is dropped and
// everything else still works.
//
// THE SIGNAL PATH:
//
//   source voice ──> category submix (Sfx/Ambience/Ui/Music) ──> master
//               └──> reverb submix (positional voices only) ───┘
//
// The category submixes are what the settings sliders address. The reverb send
// level is computed per voice by X3DAudio, so a distant sound is wetter than a
// near one for free.
//
// MONO IS REQUIRED FOR POSITIONAL SOUND. X3DAudio computes a per-channel
// output matrix from the geometry between emitter and listener; a stereo
// source has already committed its channels and there is nothing left to
// place. The import path enforces this (AssetBaker import-sound downmixes by
// default) — here a stereo sound handed an emitter is logged and played as 2D,
// so the mistake is audible as "it doesn't move" rather than silent.
// ============================================================================
#pragma once

#include "Assets/Wav.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <memory>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SubmixVoice;

namespace dungeon::audio {

class PooledVoice;

// The mixer categories. Each is a submix with its own volume — the thing a
// settings slider actually moves, so "quieter footsteps but keep the ambience"
// is expressible.
enum class Bus : u32 { Sfx, Ambience, Ui, Music, Count };
inline constexpr u32 kBusCount = static_cast<u32>(Bus::Count);

// Where the party's ears are. Fed from the camera every frame — and it must be
// the EYE yaw (Party::EyeYaw), not the grid facing, or the world will refuse to
// turn under free-look while the view does.
struct Listener {
	Vec3 position{0.0f, 0.0f, 0.0f};
	Vec3 forward{0.0f, 0.0f, 1.0f};
	Vec3 up{0.0f, 1.0f, 0.0f};
};

// A sound's place in the world. Distances are WORLD units (metres) — a cell is
// kUnit across, so a maxDistance of 15 is roughly six squares.
struct Emitter {
	Vec3 position{0.0f, 0.0f, 0.0f};
	float minDistance = 1.0f;  // full volume within this radius
	float maxDistance = 15.0f; // silent beyond it
};

// A reference to a playing sound, valid until that sound ends or is replaced.
//
// Deliberately a POD rather than an RAII owner: most plays are fire-and-forget
// and must stay free, while the few that matter (an ambient loop, a projectile
// in flight) can hold one cheaply. The generation counter is what makes it safe
// — a pooled voice bumps it on reuse, so a handle to a sound that finished
// three plays ago addresses nothing rather than somebody else's audio.
struct VoiceHandle {
	static constexpr u32 kNone = 0xFFFFFFFFu;
	u32 slot = kNone;
	u32 generation = 0;

	bool Valid() const { return slot != kNone; }
};

struct PlayParams {
	Bus bus = Bus::Sfx;
	float volume = 1.0f;
	float pitch = 1.0f;
	bool loop = false;
	// Null = a 2D sound (centred, no reverb send, no distance). Non-null = the
	// sound belongs to a place, and is spatialized against the listener.
	const Emitter* emitter = nullptr;
};

// How the space around the listener sounds. Phase 8 derives these from the map
// (an open-cell flood gives volume and corridor-vs-hall aspect); until then the
// default is a modest stone room.
struct ReverbSpace {
	float decaySeconds = 1.0f;       // how long the tail runs
	float roomDb = -10.0f;           // wet level
	float roomHfDb = -6.0f;          // HF damping: stone is bright, earth dull
	float reflectionsDelayMs = 10.0f;// early reflections — reads as size
	float wetDryMix = 20.0f;         // percent wet at the bus output
};

// XAudio2-backed playback. Source voices are pooled and reused by format, and
// playback references the caller's sample memory directly (zero copy) — so a
// SoundData passed to Play must stay alive until the sound finishes. Game-owned
// sounds live for the app's lifetime, which satisfies this trivially; a LOOPING
// sound never finishes on its own, so its owner must Stop it before freeing.
class AudioEngine {
public:
	AudioEngine();
	~AudioEngine();

	AudioEngine(const AudioEngine&) = delete;
	AudioEngine& operator=(const AudioEngine&) = delete;

	bool IsAvailable() const { return m_xaudio != nullptr; }

	// --- playback -----------------------------------------------------------

	VoiceHandle Play(const assets::SoundData& sound, const PlayParams& params);

	// Fire-and-forget 2D, the shape most call sites want: volume 0..1,
	// pan -1 (left) .. +1 (right), pitch as a playback speed ratio.
	void Play(const assets::SoundData& sound, float volume = 1.0f, float pan = 0.0f,
			  float pitch = 1.0f);

	void Stop(VoiceHandle handle);
	bool IsPlaying(VoiceHandle handle) const;

	// Move a playing positional sound — a projectile in flight, a monster.
	// Ignored (harmlessly) for a 2D voice or a stale handle.
	void SetPosition(VoiceHandle handle, const Vec3& position);
	void SetVolume(VoiceHandle handle, float volume);

	// Destroys every source voice, silencing playback and dropping all
	// references into caller-owned sample memory. Call before that memory is
	// freed when the owner dies first (the engine outlives Game at shutdown —
	// see ~Game). The engine keeps working; Play regrows the pool.
	void StopAll();

	// --- per-frame ----------------------------------------------------------

	void SetListener(const Listener& listener) { m_listener = listener; }

	// Re-spatializes every live positional voice against the listener and
	// retires finished ones. Allocation-free by construction (fixed pool, fixed
	// matrix scratch) — steady-state frames are audited, see AllocTrack.
	void Update();

	// --- mix ----------------------------------------------------------------

	void SetMasterVolume(float volume);
	float MasterVolume() const { return m_masterVolume; }

	void SetBusVolume(Bus bus, float volume);
	float BusVolume(Bus bus) const;

	void SetReverb(const ReverbSpace& space);
	bool ReverbAvailable() const { return m_reverb != nullptr; }

private:
	PooledVoice* Resolve(VoiceHandle handle) const;
	void Spatialize(PooledVoice& voice);

	IXAudio2* m_xaudio = nullptr;
	IXAudio2MasteringVoice* m_master = nullptr;
	IXAudio2SubmixVoice* m_buses[kBusCount]{};
	IXAudio2SubmixVoice* m_reverb = nullptr;

	float m_masterVolume = 1.0f;
	float m_busVolumes[kBusCount]{1.0f, 1.0f, 1.0f, 1.0f};

	u32 m_destChannels = 2; // channels of the category submixes (= master's)
	Listener m_listener;

	// X3DAUDIO_HANDLE is a byte array; kept opaque so <x3daudio.h> stays out of
	// this header (it drags in Windows.h).
	u8 m_x3d[20]{};

	std::vector<std::unique_ptr<PooledVoice>> m_voices; // grows up to kMaxVoices
};

} // namespace dungeon::audio
