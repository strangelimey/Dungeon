#include "Audio/AudioEngine.h"

#include "Core/Log.h"

#include <Windows.h>
#include <x3daudio.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>

namespace dungeon::audio {

namespace {

// Ambient loops hold a voice for as long as they play, so the pool has to cover
// the beds and emitters standing in a room PLUS whatever combat is doing on top.
constexpr size_t kMaxVoices = 48;

// The distance curve, normalized: x is distance/maxDistance, y is gain. The
// default X3DAudio curve is an inverse-square that never actually reaches zero,
// so a drip six rooms away stays faintly audible forever and every emitter in
// the level competes for the mix. This one lands on silence at maxDistance,
// which is what makes "maxDistance" mean anything.
constexpr X3DAUDIO_DISTANCE_CURVE_POINT kFalloffPoints[]{
	{0.0f, 1.0f}, {0.2f, 0.78f}, {0.45f, 0.42f}, {0.7f, 0.17f}, {1.0f, 0.0f}};

static_assert(sizeof(X3DAUDIO_HANDLE) == 20, "m_x3d must match X3DAUDIO_HANDLE");

} // namespace

// ============================================================================
// PooledVoice
// ============================================================================

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
		// USEFILTER at creation is what later allows SetFilterParameters — it
		// cannot be turned on afterwards, and it is how a distant sound gets
		// dulled rather than merely quietened.
		if (FAILED(xaudio->CreateSourceVoice(&m_voice, &fmt, XAUDIO2_VOICE_USEFILTER,
											 XAUDIO2_MAX_FREQ_RATIO, this)))
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

	IXAudio2SourceVoice* Voice() const { return m_voice; }
	u32 Channels() const { return m_channels; }
	u32 Generation() const { return m_generation; }
	void BumpGeneration() { ++m_generation; }

	bool positional = false;
	Emitter emitter;
	float volume = 1.0f;
	u32 bus = 0; // which category submix the dry send goes to
	// Whether this voice's end has already been accounted for. A voice becomes
	// idle on the AUDIO thread; the game thread notices in Update and bumps the
	// generation exactly once, which is what makes a handle to a finished sound
	// go stale instead of quietly addressing whatever plays next in this slot.
	bool retired = true;

	void Start(const assets::SoundData& sound, float gain, float pitch, bool loop) {
		m_voice->Stop(0);
		m_voice->FlushSourceBuffers();
		m_voice->SetVolume(gain);
		m_voice->SetFrequencyRatio(pitch);
		volume = gain;
		retired = false;
		++m_generation;

		XAUDIO2_BUFFER buffer{};
		buffer.AudioBytes = static_cast<UINT32>(sound.samples.size() * sizeof(i16));
		buffer.pAudioData = reinterpret_cast<const BYTE*>(sound.samples.data());
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		// An infinite loop never raises OnStreamEnd, so such a voice stays busy
		// until somebody Stops it — which is exactly the contract a bed wants.
		buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

		m_idle.store(false, std::memory_order_release);
		if (FAILED(m_voice->SubmitSourceBuffer(&buffer)) || FAILED(m_voice->Start()))
			m_idle.store(true, std::memory_order_release);
	}

	void Halt() {
		if (!m_voice) return;
		m_voice->Stop(0);
		m_voice->FlushSourceBuffers();
		m_idle.store(true, std::memory_order_release);
		positional = false;
		retired = true;
		++m_generation;
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
	u32 m_generation = 1; // 0 is never handed out, so a default handle is stale
	std::atomic<bool> m_idle{true};
};

// ============================================================================
// Construction — the bus graph
// ============================================================================

AudioEngine::AudioEngine() {
	// NO CoInitializeEx here, deliberately. XAudio2 2.8+ (xaudio2.lib, in-box on
	// Windows 10/11) is a flat API that does not need COM — and this object is
	// constructed on the MAIN thread, so initializing COM here would join that
	// thread to an apartment for the life of the process. It used to ask for the
	// MTA, which silently broke every shell dialog: a thread's apartment is fixed
	// once joined, so Platform/FileDialog's CoInitializeEx(APARTMENTTHREADED) got
	// RPC_E_CHANGED_MODE and IFileOpenDialog::Show deadlocked from the MTA — the
	// editor's "Browse Folder..." wedged the process with no window ever shown.
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

	XAUDIO2_VOICE_DETAILS master{};
	m_master->GetVoiceDetails(&master);
	m_destChannels = master.InputChannels;

	DWORD channelMask = 0;
	m_master->GetChannelMask(&channelMask);
	if (FAILED(X3DAudioInitialize(channelMask, X3DAUDIO_SPEED_OF_SOUND,
								  reinterpret_cast<X3DAUDIO_HANDLE&>(m_x3d)))) {
		log::Warn("X3DAudio init failed — sounds will play without position");
	}

	// The category buses match the master's channel count, so surround placement
	// survives on a 5.1 setup instead of being folded to stereo on the way in.
	for (u32 i = 0; i < kBusCount; ++i) {
		if (FAILED(m_xaudio->CreateSubmixVoice(&m_buses[i], m_destChannels,
											   master.InputSampleRate, 0, 0, nullptr,
											   nullptr))) {
			log::Warn("Could not create the {} bus", i);
			m_buses[i] = nullptr;
		}
	}

	// The reverb bus is deliberately STEREO whatever the speakers are. The
	// XAudio2 reverb XAPO only accepts a fixed set of channel pairings (1->1,
	// 2->2, 4->4, 5.1->5.1 and some upmixes), and 2->2 is the one combination
	// available on every layout — a 7.1 master would otherwise fail to create
	// the effect at all. Submix-to-master upmixing then puts the tail where it
	// belongs, and the dry path still gets full surround placement.
	IUnknown* reverbApo = nullptr;
	if (SUCCEEDED(XAudio2CreateReverb(&reverbApo))) {
		XAUDIO2_EFFECT_DESCRIPTOR desc{};
		desc.pEffect = reverbApo;
		desc.InitialState = TRUE;
		desc.OutputChannels = 2;
		XAUDIO2_EFFECT_CHAIN chain{};
		chain.EffectCount = 1;
		chain.pEffectDescriptors = &desc;

		if (FAILED(m_xaudio->CreateSubmixVoice(&m_reverb, 2, master.InputSampleRate, 0,
											   0, nullptr, &chain))) {
			log::Warn("Could not create the reverb bus — running dry");
			m_reverb = nullptr;
		}
		reverbApo->Release(); // the submix holds its own reference now
	} else {
		log::Warn("Reverb effect unavailable — running dry");
	}

	if (m_reverb) SetReverb(ReverbSpace{});

	m_voices.reserve(kMaxVoices);
	log::Info("Audio engine initialized ({} ch, {} Hz, {} voices, reverb {})",
			  m_destChannels, master.InputSampleRate, kMaxVoices,
			  m_reverb ? "on" : "off");
}

AudioEngine::~AudioEngine() {
	m_voices.clear(); // destroy source voices before their destinations
	if (m_reverb) m_reverb->DestroyVoice();
	for (auto*& bus : m_buses)
		if (bus) bus->DestroyVoice();
	if (m_master) m_master->DestroyVoice();
	if (m_xaudio) m_xaudio->Release();
}

void AudioEngine::StopAll() {
	// DestroyVoice (via ~PooledVoice) blocks until the mixer thread has
	// released the voice, so no submitted sample buffer is read after this.
	m_voices.clear();
}

// ============================================================================
// Playback
// ============================================================================

VoiceHandle AudioEngine::Play(const assets::SoundData& sound, const PlayParams& params) {
	if (!m_xaudio || sound.samples.empty()) return {};

	const float volume = std::clamp(params.volume, 0.0f, 1.0f);
	const float pitch = std::clamp(params.pitch, 0.05f, 4.0f);

	bool positional = params.emitter != nullptr;
	if (positional && sound.channels != 1) {
		// See the header: a stereo source has no room left for placement. Say so
		// — the alternative is a sound that quietly refuses to move, which reads
		// as a broken listener rather than a mis-imported asset.
		log::Warn("Positional playback needs a mono sound (this one has {} channels) "
				  "— playing it flat. Re-import without --stereo.",
				  sound.channels);
		positional = false;
	}

	// Find a free slot: an idle voice of the same format first, then growth,
	// then any idle voice (recreated for this format).
	size_t index = m_voices.size();
	for (size_t i = 0; i < m_voices.size(); ++i) {
		if (m_voices[i] && m_voices[i]->IsIdle() &&
			m_voices[i]->MatchesFormat(sound.channels, sound.sampleRate)) {
			index = i;
			break;
		}
	}
	if (index == m_voices.size()) {
		if (m_voices.size() < kMaxVoices) {
			m_voices.emplace_back();
		} else {
			const auto it = std::ranges::find_if(
				m_voices, [](const auto& v) { return v && v->IsIdle(); });
			if (it == m_voices.end()) return {}; // every voice busy — drop it
			index = static_cast<size_t>(it - m_voices.begin());
		}
		if (!m_voices[index] ||
			!m_voices[index]->MatchesFormat(sound.channels, sound.sampleRate)) {
			m_voices[index] =
				std::make_unique<PooledVoice>(m_xaudio, sound.channels, sound.sampleRate);
			if (!m_voices[index]->IsValid()) {
				// ERASE it. A PooledVoice whose CreateSourceVoice failed still
				// reports idle and still matches its format, so leaving it in the
				// pool poisons the slot: the NEXT Play finds it in the search
				// above and calls Start on a null voice. The failure would show
				// up as a crash on some unrelated later sound, nowhere near the
				// device problem that actually caused it.
				m_voices.erase(m_voices.begin() + static_cast<ptrdiff_t>(index));
				return {};
			}
		}
	}

	PooledVoice& voice = *m_voices[index];
	voice.positional = positional;
	voice.bus = std::min(static_cast<u32>(params.bus), kBusCount - 1);
	if (positional) voice.emitter = *params.emitter;

	// Route before starting: SetOutputVoices is only legal on a stopped voice,
	// and it resets the output matrices, so the spatialization has to follow it.
	IXAudio2SubmixVoice* bus = m_buses[voice.bus];
	XAUDIO2_SEND_DESCRIPTOR sends[2]{};
	u32 sendCount = 0;
	if (bus) sends[sendCount++] = {0, bus};
	if (positional && m_reverb) sends[sendCount++] = {0, m_reverb};
	if (sendCount > 0) {
		XAUDIO2_VOICE_SENDS sendList{sendCount, sends};
		voice.Voice()->SetOutputVoices(&sendList);
	}

	voice.Start(sound, volume, pitch, params.loop);
	if (positional) Spatialize(voice);

	return VoiceHandle{static_cast<u32>(index), voice.Generation()};
}

void AudioEngine::Play(const assets::SoundData& sound, float volume, float pan,
					   float pitch) {
	PlayParams params;
	params.volume = volume;
	params.pitch = pitch;
	const VoiceHandle handle = Play(sound, params);

	// Constant-power pan for a mono source, applied across the front pair. The
	// 2D path keeps its explicit pan rather than routing through X3DAudio — a
	// UI click has no position, and inventing one for it would be a lie the
	// listener could hear when the party turns.
	PooledVoice* voice = Resolve(handle);
	if (!voice || voice->Channels() != 1 || m_destChannels < 2 ||
		std::abs(pan) < 1e-4f)
		return;

	float matrix[8]{};
	const float angle = (std::clamp(pan, -1.0f, 1.0f) * 0.5f + 0.5f) * 1.5707963f;
	matrix[0] = std::cos(angle);
	matrix[1] = std::sin(angle);
	if (m_buses[voice->bus])
		voice->Voice()->SetOutputMatrix(m_buses[voice->bus], 1, m_destChannels, matrix);
}

PooledVoice* AudioEngine::Resolve(VoiceHandle handle) const {
	if (!handle.Valid() || handle.slot >= m_voices.size()) return nullptr;
	PooledVoice* voice = m_voices[handle.slot].get();
	if (!voice || voice->Generation() != handle.generation) return nullptr;
	return voice;
}

void AudioEngine::Stop(VoiceHandle handle) {
	if (PooledVoice* voice = Resolve(handle)) voice->Halt();
}

bool AudioEngine::IsPlaying(VoiceHandle handle) const {
	const PooledVoice* voice = Resolve(handle);
	return voice && !voice->IsIdle();
}

void AudioEngine::SetPosition(VoiceHandle handle, const Vec3& position) {
	PooledVoice* voice = Resolve(handle);
	if (!voice || !voice->positional) return;
	voice->emitter.position = position;
	Spatialize(*voice);
}

void AudioEngine::SetVolume(VoiceHandle handle, float volume) {
	if (PooledVoice* voice = Resolve(handle)) {
		voice->volume = std::clamp(volume, 0.0f, 1.0f);
		voice->Voice()->SetVolume(voice->volume);
	}
}

// ============================================================================
// Spatialization
// ============================================================================

void AudioEngine::Spatialize(PooledVoice& voice) {
	if (!voice.positional || !voice.Voice()) return;

	X3DAUDIO_LISTENER listener{};
	listener.Position = {m_listener.position.x, m_listener.position.y,
						 m_listener.position.z};
	listener.OrientFront = {m_listener.forward.x, m_listener.forward.y,
							m_listener.forward.z};
	listener.OrientTop = {m_listener.up.x, m_listener.up.y, m_listener.up.z};

	X3DAUDIO_DISTANCE_CURVE falloff{};
	falloff.pPoints = const_cast<X3DAUDIO_DISTANCE_CURVE_POINT*>(kFalloffPoints);
	falloff.PointCount = static_cast<UINT32>(std::size(kFalloffPoints));

	X3DAUDIO_EMITTER emitter{};
	emitter.ChannelCount = 1;
	emitter.CurveDistanceScaler = std::max(voice.emitter.maxDistance, 0.1f);
	emitter.DopplerScaler = 0.0f; // see below
	emitter.Position = {voice.emitter.position.x, voice.emitter.position.y,
						voice.emitter.position.z};
	emitter.OrientFront = {0.0f, 0.0f, 1.0f};
	emitter.OrientTop = {0.0f, 1.0f, 0.0f};
	emitter.pVolumeCurve = &falloff;
	// Inside the inner radius the sound stops being a point and spreads across
	// the speakers — standing under a drip should feel like being under it, not
	// like it is 30cm to the left.
	emitter.InnerRadius = std::max(voice.emitter.minDistance, 0.0f);
	emitter.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;

	float matrix[8]{};
	X3DAUDIO_DSP_SETTINGS dsp{};
	dsp.SrcChannelCount = 1;
	dsp.DstChannelCount = m_destChannels;
	dsp.pMatrixCoefficients = matrix;

	// No DOPPLER: the party moves a square at a time at walking pace, so the
	// shift would be inaudible, and on a looping flight sound a wandering pitch
	// is more likely to read as a defect than as speed. The emitter keeps the
	// field so it can be switched on for something fast later.
	UINT32 flags = X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_LPF_DIRECT;
	if (m_reverb) flags |= X3DAUDIO_CALCULATE_REVERB;

	X3DAudioCalculate(reinterpret_cast<const X3DAUDIO_HANDLE&>(m_x3d), &listener,
					  &emitter, flags, &dsp);

	if (m_buses[voice.bus])
		voice.Voice()->SetOutputMatrix(m_buses[voice.bus], 1, m_destChannels, matrix);

	if (m_reverb) {
		float wet[2] = {dsp.ReverbLevel, dsp.ReverbLevel};
		voice.Voice()->SetOutputMatrix(m_reverb, 1, 2, wet);
	}

	// Distance dulls as well as quietens. The frequency formula is XAudio2's own
	// mapping from the coefficient to a one-pole cutoff; clamped away from zero
	// because a filter frequency of exactly 0 is rejected.
	XAUDIO2_FILTER_PARAMETERS filter{};
	filter.Type = LowPassFilter;
	filter.Frequency =
		std::clamp(2.0f * std::sin(X3DAUDIO_PI / 6.0f * dsp.LPFDirectCoefficient),
				   0.01f, XAUDIO2_MAX_FILTER_FREQUENCY);
	filter.OneOverQ = 1.0f;
	voice.Voice()->SetFilterParameters(&filter);
}

void AudioEngine::Update() {
	if (!m_xaudio) return;

	for (auto& slot : m_voices) {
		if (!slot) continue;
		PooledVoice& voice = *slot;
		if (voice.IsIdle()) {
			// A finished sound: retire its handle so a caller holding one
			// cannot later address whatever reuses this slot.
			if (!voice.retired) {
				voice.retired = true;
				voice.positional = false;
				voice.BumpGeneration();
			}
			continue;
		}
		if (voice.positional) Spatialize(voice);
	}
}

// ============================================================================
// Mix
// ============================================================================

void AudioEngine::SetMasterVolume(float volume) {
	m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
	if (m_master) m_master->SetVolume(m_masterVolume);
}

void AudioEngine::SetBusVolume(Bus bus, float volume) {
	const u32 index = static_cast<u32>(bus);
	if (index >= kBusCount) return;
	m_busVolumes[index] = std::clamp(volume, 0.0f, 1.0f);
	if (m_buses[index]) m_buses[index]->SetVolume(m_busVolumes[index]);
}

float AudioEngine::BusVolume(Bus bus) const {
	const u32 index = static_cast<u32>(bus);
	return index < kBusCount ? m_busVolumes[index] : 0.0f;
}

void AudioEngine::SetReverb(const ReverbSpace& space) {
	if (!m_reverb) return;

	// I3DL2 is the vocabulary the reverb is actually tuned in; XAudio2 ships the
	// conversion to its native parameters, so a space is described in terms an
	// ear understands (how long the tail runs, how bright it is) rather than in
	// filter coefficients.
	XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2{};
	i3dl2.WetDryMix = std::clamp(space.wetDryMix, 0.0f, 100.0f);
	i3dl2.Room = static_cast<INT32>(std::clamp(space.roomDb, -100.0f, 0.0f) * 100.0f);
	i3dl2.RoomHF = static_cast<INT32>(std::clamp(space.roomHfDb, -100.0f, 0.0f) * 100.0f);
	i3dl2.RoomRolloffFactor = 0.0f;
	i3dl2.DecayTime = std::clamp(space.decaySeconds, 0.1f, 20.0f);
	i3dl2.DecayHFRatio = 0.5f;
	i3dl2.Reflections = -1000;
	// I3DL2 states its delays in SECONDS (reflections 0..0.3), not the
	// milliseconds the native parameter block uses. Passing 10 here rather than
	// 0.01 asks for a ten-second pre-delay, which the converter clamps into
	// something that sounds like a cave the size of a county.
	i3dl2.ReflectionsDelay = std::clamp(space.reflectionsDelayMs * 0.001f, 0.0f, 0.3f);
	i3dl2.Reverb = -500;
	i3dl2.ReverbDelay = 0.02f;
	i3dl2.Diffusion = 100.0f;
	i3dl2.Density = 100.0f;
	i3dl2.HFReference = 5000.0f;

	XAUDIO2FX_REVERB_PARAMETERS params{};
	ReverbConvertI3DL2ToNative(&i3dl2, &params);
	m_reverb->SetEffectParameters(0, &params, sizeof(params));
}

} // namespace dungeon::audio
