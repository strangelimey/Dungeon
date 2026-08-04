// ============================================================================
// Bc7Test — the BC7 encoder's regression run.
//
// The encoder used to be judged by eye and by a PSNR number computed once, by
// hand, in a session that is now history. This is the re-runnable version, in
// the same spirit as tools\AllocTest.ps1: a corpus, a verdict, an exit code.
//
// Three things are checked, in increasing order of how much they would hurt:
//
//  1. CONSISTENCY (the correctness gate). Every block carries the error the
//     encoder BELIEVES it has. That number decides which mode gets written, so
//     if it is wrong, mode selection is choosing at random and quality claims
//     mean nothing. The harness decodes the packed bytes with its own decoder
//     (Bc7Decode.cpp) and demands the two agree EXACTLY — not within a
//     tolerance. They can: every term is a small integer, so the float sums are
//     exact whatever order they are added in. A wrong bit offset, a dropped
//     anchor MSB, a p-bit on the wrong endpoint or a partition read against the
//     wrong subset all break this.
//
//  2. THREAD INVARIANCE. Blocks are independent, so the fan-out must not change
//     a single byte. Encoded once single-threaded and once fanned out, the two
//     buffers must be identical.
//
//  3. QUALITY, against a recorded baseline, so a refactor that quietly loses a
//     dB is a failure rather than a discovery six months later.
//
// The corpus is mostly SYNTHETIC and generated here, deterministically: the
// real textures are gitignored, so a corpus that depended on them would not
// run on a fresh clone. Each synthetic image isolates one thing the encoder is
// supposed to be good or bad at. Real textures are added when --assets points
// at an installed pool, and they are cropped, not scaled — a resample would
// invent block content that no shipped texture contains.
//
//   Bc7Test                                    synthetic corpus, defaults
//   Bc7Test --assets ..\..\assets              plus a sample of real textures
//   Bc7Test --baseline tools\bc7-baseline.txt  fail on quality regression
//   Bc7Test --audit                            the measurement matrix
//   Bc7Test --self-test                        corrupt the bytes; must FAIL
// ============================================================================
#include "AssetBaker/Bc7Encoder.h"
#include "Assets/Image.h"
#include "Bc7Decode.h"
#include "Core/Types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dungeon;

namespace {

struct Sample {
	std::string name;
	assets::ImageData image;
};

// ---- Deterministic synthetic corpus -----------------------------------------

// A plain LCG. The corpus must be identical on every machine and every run, so
// nothing here may reach for a random device or a clock.
struct Lcg {
	u32 s;
	u32 Next() {
		s = s * 1664525u + 1013904223u;
		return s >> 8;
	}
	float Unit() { return static_cast<float>(Next() % 65536) / 65535.0f; }
};

assets::ImageData MakeImage(u32 w, u32 h) {
	assets::ImageData img;
	img.width = w;
	img.height = h;
	img.pixels.assign(static_cast<size_t>(w) * h * 4, u8{255});
	return img;
}

void Put(assets::ImageData& img, u32 x, u32 y, int r, int g, int b, int a) {
	u8* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
	p[0] = static_cast<u8>(std::clamp(r, 0, 255));
	p[1] = static_cast<u8>(std::clamp(g, 0, 255));
	p[2] = static_cast<u8>(std::clamp(b, 0, 255));
	p[3] = static_cast<u8>(std::clamp(a, 0, 255));
}

// Hard two-material boundaries: mortar lines through brick. THE case multi-mode
// encoding exists for — a 4x4 block landing on a mortar line has to describe
// two colour clusters, which one line through the middle cannot do.
assets::ImageData MakeBrick(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	Lcg rng{12345};
	for (u32 y = 0; y < h; ++y) {
		for (u32 x = 0; x < w; ++x) {
			const u32 row = y / 16;
			const u32 sx = x + (row & 1 ? 16 : 0); // stagger alternate courses
			const bool mortar = (y % 16) < 3 || (sx % 32) < 3;
			const int n = static_cast<int>(rng.Unit() * 12.0f) - 6;
			if (mortar) Put(img, x, y, 132 + n, 128 + n, 120 + n, 255);
			else Put(img, x, y, 158 + n, 74 + n, 52 + n, 255);
		}
	}
	return img;
}

// A smooth 2-D gradient: no edges at all, so a single well-placed colour line
// with plenty of index positions should win outright.
assets::ImageData MakeGradient(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) {
			const float u = static_cast<float>(x) / static_cast<float>(w - 1);
			const float v = static_cast<float>(y) / static_cast<float>(h - 1);
			Put(img, x, y, static_cast<int>(u * 255.0f),
				static_cast<int>(v * 255.0f),
				static_cast<int>((1.0f - u) * 200.0f + 30.0f), 255);
		}
	return img;
}

// Uncorrelated noise — the block-compression worst case, and a good tripwire
// for any code path that assumes the pixels lie near a line.
assets::ImageData MakeNoise(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	Lcg rng{777};
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x)
			Put(img, x, y, static_cast<int>(rng.Unit() * 255.0f),
				static_cast<int>(rng.Unit() * 255.0f),
				static_cast<int>(rng.Unit() * 255.0f), 255);
	return img;
}

// One colour everywhere: exercises the degenerate paths (zero covariance, a
// singular least-squares fit, a zero-length endpoint segment) that only ever
// run on flat blocks.
assets::ImageData MakeFlat(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) Put(img, x, y, 96, 112, 128, 255);
	return img;
}

// A normal map with HEIGHT IN ALPHA — the project's actual _n.png layout, and
// mode 5's reason for existing. The RGB is a smooth low-frequency normal field;
// the alpha is a stepped height that shares none of its structure. A single
// 4-D line has to serve both, so every alpha step drags the normal off-line.
assets::ImageData MakeNormalHeight(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) {
			const float fx = static_cast<float>(x) * 0.19f;
			const float fy = static_cast<float>(y) * 0.13f;
			const float nx = std::sin(fx) * 0.35f;
			const float ny = std::cos(fy) * 0.35f;
			const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
			// Height: coarse plateaus on a different lattice from the normal.
			const int step = static_cast<int>((x / 9 + y / 7) % 5);
			const int height = 30 + step * 50;
			Put(img, x, y, static_cast<int>((nx * 0.5f + 0.5f) * 255.0f),
				static_cast<int>((ny * 0.5f + 0.5f) * 255.0f),
				static_cast<int>((nz * 0.5f + 0.5f) * 255.0f), height);
		}
	return img;
}

// Constant RGB under a sweeping alpha ramp: all the information is in the
// channel mode 1 cannot carry and mode 6 has to share.
assets::ImageData MakeAlphaRamp(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) {
			const int a = static_cast<int>(static_cast<float>(x) /
										   static_cast<float>(w - 1) * 255.0f);
			Put(img, x, y, 180, 170, 160, a);
		}
	return img;
}

// Sharp colour edges AND a sharp alpha cutout at once: a block can need two
// colour lines and a hard alpha step in the same 16 pixels, which is where the
// modes actively disagree about what matters.
assets::ImageData MakeCutout(u32 w, u32 h) {
	assets::ImageData img = MakeImage(w, h);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) {
			const bool leaf = ((x / 11) + (y / 13)) % 3 != 0;
			const bool dark = (x / 5 + y / 5) % 2 == 0;
			Put(img, x, y, dark ? 40 : 150, dark ? 90 : 190, dark ? 30 : 70,
				leaf ? 255 : 0);
		}
	return img;
}

std::vector<Sample> SyntheticCorpus() {
	constexpr u32 kW = 128, kH = 128; // 32x32 blocks each
	std::vector<Sample> out;
	out.push_back({"syn.brick", MakeBrick(kW, kH)});
	out.push_back({"syn.gradient", MakeGradient(kW, kH)});
	out.push_back({"syn.noise", MakeNoise(kW, kH)});
	out.push_back({"syn.flat", MakeFlat(kW, kH)});
	out.push_back({"syn.normalheight", MakeNormalHeight(kW, kH)});
	out.push_back({"syn.alpharamp", MakeAlphaRamp(kW, kH)});
	out.push_back({"syn.cutout", MakeCutout(kW, kH)});
	return out;
}

// ---- Real textures ----------------------------------------------------------

// Centre crop to at most `maxDim`, aligned down to a block boundary. Cropping
// rather than scaling: a resampled texture is a different image, and the point
// of including real content is to measure the blocks that actually ship.
assets::ImageData CropCentre(const assets::ImageData& src, u32 maxDim) {
	const u32 w = std::min(src.width, maxDim) & ~3u;
	const u32 h = std::min(src.height, maxDim) & ~3u;
	assets::ImageData dst = MakeImage(w, h);
	const u32 ox = (src.width - w) / 2, oy = (src.height - h) / 2;
	for (u32 y = 0; y < h; ++y)
		std::memcpy(&dst.pixels[static_cast<size_t>(y) * w * 4],
					&src.pixels[(static_cast<size_t>(y + oy) * src.width + ox) * 4],
					static_cast<size_t>(w) * 4);
	return dst;
}

// Picks a spread of installed textures: albedo, normal+height and ORM maps are
// three different kinds of content, and the normal maps are precisely the ones
// mode 5 was added for, so a sample that happened to miss them would measure
// the wrong thing.
std::vector<Sample> RealCorpus(const std::string& assetsDir, int perKind, u32 maxDim) {
	const std::filesystem::path dir = std::filesystem::path(assetsDir) / "textures";
	std::vector<std::string> albedo, normals, orm;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
		if (!e.is_regular_file() || e.path().extension() != ".png") continue;
		const std::string stem = e.path().stem().string();
		if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "_n") == 0)
			normals.push_back(e.path().string());
		else if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "_mr") == 0)
			orm.push_back(e.path().string());
		else
			albedo.push_back(e.path().string());
	}
	if (ec) {
		std::printf("NOTE no textures at %s — synthetic corpus only\n",
					dir.string().c_str());
		return {};
	}

	std::vector<Sample> out;
	for (auto* list : {&albedo, &normals, &orm}) {
		std::sort(list->begin(), list->end()); // determinism across machines
		const int n = std::min<int>(perKind, static_cast<int>(list->size()));
		for (int i = 0; i < n; ++i) {
			// Evenly spaced through the sorted set rather than the first few,
			// which would all be the same material family.
			const size_t k = list->size() * static_cast<size_t>(i) / std::max(1, n);
			auto img = assets::LoadImageFile((*list)[k]);
			if (!img) continue;
			if (img->width < 8 || img->height < 8) continue;
			out.push_back({std::filesystem::path((*list)[k]).stem().string(),
						   CropCentre(*img, maxDim)});
		}
	}
	return out;
}

// ---- Measurement ------------------------------------------------------------

struct Result {
	double psnr = 0;
	double encodeMs = 0;
	size_t blocks = 0;
	size_t badBlocks = 0;    // encoder's error estimate != a real decode's
	bool threadMismatch = false;
	std::map<int, size_t> modeCount;
};

double Psnr(double mse) {
	return mse <= 0.0 ? 99.99 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// The per-block truth: decode the packed bytes and square the differences
// against the source, exactly as the encoder claims to have done.
float BlockError(const assets::ImageData& src, const assets::ImageData& dec, u32 bx,
				 u32 by) {
	float err = 0;
	for (u32 j = 0; j < 4; ++j)
		for (u32 i = 0; i < 4; ++i) {
			const size_t o =
				(static_cast<size_t>(by * 4 + j) * src.width + bx * 4 + i) * 4;
			for (int c = 0; c < 4; ++c) {
				const float d = static_cast<float>(dec.pixels[o + c]) -
								static_cast<float>(src.pixels[o + c]);
				err += d * d;
			}
		}
	return err;
}

Result Measure(const Sample& s, const baker::Bc7Options& opt, bool selfTest,
			   bool checkThreads) {
	Result r;
	std::vector<baker::Bc7BlockStat> stats;

	const auto t0 = std::chrono::steady_clock::now();
	std::vector<u8> enc = baker::EncodeBc7(s.image, opt, &stats);
	const auto t1 = std::chrono::steady_clock::now();
	r.encodeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

	if (checkThreads) {
		baker::Bc7Options serial = opt;
		serial.threads = 1;
		r.threadMismatch = (baker::EncodeBc7(s.image, serial) != enc);
	}

	// --self-test damages the packed bytes AFTER encoding. Nothing else changes,
	// so a harness that still says PASS is not reading the bytes at all.
	if (selfTest)
		for (size_t b = 0; b < stats.size(); b += 97) enc[b * 16 + 15] ^= 0x40;

	const assets::ImageData dec =
		bc7test::DecodeBc7(enc.data(), s.image.width, s.image.height);

	const u32 blocksX = (s.image.width + 3) / 4;
	const u32 blocksY = (s.image.height + 3) / 4;
	r.blocks = stats.size();
	for (u32 by = 0; by < blocksY; ++by)
		for (u32 bx = 0; bx < blocksX; ++bx) {
			const size_t b = static_cast<size_t>(by) * blocksX + bx;
			++r.modeCount[static_cast<int>(stats[b].mode)];
			if (BlockError(s.image, dec, bx, by) != stats[b].error) ++r.badBlocks;
		}

	double sum = 0;
	for (size_t i = 0; i < s.image.pixels.size(); ++i) {
		const double d = static_cast<double>(dec.pixels[i]) -
						 static_cast<double>(s.image.pixels[i]);
		sum += d * d;
	}
	r.psnr = Psnr(sum / static_cast<double>(s.image.pixels.size()));
	return r;
}

// ---- Baseline ---------------------------------------------------------------

std::map<std::string, double> LoadBaseline(const std::string& path) {
	std::map<std::string, double> out;
	std::ifstream in(path);
	std::string name;
	double psnr;
	while (in >> name >> psnr) {
		if (name.empty() || name[0] == '#') continue;
		out[name] = psnr;
	}
	return out;
}

// ---- The audit --------------------------------------------------------------

// Phase 2's question, asked properly: what is each knob actually worth? Runs the
// same corpus under several configurations and prints one table. The prescore
// row is the interesting one — it compares the shipped top-8 partition search
// against evaluating all 64, per block, which is the only honest way to know
// whether the heuristic ever misses.
void RunAudit(const std::vector<Sample>& corpus) {
	struct Config {
		const char* label;
		baker::Bc7Options opt;
	};
	std::vector<Config> configs;
	auto add = [&](const char* label, u32 modes, int shapes, bool ptrial,
				   unsigned threads = 0) {
		baker::Bc7Options o;
		o.modes = modes;
		o.shapeTrials = shapes;
		o.trialPBits = ptrial;
		o.threads = threads;
		configs.push_back({label, o});
	};
	using namespace dungeon::baker;
	add("mode6 only (the original)", kBc7Mode6, 8, true);
	add("modes 1+6 (what shipped)", kBc7Mode1 | kBc7Mode6, 8, true);
	add("modes 1+5+6, p-trial", kBc7AllModes, 8, true);
	add("  shapes=4", kBc7AllModes, 4, true);
	add("  shapes=16", kBc7AllModes, 16, true);
	add("  shapes=64 (exhaustive)", kBc7AllModes, 64, true);
	add("p-bit PROXY, shapes=8", kBc7AllModes, 8, false);
	add("  shapes=16 (the default)", kBc7AllModes, 16, false);
	add("  shapes=64", kBc7AllModes, 64, false);
	add("the default, ONE thread", kBc7AllModes, 16, true, 1);

	// AGGREGATE AS THE MEAN OF PER-IMAGE PSNR, one vote each. Pooling the squared
	// error instead would be arithmetically tidier and completely misleading: an
	// incompressible image sits ~1000x higher in MSE than a smooth one, so a
	// pooled number is a report on the worst image in the corpus and says almost
	// nothing about the rest. The per-image WORST column is what catches a knob
	// that helps on average while hurting one kind of content.
	std::printf("\n%-28s %9s %9s %9s %8s\n", "configuration", "mean PSNR", "d(mean)",
				"worst img", "ms");
	std::printf("%s\n", std::string(68, '-').c_str());

	std::vector<double> refPsnr; // per-image, from the first configuration
	double base = 0;
	for (size_t ci = 0; ci < configs.size(); ++ci) {
		std::vector<double> psnrs;
		double ms = 0;
		for (const auto& s : corpus) {
			const Result r = Measure(s, configs[ci].opt, false, false);
			psnrs.push_back(r.psnr);
			ms += r.encodeMs;
		}
		double mean = 0;
		for (double p : psnrs) mean += p;
		mean /= static_cast<double>(psnrs.size());

		double worst = 0;
		if (ci == 0) {
			refPsnr = psnrs;
			base = mean;
		} else {
			worst = 1e9;
			for (size_t i = 0; i < psnrs.size(); ++i)
				worst = std::min(worst, psnrs[i] - refPsnr[i]);
		}

		std::printf("%-28s %9.2f %+9.2f %+9.2f %8.0f\n", configs[ci].label, mean,
					mean - base, worst, ms);
	}

	// The p-bit trial is the one knob whose aggregate verdict is small enough to
	// be an artefact, so it gets its own per-image column rather than a summary.
	std::printf("\np-bit trial vs the cheap proxy, per image (shapes=16):\n");
	baker::Bc7Options withTrial, proxy;
	withTrial.shapeTrials = proxy.shapeTrials = 16;
	withTrial.trialPBits = true;
	proxy.trialPBits = false;
	for (const auto& s : corpus) {
		const double a = Measure(s, withTrial, false, false).psnr;
		const double b = Measure(s, proxy, false, false).psnr;
		std::printf("  %-26s trial %6.2f   proxy %6.2f   %+5.2f dB\n", s.name.c_str(),
					a, b, b - a);
	}

	// Per-block prescore audit: does the top-8 shortlist ever miss a shape that
	// exhaustive search would have taken?
	baker::Bc7Options top8, all64;
	top8.shapeTrials = 8;
	all64.shapeTrials = 64;
	size_t missed = 0, total = 0;
	for (const auto& s : corpus) {
		std::vector<baker::Bc7BlockStat> a, b;
		baker::EncodeBc7(s.image, top8, &a);
		baker::EncodeBc7(s.image, all64, &b);
		for (size_t i = 0; i < a.size(); ++i) {
			++total;
			if (b[i].error < a[i].error) ++missed;
		}
	}
	std::printf("\nprescore: exhaustive beat the top-8 shortlist on %zu of %zu blocks"
				" (%.3f%%)\n",
				missed, total,
				total ? 100.0 * static_cast<double>(missed) / static_cast<double>(total)
					  : 0.0);
}

void Usage() {
	std::printf(
		"usage: Bc7Test [options] [image.png ...]\n"
		"  --assets <dir>        add real textures from <dir>\\textures\n"
		"  --per-kind N          textures sampled per kind (default 3)\n"
		"  --max-dim N           centre-crop real textures to N px (default 256)\n"
		"  --modes 1,5,6         modes to trial\n"
		"  --shape-trials N      mode 1 partition shapes evaluated (1..64)\n"
		"  --no-ptrial           use the cheap p-bit proxy\n"
		"  --threads N           encoder threads (0 = auto)\n"
		"  --baseline <file>     fail if PSNR regresses against <file>\n"
		"  --write-baseline <f>  record the current PSNR as the baseline\n"
		"  --audit               print the knob-by-knob measurement table\n"
		"  --self-test           corrupt encoded bytes; the run MUST fail\n");
}

} // namespace

int main(int argc, char** argv) {
	baker::Bc7Options opt;
	std::string assetsDir, baselinePath, writeBaselinePath;
	std::vector<std::string> explicitImages;
	int perKind = 3;
	u32 maxDim = 256;
	bool audit = false, selfTest = false;

	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--assets" && i + 1 < argc) assetsDir = argv[++i];
		else if (a == "--per-kind" && i + 1 < argc) perKind = std::atoi(argv[++i]);
		else if (a == "--max-dim" && i + 1 < argc)
			maxDim = static_cast<u32>(std::atoi(argv[++i]));
		else if (a == "--shape-trials" && i + 1 < argc)
			opt.shapeTrials = std::atoi(argv[++i]);
		else if (a == "--threads" && i + 1 < argc)
			opt.threads = static_cast<unsigned>(std::atoi(argv[++i]));
		else if (a == "--no-ptrial") opt.trialPBits = false;
		else if (a == "--baseline" && i + 1 < argc) baselinePath = argv[++i];
		else if (a == "--write-baseline" && i + 1 < argc) writeBaselinePath = argv[++i];
		else if (a == "--audit") audit = true;
		else if (a == "--self-test") selfTest = true;
		else if (a == "--modes" && i + 1 < argc) {
			opt.modes = 0;
			for (const char* p = argv[++i]; *p; ++p) {
				if (*p == '1') opt.modes |= baker::kBc7Mode1;
				else if (*p == '5') opt.modes |= baker::kBc7Mode5;
				else if (*p == '6') opt.modes |= baker::kBc7Mode6;
			}
		} else if (a == "--help" || a == "-h") {
			Usage();
			return 0;
		} else if (a.rfind("--", 0) == 0) {
			std::printf("unknown option %s\n", a.c_str());
			Usage();
			return 2;
		} else {
			explicitImages.push_back(a);
		}
	}

	std::vector<Sample> corpus = SyntheticCorpus();
	if (!assetsDir.empty()) {
		auto real = RealCorpus(assetsDir, perKind, maxDim);
		corpus.insert(corpus.end(), std::make_move_iterator(real.begin()),
					  std::make_move_iterator(real.end()));
	}
	for (const auto& path : explicitImages) {
		auto img = assets::LoadImageFile(path);
		if (!img) {
			std::printf("FAILED to load %s\n", path.c_str());
			return 2;
		}
		corpus.push_back(
			{std::filesystem::path(path).stem().string(), CropCentre(*img, maxDim)});
	}

	if (audit) {
		RunAudit(corpus);
		return 0;
	}

	const auto baseline = baselinePath.empty() ? std::map<std::string, double>{}
											   : LoadBaseline(baselinePath);

	std::printf("%-26s %10s %7s %6s %6s %s\n", "image", "size", "PSNR", "bad", "thr",
				"mode mix");
	std::printf("%s\n", std::string(86, '-').c_str());

	size_t badTotal = 0, threadFails = 0, baselineFails = 0;
	double minPsnr = 1e9;
	std::vector<std::pair<std::string, double>> recorded;

	for (const auto& s : corpus) {
		const Result r = Measure(s, opt, selfTest, true);
		badTotal += r.badBlocks;
		threadFails += r.threadMismatch ? 1 : 0;
		minPsnr = std::min(minPsnr, r.psnr);
		recorded.emplace_back(s.name, r.psnr);

		std::string mix;
		for (const auto& [mode, count] : r.modeCount) {
			char buf[48];
			std::snprintf(buf, sizeof buf, "m%d:%.0f%% ", mode,
						  100.0 * static_cast<double>(count) /
							  static_cast<double>(r.blocks));
			mix += buf;
		}

		char size[32];
		std::snprintf(size, sizeof size, "%ux%u", s.image.width, s.image.height);
		std::printf("%-26s %10s %7.2f %6zu %6s %s", s.name.c_str(), size, r.psnr,
					r.badBlocks, r.threadMismatch ? "DIFF" : "ok", mix.c_str());

		const auto it = baseline.find(s.name);
		if (it != baseline.end() && r.psnr < it->second - 0.01) {
			std::printf(" REGRESSED (was %.2f)", it->second);
			++baselineFails;
		}
		std::printf("\n");
	}

	if (!writeBaselinePath.empty()) {
		std::ofstream out(writeBaselinePath);
		out << "# BC7 quality baseline - PSNR in dB per corpus image.\n"
			   "# Regenerate with: Bc7Test --assets <dir> --write-baseline <this "
			   "file>\n"
			   "# A drop here is a regression; a rise is an improvement worth a "
			   "commit message.\n";
		for (const auto& [name, psnr] : recorded) {
			char buf[32];
			std::snprintf(buf, sizeof buf, "%.2f", psnr);
			out << name << ' ' << buf << '\n';
		}
		std::printf("\nwrote baseline %s (%zu images)\n", writeBaselinePath.c_str(),
					recorded.size());
	}

	const bool pass = badTotal == 0 && threadFails == 0 && baselineFails == 0;
	std::printf("\nBC7TEST VERDICT=%s images=%zu consistency_bad=%zu thread_diff=%zu "
				"regressed=%zu minpsnr=%.2f\n",
				pass ? "PASS" : "FAIL", corpus.size(), badTotal, threadFails,
				baselineFails, minPsnr);

	if (selfTest) {
		// The checker checking itself: with the bytes corrupted, a PASS means the
		// consistency test is not actually reading them.
		const bool caught = !pass;
		std::printf("BC7TEST SELFTEST=%s (corruption %s)\n", caught ? "PASS" : "FAIL",
					caught ? "detected" : "MISSED");
		return caught ? 0 : 1;
	}
	return pass ? 0 : 1;
}
