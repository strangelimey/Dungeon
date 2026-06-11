#include "Game/ProceduralSounds.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace dungeon::game {

namespace {

constexpr u32 kRate = 22050;

assets::SoundData MakeSound(float seconds) {
    assets::SoundData s;
    s.channels = 1;
    s.sampleRate = kRate;
    s.samples.resize(static_cast<size_t>(seconds * kRate));
    return s;
}

i16 Pack(float v) {
    return static_cast<i16>(std::clamp(v, -1.0f, 1.0f) * 32000.0f);
}

// Cheap deterministic noise generator.
struct Noise {
    std::mt19937 rng{12345};
    std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
    float lowpass = 0.0f;

    float White() { return dist(rng); }
    float Brown(float cutoff) { // one-pole lowpassed noise
        lowpass += cutoff * (White() - lowpass);
        return lowpass;
    }
};

} // namespace

assets::SoundData MakeFootstepSound() {
    assets::SoundData s = MakeSound(0.18f);
    Noise noise;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::exp(-t * 38.0f);
        const float thump = std::sin(2 * 3.14159f * 70.0f * t) * std::exp(-t * 50.0f);
        const float scuff = noise.Brown(0.22f) * 0.7f;
        s.samples[i] = Pack((thump * 0.8f + scuff * 0.5f) * env);
    }
    return s;
}

assets::SoundData MakeBumpSound() {
    assets::SoundData s = MakeSound(0.25f);
    Noise noise;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::exp(-t * 22.0f);
        const float thud = std::sin(2 * 3.14159f * 55.0f * t * (1.0f - t * 0.8f));
        const float rattle = noise.Brown(0.4f) * 0.3f * std::exp(-t * 40.0f);
        s.samples[i] = Pack((thud * 0.9f + rattle) * env);
    }
    return s;
}

assets::SoundData MakeTurnSound() {
    assets::SoundData s = MakeSound(0.12f);
    Noise noise;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::sin(3.14159f * t / 0.12f); // swell in and out
        s.samples[i] = Pack(noise.Brown(0.5f) * env * 0.18f);
    }
    return s;
}

assets::SoundData MakeUiClickSound() {
    assets::SoundData s = MakeSound(0.05f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = std::exp(-t * 120.0f);
        s.samples[i] = Pack(std::sin(2 * 3.14159f * 900.0f * t) * env * 0.4f);
    }
    return s;
}

} // namespace dungeon::game
