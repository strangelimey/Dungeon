// ============================================================================
// SoundBaker.cpp — synthesized sound effects, written as 22.05kHz mono WAVs.
//
// Each effect is a few lines of additive synthesis: a sine "body" plus
// low-passed ("brown") noise, shaped by an exponential or half-sine
// envelope. Deterministic (fixed RNG seed) so rebakes are reproducible.
// ============================================================================
#include "SoundBaker.h"

#include "Core/Log.h"
#include "Core/Types.h"

#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace dungeon::baker {

namespace {

constexpr u32 kRate = 22050;

i16 Pack(float v) { return static_cast<i16>(std::clamp(v, -1.0f, 1.0f) * 32000.0f); }

struct Noise {
    std::mt19937 rng{12345};
    std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
    float lowpass = 0.0f;
    float White() { return dist(rng); }
    float Brown(float cutoff) {
        lowpass += cutoff * (White() - lowpass);
        return lowpass;
    }
};

bool WriteWav(const std::string& path, const std::vector<i16>& samples) {
    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = kRate;
    format.bitsPerSample = 16;

    drwav wav;
    if (!drwav_init_file_write(&wav, path.c_str(), &format, nullptr)) {
        log::Error("Cannot write {}", path);
        return false;
    }
    drwav_write_pcm_frames(&wav, samples.size(), samples.data());
    drwav_uninit(&wav);
    log::Info("Wrote {}", path);
    return true;
}

std::vector<i16> Footstep() {
    std::vector<i16> s(static_cast<size_t>(0.18f * kRate));
    Noise noise;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::exp(-t * 38.0f);
        const float thump = std::sin(2 * 3.14159f * 70.0f * t) * std::exp(-t * 50.0f);
        const float scuff = noise.Brown(0.22f) * 0.7f;
        s[i] = Pack((thump * 0.8f + scuff * 0.5f) * env);
    }
    return s;
}

std::vector<i16> Bump() {
    std::vector<i16> s(static_cast<size_t>(0.25f * kRate));
    Noise noise;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::exp(-t * 22.0f);
        const float thud = std::sin(2 * 3.14159f * 55.0f * t * (1.0f - t * 0.8f));
        const float rattle = noise.Brown(0.4f) * 0.3f * std::exp(-t * 40.0f);
        s[i] = Pack((thud * 0.9f + rattle) * env);
    }
    return s;
}

std::vector<i16> Turn() {
    std::vector<i16> s(static_cast<size_t>(0.12f * kRate));
    Noise noise;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::sin(3.14159f * t / 0.12f);
        s[i] = Pack(noise.Brown(0.5f) * env * 0.18f);
    }
    return s;
}

std::vector<i16> Click() {
    std::vector<i16> s(static_cast<size_t>(0.05f * kRate));
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        s[i] = Pack(std::sin(2 * 3.14159f * 900.0f * t) * std::exp(-t * 120.0f) * 0.4f);
    }
    return s;
}

std::vector<i16> MonsterGroan() {
    // Low wavering drone — played when a monster blocks the way.
    std::vector<i16> s(static_cast<size_t>(0.6f * kRate));
    Noise noise;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::sin(3.14159f * t / 0.6f);
        const float wobble = 1.0f + 0.08f * std::sin(2 * 3.14159f * 5.0f * t);
        const float tone = std::sin(2 * 3.14159f * 95.0f * wobble * t) * 0.5f +
                           std::sin(2 * 3.14159f * 142.0f * wobble * t) * 0.25f;
        s[i] = Pack((tone + noise.Brown(0.15f) * 0.35f) * env * 0.7f);
    }
    return s;
}

} // namespace

bool BakeSounds(const std::string& dir) {
    bool ok = true;
    ok &= WriteWav(dir + "\\footstep.wav", Footstep());
    ok &= WriteWav(dir + "\\bump.wav", Bump());
    ok &= WriteWav(dir + "\\turn.wav", Turn());
    ok &= WriteWav(dir + "\\click.wav", Click());
    ok &= WriteWav(dir + "\\monster.wav", MonsterGroan());
    return ok;
}

} // namespace dungeon::baker
