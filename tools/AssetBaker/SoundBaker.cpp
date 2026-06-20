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
#include <filesystem>
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

std::vector<i16> Oof() {
	// Short pained grunt — played when the party walks face-first into a wall.
	// A vocal-ish tone that drops in pitch over the grunt, plus a breathy noise
	// transient on the attack. Fast attack, quick decay (~0.22s total).
	std::vector<i16> s(static_cast<size_t>(0.22f * kRate));
	Noise noise;
	for (size_t i = 0; i < s.size(); ++i) {
		const float t = static_cast<float>(i) / kRate;
		const float env = (1.0f - std::exp(-t * 90.0f)) * std::exp(-t * 12.0f);
		// Pitch falls from ~170Hz to ~95Hz over the first ~0.18s.
		const float f = 170.0f - 75.0f * std::min(t / 0.18f, 1.0f);
		const float tone = std::sin(2 * 3.14159f * f * t) * 0.6f +
						   std::sin(2 * 3.14159f * f * 2.0f * t) * 0.2f;
		const float breath = noise.Brown(0.3f) * 0.25f * std::exp(-t * 20.0f);
		s[i] = Pack((tone + breath) * env);
	}
	return s;
}

std::vector<i16> SpellCast() {
	// Rising arcane shimmer — an upward pitch sweep with bright overtones over a
	// breath of airy noise, the gathered energy releasing. ~0.35s.
	std::vector<i16> s(static_cast<size_t>(0.35f * kRate));
	Noise noise;
	for (size_t i = 0; i < s.size(); ++i) {
		const float t = static_cast<float>(i) / kRate;
		const float env = (1.0f - std::exp(-t * 35.0f)) * std::exp(-t * 6.0f);
		// Pitch sweeps up from ~330Hz to ~880Hz over the first ~0.3s.
		const float f = 330.0f + 550.0f * std::min(t / 0.3f, 1.0f);
		const float tone = std::sin(2 * 3.14159f * f * t) * 0.5f +
						   std::sin(2 * 3.14159f * f * 2.01f * t) * 0.25f +
						   std::sin(2 * 3.14159f * f * 3.0f * t) * 0.12f;
		const float shimmer = noise.Brown(0.6f) * 0.2f;
		s[i] = Pack((tone + shimmer) * env * 0.7f);
	}
	return s;
}

std::vector<i16> SpellImpact() {
	// Bright arcane burst — a sharp noisy crack over a quickly-falling tone, the
	// bolt striking home. Fast attack, ~0.28s decay.
	std::vector<i16> s(static_cast<size_t>(0.28f * kRate));
	Noise noise;
	for (size_t i = 0; i < s.size(); ++i) {
		const float t = static_cast<float>(i) / kRate;
		const float env = std::exp(-t * 18.0f);
		// Tone drops from ~520Hz as the energy dissipates.
		const float f = 520.0f - 300.0f * std::min(t / 0.2f, 1.0f);
		const float tone = std::sin(2 * 3.14159f * f * t) * 0.45f +
						   std::sin(2 * 3.14159f * f * 1.5f * t) * 0.2f;
		const float crack = noise.Brown(0.7f) * 0.6f * std::exp(-t * 45.0f);
		s[i] = Pack((tone + crack) * env * 0.85f);
	}
	return s;
}

std::vector<i16> SpellFizzle() {
	// A failed cast — a deflating downward warble sputtering into noise, the
	// spell collapsing. ~0.3s.
	std::vector<i16> s(static_cast<size_t>(0.3f * kRate));
	Noise noise;
	for (size_t i = 0; i < s.size(); ++i) {
		const float t = static_cast<float>(i) / kRate;
		const float env = std::sin(3.14159f * t / 0.3f);
		// Pitch sags from ~400Hz down to ~120Hz — the energy draining away.
		const float f = 400.0f - 280.0f * std::min(t / 0.3f, 1.0f);
		const float warble = 1.0f + 0.15f * std::sin(2 * 3.14159f * 18.0f * t);
		const float tone = std::sin(2 * 3.14159f * f * warble * t) * 0.4f;
		const float sputter = noise.Brown(0.35f) * 0.4f;
		s[i] = Pack((tone + sputter) * env * 0.6f);
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
	ok &= WriteWav(dir + "\\oof.wav", Oof());

	// Spell effects live in a dedicated subfolder.
	std::error_code ec;
	std::filesystem::create_directories(dir + "\\spells", ec);
	ok &= WriteWav(dir + "\\spells\\cast.wav", SpellCast());
	ok &= WriteWav(dir + "\\spells\\impact.wav", SpellImpact());
	ok &= WriteWav(dir + "\\spells\\fizzle.wav", SpellFizzle());
	return ok;
}

} // namespace dungeon::baker
