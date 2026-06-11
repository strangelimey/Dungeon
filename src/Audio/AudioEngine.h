#pragma once

#include "Assets/Wav.h"
#include "Core/Types.h"

#include <memory>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace dungeon::audio {

class PooledVoice;

// XAudio2-backed sound-effect playback. Source voices are pooled and reused
// by format, and playback references the caller's sample memory directly
// (zero copy) — so a SoundData passed to Play must stay alive until the
// sound finishes. Game-owned sounds live for the app's lifetime, which
// satisfies this trivially.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool IsAvailable() const { return m_xaudio != nullptr; }

    // volume: 0..1, pan: -1 (left) .. +1 (right), pitch: playback speed ratio.
    void Play(const assets::SoundData& sound, float volume = 1.0f, float pan = 0.0f,
              float pitch = 1.0f);

    void SetMasterVolume(float volume);
    float MasterVolume() const { return m_masterVolume; }

private:
    IXAudio2* m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    float m_masterVolume = 1.0f;
    std::vector<std::unique_ptr<PooledVoice>> m_voices; // grows up to kMaxVoices
};

} // namespace dungeon::audio
