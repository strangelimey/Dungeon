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

// WHAT THE CHANNELS MEAN, which a channel COUNT does not tell you. Averaging is
// the right fold for some layouts and actively wrong for others, so the layout
// is established first and the fold follows from it.
enum class Layout { Mono, Stereo, Quad, Ambisonic, Surround51, Unknown };

// Ambisonic B-format is the one that has to be recognised by NAME, because a
// four-channel file is otherwise indistinguishable from a quad recording — and
// the two want opposite treatment. Same idea as the importer detecting an
// OpenGL normal map from its filename: the convention is in the name because
// it is nowhere in the data.
Layout DetectLayout(u32 channels, const std::string& filename, bool forceAmbisonic) {
	std::string low = filename;
	std::ranges::transform(low, low.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	const bool named = low.find("b-format") != std::string::npos ||
					   low.find("bformat") != std::string::npos ||
					   low.find("ambix") != std::string::npos ||
					   low.find("ambisonic") != std::string::npos ||
					   low.find("fuma") != std::string::npos;

	if (forceAmbisonic || (channels == 4 && named)) return Layout::Ambisonic;
	switch (channels) {
	case 1: return Layout::Mono;
	case 2: return Layout::Stereo;
	case 4: return Layout::Quad;
	case 6: return Layout::Surround51;
	default: return Layout::Unknown;
	}
}

const char* LayoutName(Layout layout) {
	switch (layout) {
	case Layout::Mono: return "mono";
	case Layout::Stereo: return "stereo";
	case Layout::Quad: return "quad";
	case Layout::Ambisonic: return "ambisonic B-format";
	case Layout::Surround51: return "5.1";
	default: return "unknown layout";
	}
}

// Stereo's classic failure, CHECKED rather than assumed: material whose channels
// are out of phase cancels when folded — a wide, chorused or mid-side-encoded
// ambience collapses to something thin and hollow, or in the pathological case
// to near silence. The symptom in-game ("this drip sounds weak") points nowhere
// near the import step, so it is caught here.
void CheckCorrelation(const Buffer& in, const std::string& label) {
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

Buffer ToMono(const Buffer& in, Layout layout, const std::string& label) {
	if (in.channels <= 1) return in;
	if (layout == Layout::Stereo) CheckCorrelation(in, label);

	Buffer out;
	out.channels = 1;
	out.rate = in.rate;
	out.s.resize(in.frames());

	switch (layout) {
	case Layout::Ambisonic:
		// B-FORMAT IS NOT FOUR MICROPHONES. It is W — an omnidirectional
		// pressure signal — plus X/Y/Z, which are SIGNED directional gradients
		// describing where the energy came from. Averaging all four is not a
		// downmix in any sense: the gradients carry no independent content, they
		// are signed, and they sum toward nothing, so the result is a quieter
		// hollowed-out W with the gradient noise stirred in. W ALONE *is* the
		// mono recording, already correct and already the right level.
		log::Info("  ambisonic: taking W (the omni channel); X/Y/Z are directional "
				  "gradients and averaging them in would only hollow it out");
		for (size_t f = 0; f < in.frames(); ++f) out.s[f] = in.s[f * in.channels];
		break;

	case Layout::Surround51: {
		// ITU/ATSC fold: front pair at unity, centre and surrounds at -3 dB, and
		// LFE DISCARDED. The LFE is not a bass part of the mix — it is a
		// separate band-limited effects channel already ~10 dB hot, so averaging
		// it in lands a low-frequency lump on top of everything.
		constexpr float kAtten = 0.7071f;
		log::Info("  5.1: L+R at unity, C/Ls/Rs at -3 dB, LFE discarded");
		for (size_t f = 0; f < in.frames(); ++f) {
			const float* s = &in.s[f * in.channels];
			out.s[f] = (s[0] + s[1] + kAtten * s[2] + kAtten * (s[4] + s[5])) / 3.0f;
		}
		break;
	}

	default:
		// Stereo, quad, or something unrecognised: every channel is a real
		// microphone signal of the same scene, so the average is the fold.
		if (layout == Layout::Unknown)
			log::Warn("{}: {} channels with no known layout — averaging them, which "
					  "may not be right. Check the result.",
					  label, in.channels);
		for (size_t f = 0; f < in.frames(); ++f) {
			float sum = 0.0f;
			for (u32 c = 0; c < in.channels; ++c) sum += in.s[f * in.channels + c];
			out.s[f] = sum / in.channels;
		}
		break;
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

// ---------------------------------------------------------------------------
// Making a loop
// ---------------------------------------------------------------------------

// RMS over a window, in the units the level checks use.
double WindowRms(const Buffer& b, size_t firstFrame, size_t frames) {
	const size_t end = std::min(firstFrame + frames, b.frames());
	if (end <= firstFrame) return 0.0;
	double sum = 0.0;
	for (size_t f = firstFrame; f < end; ++f)
		for (u32 c = 0; c < b.channels; ++c) {
			const double v = b.s[f * b.channels + c];
			sum += v * v;
		}
	return std::sqrt(sum / ((end - firstFrame) * b.channels));
}

// Where to cut from, when the caller doesn't say. A field recording is not
// uniform — it opens with the recordist settling, it has the one dramatic event
// the library was sold on, and it has the long stretch of ordinary material in
// between. A BED wants the ordinary stretch, so the candidate whose RMS sits
// closest to the whole file's is chosen: not the loudest, not the quietest, the
// most typical. It is a cheap heuristic and it is meant to be overridden by ear.
size_t PickLoopStart(const Buffer& b, size_t loopFrames, size_t needFrames) {
	if (b.frames() <= needFrames) return 0;
	const double target = WindowRms(b, 0, b.frames());
	const size_t last = b.frames() - needFrames;
	const size_t stride = std::max<size_t>(loopFrames / 8, 1);

	size_t best = 0;
	double bestErr = 1e30;
	for (size_t start = 0; start <= last; start += stride) {
		const double err = std::abs(WindowRms(b, start, loopFrames) - target);
		if (err < bestErr) {
			bestErr = err;
			best = start;
		}
	}
	return best;
}

// Cut a segment and CROSSFADE its tail onto its head so it wraps seamlessly.
//
// This is the one place the module repairs rather than reports, and the reason
// is that the material is different in kind. Crossfading something authored
// seamless would smear it — but a field recording has no seam to preserve, and
// ambience is stochastic, so a quarter-second equal-power blend of two arbitrary
// moments of the same room is inaudible. It is how every ambient bed is made.
//
// EQUAL POWER, not linear: the two sides are uncorrelated, so their powers add
// while their amplitudes do not. A linear fade would dip ~3 dB in the middle of
// every wrap — a slow breathing pulse once per cycle, which is exactly the kind
// of defect that is maddening to find later because it is not a click.
Buffer MakeLoop(const Buffer& in, float seconds, float fromSeconds, float fadeMs,
				const std::string& label) {
	const size_t loopFrames = static_cast<size_t>(seconds * in.rate);
	size_t fadeFrames = static_cast<size_t>(fadeMs * 0.001f * in.rate);
	if (loopFrames < 2 || in.frames() < loopFrames + fadeFrames) {
		log::Warn("{}: source is {:.1f}s — too short for a {:.1f}s loop plus its "
				  "crossfade. Using the whole thing.",
				  label, in.seconds(), seconds);
		return in;
	}
	fadeFrames = std::min(fadeFrames, loopFrames / 2);

	const size_t need = loopFrames + fadeFrames;
	const size_t start = fromSeconds >= 0.0f
							 ? std::min(static_cast<size_t>(fromSeconds * in.rate),
										in.frames() - need)
							 : PickLoopStart(in, loopFrames, need);

	Buffer out;
	out.channels = in.channels;
	out.rate = in.rate;
	out.s.assign(loopFrames * in.channels, 0.0f);

	for (size_t f = 0; f < loopFrames; ++f)
		for (u32 c = 0; c < in.channels; ++c)
			out.s[f * out.channels + c] = in.s[(start + f) * in.channels + c];

	// Blend the material that FOLLOWS the loop over the loop's opening, so the
	// last sample runs into the first without a step.
	for (size_t f = 0; f < fadeFrames; ++f) {
		const float t = static_cast<float>(f) / fadeFrames;
		const float rising = std::sin(t * 1.5707963f);
		const float falling = std::cos(t * 1.5707963f);
		for (u32 c = 0; c < in.channels; ++c) {
			const float head = in.s[(start + f) * in.channels + c];
			const float tail = in.s[(start + loopFrames + f) * in.channels + c];
			out.s[f * out.channels + c] = head * rising + tail * falling;
		}
	}

	const double whole = WindowRms(in, 0, in.frames());
	const double cut = WindowRms(out, 0, out.frames());
	log::Info("  loop: {:.1f}s cut at {:.1f}s, {:.0f}ms equal-power crossfade "
			  "(segment is {:+.1f} dB against the whole recording)",
			  seconds, static_cast<float>(start) / in.rate,
			  static_cast<float>(fadeFrames) * 1000.0f / in.rate,
			  Dbfs(static_cast<float>(cut)) - Dbfs(static_cast<float>(whole)));
	return out;
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

	const Layout layout =
		DetectLayout(b.channels, src.filename().string(), opts.ambisonic);
	if (b.channels > 1) log::Info("  layout: {}", LayoutName(layout));

	if (opts.mono) {
		b = ToMono(b, layout, label);
	} else if (layout == Layout::Ambisonic) {
		// --stereo on B-format would ship W/X/Y/Z as if they were speaker feeds,
		// which is not a bed — it is a pressure signal and three gradients played
		// at the listener. Nothing downstream can rescue that, so say so here.
		log::Error("{}: this is ambisonic B-format and cannot be used as a stereo "
				   "bed — its channels are not speaker feeds. Import it positional "
				   "(the default) to take W, or decode it to stereo first.",
				   label);
		return false;
	}

	if (b.rate != opts.rate) {
		log::Info("  resample: {} -> {} Hz", b.rate, opts.rate);
		b = Resample(b, opts.rate);
	}
	// Cut the loop after resampling, so the crossfade is computed at the rate
	// the file will actually play at, and before levelling, so normalization
	// measures the loop rather than the recording it came from.
	if (opts.loopSeconds > 0.0f)
		b = MakeLoop(b, opts.loopSeconds, opts.loopFromSeconds, opts.loopFadeMs, label);

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
