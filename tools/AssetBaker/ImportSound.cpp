// ============================================================================
// ImportSound.cpp — bought audio in, engine format out.
//
// The engine plays 16-bit interleaved PCM straight out of memory with no
// decode and no conversion (Assets/Wav.h, AudioEngine's zero-copy Play), so an
// import is a NORMALIZATION: resample, downmix, level, trim. Everything a
// bought library varies — 48k vs 96k, stereo vs mono, wildly different
// mastering levels, seconds of room tone before the hit — is dealt with here,
// once, offline, instead of at runtime.
//
// THE ONE RULE, and it is not a preference: 3D positional audio needs MONO
// sources. X3DAudio's whole job is to compute a per-channel output matrix from
// the geometry between emitter and listener, and a stereo file has already
// committed its channels — there is nothing left for it to place. So anything
// that belongs to a place or a body (a drip, a door, a footstep, a sword) is
// downmixed here, and only the non-positional survivors (the level's ambient
// bed, UI) keep their stereo image. The failure mode is silent rather than
// loud: a stereo drip simply refuses to move as you walk past it.
//
// Processing runs in float and converts back once at the end, so the rounding
// to 16-bit happens after the gain change rather than before it.
// ============================================================================
#include "ImportSound.h"

#include "Assets/Wav.h"
#include "Core/Log.h"

#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

namespace dungeon::baker {

namespace {

// Interleaved float audio — the working form for every pass below.
struct Buffer {
	u32 channels = 0;
	u32 rate = 0;
	std::vector<float> s;

	size_t frames() const { return channels ? s.size() / channels : 0; }
	float seconds() const { return rate ? static_cast<float>(frames()) / rate : 0.0f; }
};

constexpr float kSilenceDbfs = -60.0f; // below this counts as nothing
constexpr float kTrimPadMs = 5.0f;     // keep a hair either side of a trim

float Dbfs(float amplitude) {
	return amplitude > 1e-9f ? 20.0f * std::log10(amplitude) : -120.0f;
}

float FromDbfs(float db) { return std::pow(10.0f, db / 20.0f); }

Buffer ToFloat(const assets::SoundData& in) {
	Buffer b;
	b.channels = in.channels;
	b.rate = in.sampleRate;
	b.s.resize(in.samples.size());
	for (size_t i = 0; i < in.samples.size(); ++i) b.s[i] = in.samples[i] / 32768.0f;
	return b;
}

float Peak(const Buffer& b) {
	float peak = 0.0f;
	for (const float v : b.s) peak = std::max(peak, std::abs(v));
	return peak;
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------

// Windowed-sinc resample, per channel. This is an OFFLINE bake, so a wide
// kernel costs nothing we care about and linear interpolation would be a false
// economy: the cutoff has to follow the LOWER of the two rates, or a downsample
// folds everything above the new Nyquist back into the audible band as alias
// tones. On a bright sound — a sword, a splash, anything with real top end —
// that reads as a metallic ring that no amount of later EQ removes.
Buffer Resample(const Buffer& in, u32 rate) {
	if (in.rate == rate || in.frames() == 0) return in;

	constexpr int kTaps = 16; // kernel radius, in source samples
	const double ratio = static_cast<double>(rate) / in.rate;
	const double cutoff = std::min(1.0, ratio); // band-limit before decimating

	Buffer out;
	out.channels = in.channels;
	out.rate = rate;
	const size_t outFrames = static_cast<size_t>(in.frames() * ratio);
	out.s.assign(outFrames * in.channels, 0.0f);

	const size_t inFrames = in.frames();
	for (size_t f = 0; f < outFrames; ++f) {
		const double srcPos = f / ratio;
		const auto center = static_cast<ptrdiff_t>(std::floor(srcPos));

		for (u32 c = 0; c < in.channels; ++c) {
			double acc = 0.0, weight = 0.0;
			for (int t = -kTaps; t <= kTaps; ++t) {
				const ptrdiff_t idx = center + t;
				if (idx < 0 || static_cast<size_t>(idx) >= inFrames) continue;

				const double x = srcPos - idx;
				// sinc, windowed by a Blackman of the same radius.
				double sinc = 1.0;
				if (std::abs(x) > 1e-9) {
					const double px = std::numbers::pi * x * cutoff;
					sinc = std::sin(px) / px;
				}
				const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * x / (kTaps + 1));
				const double k = sinc * cutoff * w;

				acc += in.s[static_cast<size_t>(idx) * in.channels + c] * k;
				weight += k;
			}
			// Normalize by the kernel's actual sum: at the very start and end
			// the window is truncated, and without this the first and last few
			// samples come out quiet — a fade-in nobody asked for.
			out.s[f * out.channels + c] =
				static_cast<float>(weight > 1e-9 ? acc / weight : acc);
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Downmix
// ---------------------------------------------------------------------------

// Average the channels. Averaging (not summing) keeps the level, and for the
// stereo case it is what every DAW's mono fold-down does.
//
// It has one classic failure, so it is CHECKED rather than assumed: material
// whose channels are out of phase cancels when folded — a wide, chorused or
// mid-side-encoded ambience can collapse to something thin and hollow, or in
// the pathological case to near silence. Correlation catches that before the
// file is written, because the symptom in-game ("this drip sounds weak") does
// not point anywhere near the import step.
Buffer ToMono(const Buffer& in, const std::string& label) {
	if (in.channels <= 1) return in;

	if (in.channels == 2) {
		double lr = 0.0, ll = 0.0, rr = 0.0;
		for (size_t f = 0; f < in.frames(); ++f) {
			const double l = in.s[f * 2], r = in.s[f * 2 + 1];
			lr += l * r;
			ll += l * l;
			rr += r * r;
		}
		const double denom = std::sqrt(ll * rr);
		const double correlation = denom > 1e-12 ? lr / denom : 1.0;
		if (correlation < 0.1) {
			log::Warn("{}: channels correlate {:.2f} — this is wide or out-of-phase "
					  "stereo and will thin out when folded to mono. Check it, or "
					  "import one channel only.",
					  label, correlation);
		}
	}

	Buffer out;
	out.channels = 1;
	out.rate = in.rate;
	out.s.resize(in.frames());
	for (size_t f = 0; f < in.frames(); ++f) {
		float sum = 0.0f;
		for (u32 c = 0; c < in.channels; ++c) sum += in.s[f * in.channels + c];
		out.s[f] = sum / in.channels;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Level and length
// ---------------------------------------------------------------------------

void Normalize(Buffer& b, float peakDbfs, const std::string& label) {
	const float peak = Peak(b);
	if (peak < 1e-6f) {
		log::Warn("{}: silent (peak {:.1f} dBFS) — nothing to normalize", label,
				  Dbfs(peak));
		return;
	}
	const float gain = FromDbfs(peakDbfs) / peak;
	for (float& v : b.s) v = std::clamp(v * gain, -1.0f, 1.0f);
	log::Info("  level: peak {:.1f} -> {:.1f} dBFS ({:+.1f} dB)", Dbfs(peak), peakDbfs,
			  Dbfs(gain));
}

// Strip silence either end, keeping a few ms of pad so an attack transient is
// never clipped off. NEVER applied to a loop: the tail of a bed is half of its
// seam, and trimming it is how a seamless loop acquires a click.
void Trim(Buffer& b) {
	const float floorAmp = FromDbfs(kSilenceDbfs);
	const size_t frames = b.frames();

	size_t first = 0, last = frames;
	for (; first < frames; ++first) {
		bool loud = false;
		for (u32 c = 0; c < b.channels; ++c)
			loud = loud || std::abs(b.s[first * b.channels + c]) > floorAmp;
		if (loud) break;
	}
	for (; last > first; --last) {
		bool loud = false;
		for (u32 c = 0; c < b.channels; ++c)
			loud = loud || std::abs(b.s[(last - 1) * b.channels + c]) > floorAmp;
		if (loud) break;
	}
	if (first >= last) return; // all silence — leave it, the warning is elsewhere

	const auto pad = static_cast<size_t>(kTrimPadMs * 0.001f * b.rate);
	first = first > pad ? first - pad : 0;
	last = std::min(frames, last + pad);
	if (first == 0 && last == frames) return;

	b.s = std::vector<float>(b.s.begin() + first * b.channels,
							 b.s.begin() + last * b.channels);
	log::Info("  trim: {} -> {} frames ({:.0f} ms of silence removed)", frames,
			  b.frames(), (frames - b.frames()) * 1000.0f / b.rate);
}

// A loop's seam is the join from its last sample back to its first. Measure the
// step across that join against the signal's own typical sample-to-sample step:
// a seamless loop's seam is unremarkable, a bad one is a discontinuity an order
// of magnitude larger, and it ticks once per cycle forever.
//
// This REPORTS rather than repairs. Crossfading a bed that was authored
// seamless would smear it, and a bed that was not is the wrong file — better to
// know at import than to hunt a tick later.
void CheckSeam(const Buffer& b, const std::string& label) {
	const size_t frames = b.frames();
	if (frames < 2) return;

	double typical = 0.0;
	for (size_t f = 1; f < frames; ++f)
		for (u32 c = 0; c < b.channels; ++c) {
			const double d = b.s[f * b.channels + c] - b.s[(f - 1) * b.channels + c];
			typical += d * d;
		}
	typical = std::sqrt(typical / ((frames - 1) * b.channels));

	double seam = 0.0;
	for (u32 c = 0; c < b.channels; ++c) {
		const double d = b.s[c] - b.s[(frames - 1) * b.channels + c];
		seam += d * d;
	}
	seam = std::sqrt(seam / b.channels);

	const double ratio = typical > 1e-9 ? seam / typical : 0.0;
	if (ratio > 8.0) {
		log::Warn("{}: loop seam is {:.0f}x the typical sample step — this will "
				  "tick once per cycle. The source is probably not a seamless loop.",
				  label, ratio);
	} else {
		log::Info("  seam: {:.1f}x typical step — clean", ratio);
	}
}

// ---------------------------------------------------------------------------

bool WriteWav(const std::string& path, const Buffer& b) {
	std::vector<i16> pcm(b.s.size());
	for (size_t i = 0; i < b.s.size(); ++i)
		pcm[i] = static_cast<i16>(std::clamp(b.s[i], -1.0f, 1.0f) * 32767.0f);

	drwav_data_format format{};
	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_PCM;
	format.channels = static_cast<drwav_uint32>(b.channels);
	format.sampleRate = b.rate;
	format.bitsPerSample = 16;

	drwav wav;
	if (!drwav_init_file_write(&wav, path.c_str(), &format, nullptr)) {
		log::Error("Cannot write {}", path);
		return false;
	}
	drwav_write_pcm_frames(&wav, b.frames(), pcm.data());
	drwav_uninit(&wav);
	return true;
}

bool ImportOne(const std::filesystem::path& src, const std::string& outPath,
			   const std::string& label, const SoundImportOptions& opts) {
	auto loaded = assets::LoadWavFile(src.string());
	if (!loaded) {
		log::Error("{}: {}", label, loaded.error());
		return false;
	}

	Buffer b = ToFloat(*loaded);
	log::Info("{}: {} Hz, {} ch, {:.2f}s", label, b.rate, b.channels, b.seconds());
	if (b.frames() == 0) {
		log::Error("{}: no audio frames", label);
		return false;
	}

	if (opts.mono) b = ToMono(b, label);
	if (b.rate != opts.rate) {
		log::Info("  resample: {} -> {} Hz", b.rate, opts.rate);
		b = Resample(b, opts.rate);
	}
	if (opts.trim && !opts.loop) Trim(b);

	// REFUSE to write silence. There is no legitimate silent asset, and every
	// route to one here is a mistake worth catching now: a fully out-of-phase
	// stereo file that cancelled in the fold, a source that was silent to begin
	// with, a bad trim. Warning and writing anyway (which this did until the
	// out-of-phase test caught it) buries the mistake in a file that loads,
	// plays, and produces nothing — the hardest kind of bug to trace back.
	if (const float peak = Peak(b); Dbfs(peak) < kSilenceDbfs) {
		log::Error("{}: result is silent ({:.1f} dBFS) — refusing to write. {}", label,
				   Dbfs(peak),
				   opts.mono && loaded->channels > 1
					   ? "The channels cancelled in the mono fold; import one "
						 "channel only, or keep it --stereo if it is a bed."
					   : "The source is silent or the trim ate it.");
		return false;
	}

	if (opts.normalize) Normalize(b, opts.peakDbfs, label);
	if (opts.loop) CheckSeam(b, label);

	if (!WriteWav(outPath, b)) return false;
	log::Info("  -> {} ({} Hz, {} ch, {:.2f}s)", outPath, b.rate, b.channels,
			  b.seconds());
	return true;
}

bool IsWav(const std::filesystem::path& p) {
	std::string ext = p.extension().string();
	std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext == ".wav";
}

} // namespace

bool ImportSound(const std::string& src, const std::string& assetsDir,
				 const std::string& name, const SoundImportOptions& opts) {
	namespace fs = std::filesystem;

	const fs::path source(src);
	if (!fs::exists(source)) {
		log::Error("Sound source not found: {}", src);
		return false;
	}

	const fs::path outDir = fs::path(assetsDir) / "sounds";
	std::error_code ec;
	fs::create_directories(outDir, ec);

	// A single file is one sound; a folder is the VARIANT case — several takes
	// of the same thing, numbered, for the round-robin that stops a footstep
	// sounding like a machine gun.
	if (fs::is_regular_file(source)) {
		if (!IsWav(source)) {
			log::Error("{}: only .wav is supported (the vendored decoder is "
					   "dr_wav). Convert it first.",
					   source.filename().string());
			return false;
		}
		return ImportOne(source, (outDir / (name + ".wav")).string(), name, opts);
	}

	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(source, ec))
		if (entry.is_regular_file() && IsWav(entry.path())) files.push_back(entry.path());

	if (files.empty()) {
		log::Error("No .wav files in {}", src);
		return false;
	}
	// Directory order is filesystem-defined; sort so variant numbering is
	// stable across machines and a re-import doesn't shuffle them.
	std::ranges::sort(files);

	log::Info("{}: {} variants", name, files.size());
	bool ok = true;
	for (size_t i = 0; i < files.size(); ++i) {
		const std::string variant = name + "_" + std::to_string(i + 1);
		ok &= ImportOne(files[i], (outDir / (variant + ".wav")).string(), variant, opts);
	}
	return ok;
}

} // namespace dungeon::baker
