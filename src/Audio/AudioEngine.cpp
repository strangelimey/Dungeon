#include "Audio/AudioEngine.h"

#include "Core/Log.h"

#include <Windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dungeon::audio {

// A reusable XAudio2 source voice. The voice object (and its OS resources)
// lives for the engine's lifetime and is restarted for each playback —
// nothing is allocated per Play, and sample memory is referenced, not copied.
class PooledVoice : public IXAudio2VoiceCallback {
public:
	PooledVoice(IXAudio2* xaudio, u32 channels, u32 sampleRate)
		: m_channels(channels), m_sampleRate(sampleRate) {
		WAVEFORMATEX fmt{};
		fmt.wFormatTag = WAVE_FORMAT_PCM;
		fmt.nChannels = static_cast<WORD>(channels);
		fmt.nSamplesPerSec = sampleRate;
		fmt.wBitsPerSample = 16;
		fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
		fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
		if (FAILED(xaudio->CreateSourceVoice(&m_voice, &fmt, 0, XAUDIO2_MAX_FREQ_RATIO,
											 this)))
			m_voice = nullptr;
	}

	~PooledVoice() {
		if (m_voice) m_voice->DestroyVoice();
	}

	bool IsValid() const { return m_voice != nullptr; }
	bool IsIdle() const { return m_idle.load(std::memory_order_acquire); }
	bool MatchesFormat(u32 channels, u32 sampleRate) const {
		return channels == m_channels && sampleRate == m_sampleRate;
	}

	void Start(const assets::SoundData& sound, float volume, float pan, float pitch) {
		m_voice->Stop(0);
		m_voice->FlushSourceBuffers();
		m_voice->SetVolume(volume);
		m_voice->SetFrequencyRatio(pitch);
		if (m_channels == 1) {
			// Constant-power pan for mono sources into a stereo master.
			const float angle = (pan * 0.5f + 0.5f) * 1.5707963f;
			const float matrix[2] = {std::cos(angle), std::sin(angle)};
			m_voice->SetOutputMatrix(nullptr, 1, 2, matrix);
		}

		XAUDIO2_BUFFER buffer{};
		buffer.AudioBytes = static_cast<UINT32>(sound.samples.size() * sizeof(i16));
		buffer.pAudioData = reinterpret_cast<const BYTE*>(sound.samples.data());
		buffer.Flags = XAUDIO2_END_OF_STREAM;

		m_idle.store(false, std::memory_order_release);
		if (FAILED(m_voice->SubmitSourceBuffer(&buffer)) || FAILED(m_voice->Start()))
			m_idle.store(true, std::memory_order_release);
	}

	// IXAudio2VoiceCallback (audio thread)
	void __stdcall OnStreamEnd() override { m_idle.store(true, std::memory_order_release); }
	void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
	void __stdcall OnVoiceProcessingPassEnd() override {}
	void __stdcall OnBufferStart(void*) override {}
	void __stdcall OnBufferEnd(void*) override {}
	void __stdcall OnLoopEnd(void*) override {}
	void __stdcall OnVoiceError(void*, HRESULT) override {
		m_idle.store(true, std::memory_order_release);
	}

private:
	IXAudio2SourceVoice* m_voice = nullptr;
	u32 m_channels;
	u32 m_sampleRate;
	std::atomic<bool> m_idle{true};
};

AudioEngine::AudioEngine() {
	// NO CoInitializeEx here, deliberately. XAudio2 2.8+ (xaudio2.lib, in-box on
	// Windows 10/11) is a flat API that does not need COM — and this object is
	// constructed on the MAIN thread, so initializing COM here would join that
	// thread to an apartment for the life of the process. It used to ask for the
	// MTA, which silently broke every shell dialog: a thread's apartment is fixed
	// once joined, so Platform/FileDialog's CoInitializeEx(APARTMENTTHREADED) got
	// RPC_E_CHANGED_MODE and IFileOpenDialog::Show deadlocked from the MTA — the
	// editor's "Browse Folder..." wedged the process with no window ever shown.
	if (FAILED(XAudio2Create(m_xaudio.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR))) {
		log::Warn("XAudio2 unavailable - running silent");
		m_xaudio.Reset();
		return;
	}
	IXAudio2MasteringVoice* master = nullptr;
	if (FAILED(m_xaudio->CreateMasteringVoice(&master))) {
		log::Warn("No audio output device - running silent");
		m_xaudio.Reset();
		return;
	}
	m_master.reset(master);
	m_voices.reserve(kMaxVoices); // may throw: the members above still clean up
	log::Info("Audio engine initialized (voice pool, max {})", kMaxVoices);
}

void DestroyMasterVoice::operator()(IXAudio2MasteringVoice* voice) const {
	voice->DestroyVoice();
}

// Nothing to do by hand: the members tear down in reverse declaration order -
// source voices, mastering voice, engine (see the header).
AudioEngine::~AudioEngine() = default;

void AudioEngine::StopAll() {
	// DestroyVoice (via ~PooledVoice) blocks until the mixer thread has
	// released the voice, so no submitted sample buffer is read after this.
	m_voices.clear();
}

void AudioEngine::Play(const assets::SoundData& sound, float volume, float pan,
					   float pitch) {
	if (!m_xaudio || sound.samples.empty()) return;
	volume = std::clamp(volume, 0.0f, 1.0f);
	pan = std::clamp(pan, -1.0f, 1.0f);
	pitch = std::clamp(pitch, 0.05f, 4.0f);

	// Reuse an idle voice with a matching format.
	for (auto& voice : m_voices) {
		if (voice->IsIdle() && voice->MatchesFormat(sound.channels, sound.sampleRate)) {
			voice->Start(sound, volume, pan, pitch);
			return;
		}
	}

	// Grow the pool, or recycle an idle voice of a different format.
	std::unique_ptr<PooledVoice>* slot = nullptr;
	if (m_voices.size() < kMaxVoices) {
		slot = &m_voices.emplace_back();
	} else {
		const auto it = std::ranges::find_if(
			m_voices, [](const auto& v) { return v->IsIdle(); });
		if (it == m_voices.end()) return; // every voice busy — drop the sound
		slot = &*it;
	}
	*slot = std::make_unique<PooledVoice>(m_xaudio.Get(), sound.channels, sound.sampleRate);
	if (!(*slot)->IsValid()) {
		m_voices.erase(m_voices.begin() + (slot - m_voices.data()));
		return;
	}
	(*slot)->Start(sound, volume, pan, pitch);
}

void AudioEngine::Reserve(u32 channels, u32 sampleRate, size_t count) {
	if (!m_xaudio) return;
	size_t have = static_cast<size_t>(std::ranges::count_if(
		m_voices, [&](const auto& v) { return v->MatchesFormat(channels, sampleRate); }));
	while (have < count && m_voices.size() < kMaxVoices) {
		auto voice = std::make_unique<PooledVoice>(m_xaudio.Get(), channels, sampleRate);
		if (!voice->IsValid()) return; // Play still creates one on demand
		m_voices.push_back(std::move(voice));
		++have;
	}
}

void AudioEngine::SetMasterVolume(float volume) {
	m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
	if (m_master) m_master->SetVolume(m_masterVolume);
}

} // namespace dungeon::audio
