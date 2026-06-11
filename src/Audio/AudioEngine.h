#pragma once

#include "Assets/Wav.h"
#include "Core/Types.h"

#include <memory>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace dungeon::audio {

class PlayingVoice;

// XAudio2-backed sound-effect playback. Sounds are fire-and-forget; call
// Update() once per frame to reclaim finished voices.
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

    void Update();

private:
    IXAudio2* m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    float m_masterVolume = 1.0f;
    std::vector<std::unique_ptr<PlayingVoice>> m_voices;
};

} // namespace dungeon::audio
