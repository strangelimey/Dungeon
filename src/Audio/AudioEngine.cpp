#include "Audio/AudioEngine.h"

#include "Core/Log.h"

#include <Windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dungeon::audio {

// One playing sound: owns a copy of the samples (so the caller's SoundData
// may be transient) and the XAudio2 source voice.
class PlayingVoice : public IXAudio2VoiceCallback {
public:
    PlayingVoice(IXAudio2* xaudio, const assets::SoundData& sound, float volume,
                 float pan, float pitch) {
        m_samples = sound.samples;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(sound.channels);
        fmt.nSamplesPerSec = sound.sampleRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        if (FAILED(xaudio->CreateSourceVoice(&m_voice, &fmt, 0, XAUDIO2_MAX_FREQ_RATIO,
                                             this))) {
            m_done = true;
            return;
        }

        m_voice->SetVolume(volume);
        m_voice->SetFrequencyRatio(pitch);
        if (sound.channels == 1 && pan != 0.0f) {
            // Constant-power pan for mono sources into a stereo master.
            const float angle = (pan * 0.5f + 0.5f) * 1.5707963f;
            float matrix[2] = {std::cos(angle), std::sin(angle)};
            m_voice->SetOutputMatrix(nullptr, 1, 2, matrix);
        }

        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(m_samples.size() * sizeof(i16));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(m_samples.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        if (FAILED(m_voice->SubmitSourceBuffer(&buffer)) || FAILED(m_voice->Start())) {
            m_done = true;
        }
    }

    ~PlayingVoice() {
        if (m_voice) m_voice->DestroyVoice();
    }

    bool IsDone() const { return m_done.load(); }

    // IXAudio2VoiceCallback
    void __stdcall OnStreamEnd() override { m_done = true; }
    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnBufferStart(void*) override {}
    void __stdcall OnBufferEnd(void*) override {}
    void __stdcall OnLoopEnd(void*) override {}
    void __stdcall OnVoiceError(void*, HRESULT) override { m_done = true; }

private:
    IXAudio2SourceVoice* m_voice = nullptr;
    std::vector<i16> m_samples;
    std::atomic<bool> m_done{false};
};

AudioEngine::AudioEngine() {
    // COM may already be initialized by the app; both outcomes are fine.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        log::Warn("XAudio2 unavailable — running silent");
        m_xaudio = nullptr;
        return;
    }
    if (FAILED(m_xaudio->CreateMasteringVoice(&m_master))) {
        log::Warn("No audio output device — running silent");
        m_xaudio->Release();
        m_xaudio = nullptr;
        return;
    }
    log::Info("Audio engine initialized");
}

AudioEngine::~AudioEngine() {
    m_voices.clear(); // destroy source voices before the engine
    if (m_master) m_master->DestroyVoice();
    if (m_xaudio) m_xaudio->Release();
}

void AudioEngine::Play(const assets::SoundData& sound, float volume, float pan,
                       float pitch) {
    if (!m_xaudio || sound.samples.empty()) return;
    m_voices.push_back(std::make_unique<PlayingVoice>(
        m_xaudio, sound, std::clamp(volume, 0.0f, 1.0f), std::clamp(pan, -1.0f, 1.0f),
        std::clamp(pitch, 0.05f, 4.0f)));
}

void AudioEngine::SetMasterVolume(float volume) {
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (m_master) m_master->SetVolume(m_masterVolume);
}

void AudioEngine::Update() {
    std::erase_if(m_voices, [](const auto& v) { return v->IsDone(); });
}

} // namespace dungeon::audio
