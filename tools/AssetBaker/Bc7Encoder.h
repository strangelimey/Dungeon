#pragma once

#include "Assets/Image.h"
#include "Core/Types.h"

#include <vector>

namespace dungeon::baker {

// Modes trialled per block, as a bitmask. The shipping default is all of them;
// the mask exists so tools/Bc7Test can measure what each mode is worth.
enum Bc7Mode : u32 {
	kBc7Mode1 = 1u << 1, // 2 subsets, RGB 6.6.6 + shared p, 3-bit indices
	kBc7Mode5 = 1u << 5, // 1 subset, RGB 7.7.7 + SEPARATE alpha 8.8, 2-bit
	kBc7Mode6 = 1u << 6, // 1 subset, RGBA 7.7.7.7 + per-endpoint p, 4-bit
	kBc7AllModes = kBc7Mode1 | kBc7Mode5 | kBc7Mode6,
};

struct Bc7Options {
	u32 modes = kBc7AllModes;

	// Mode 1 partition shapes fully evaluated, best-first by the bounding-box
	// prescore (1..64). 64 is exhaustive.
	//
	// MEASURED (Bc7Test --audit, 16-image corpus): the prescore shortlist is
	// genuinely lossy — exhaustive search finds a better shape on ~31% of
	// blocks. But the shortlist is cheap to WIDEN and expensive to perfect:
	// 8 -> 16 buys +0.22 dB mean for +66% encode time, and 16 -> 64 buys a
	// further +0.17 dB for another +200%. 16 is the knee, so it is the default.
	int shapeTrials = 16;

	// Carry EVERY p-bit choice through to the final block error instead of
	// picking the one with the lower endpoint-quantization error.
	//
	// ON, and the margin is a lesson in how the corpus is summarised. Averaged
	// as pooled squared error this knob looks worth +0.01 dB and not worth its
	// +27% cost — but pooled error is dominated by whichever image compresses
	// worst, so that number is a report on the noise tile. Per image, dropping
	// the trial costs 1.35 dB on the brick tile and 0.16-0.27 dB on the real
	// scanned stone: precisely the content this dungeon is built out of. The
	// endpoint-quantization proxy is systematically wrong exactly where two
	// materials meet, which is the case mode 1 exists to serve.
	bool trialPBits = true;

	// Worker threads for the block fan-out; 0 = hardware_concurrency. Output is
	// byte-identical at any thread count (blocks are independent) — Bc7Test
	// asserts that rather than assuming it.
	unsigned threads = 0;
};

// Per-block record of what the encoder chose and the error IT believes the
// result carries. That estimate is computed from the reconstructed endpoints
// and the hardware blend, never from the packed bytes, so it should equal a
// real decode's error EXACTLY — tools/Bc7Test asserts that equality, which is
// what makes "keep the lower-error mode" a trustworthy rule.
struct Bc7BlockStat {
	u32 mode = 0;
	float error = 0.0f; // sum of squared channel differences over 16 px x 4 ch
};

// Encodes an RGBA8 image as BC7 blocks, trialling several modes per block and
// keeping the lowest-error result:
//   * mode 6 — one RGBA colour line, the workhorse for photographic albedo.
//   * mode 1 — two subsets with their OWN colour line each, so a block
//     straddling two materials (brick/mortar) stops smearing one line through
//     the middle. RGB-only, so fully-opaque blocks only.
//   * mode 5 — RGB and ALPHA solved separately. The mode for normal+height
//     maps, where alpha carries height uncorrelated with the normal: mode 6
//     fits a single 4-D line through RGB and A together, so a height edge
//     drags the normal off its line.
// Returns ceil(w/4) * ceil(h/4) * 16 bytes. `stats`, if given, is filled with
// one entry per block in raster order.
std::vector<u8> EncodeBc7(const assets::ImageData& image, const Bc7Options& opt = {},
						  std::vector<Bc7BlockStat>* stats = nullptr);

} // namespace dungeon::baker
