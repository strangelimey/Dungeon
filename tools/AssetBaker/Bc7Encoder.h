#pragma once

#include "Assets/Image.h"
#include "Core/Types.h"

#include <vector>

namespace dungeon::baker {

// Modes trialled per block, as a bitmask. The shipping default is all of them;
// the mask exists so tools/Bc7Test can measure what each mode is worth.
enum Bc7Mode : u32 {
	kBc7Mode1 = 1u << 1, // 2 subsets, RGB 6.6.6 + shared p, 3-bit indices
	kBc7Mode3 = 1u << 3, // 2 subsets, RGB 7.7.7 + per-endpoint p, 2-bit indices
	kBc7Mode5 = 1u << 5, // 1 subset, RGB 7.7.7 + SEPARATE alpha 8.8, 2-bit
	kBc7Mode6 = 1u << 6, // 1 subset, RGBA 7.7.7.7 + per-endpoint p, 4-bit
	kBc7AllModes = kBc7Mode1 | kBc7Mode3 | kBc7Mode5 | kBc7Mode6,
};

// How the two-subset modes shortlist the 64 partition shapes before fully
// solving the best few. Both are cheap per-shape scores; they differ in what
// they think "a good partition" means.
enum class Bc7Prescore {
	// Sum of each subset's per-channel bounding-box extent. Cheap, and blind to
	// how many pixels are in each subset — a 15/1 split scores well trivially,
	// because the lone pixel's box has zero extent.
	BoundingBox,
	// Total WITHIN-SUBSET SCATTER: sum over subsets of n * variance, per
	// channel. This is the k-means objective, and a two-subset BC7 fit is
	// essentially constrained 2-means with a line per cluster, so it is scoring
	// the thing the solve actually optimises. Population falls out of it for
	// free: a 15/1 split leaves all the scatter in the big subset.
	Scatter,
};

struct Bc7Options {
	u32 modes = kBc7AllModes;

	// Which per-shape score ranks the partition shortlist. See Bc7Prescore.
	Bc7Prescore prescore = Bc7Prescore::Scatter;

	// Partition shapes fully evaluated by the two-subset modes, best-first by
	// the prescore (1..64). 64 is exhaustive.
	//
	// 8, and the history of this number is the point. It was raised to 16 when
	// modes 1/5/6 were the whole encoder and the step was worth +0.22 dB. Adding
	// mode 3 and mode 5's rotations moved the knee back: the same 8 -> 16 step is
	// now worth +0.07 dB for +65% encode time, because a block the shortlist
	// mis-partitions usually has another MODE that suits it, and the extra modes
	// got there first. A wider search and a richer mode set buy overlapping
	// things, so this is worth re-measuring whenever a mode is added rather than
	// ratcheting upward on the assumption that more search is always better.
	int shapeTrials = 8;

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

	// Trial all four of mode 5's channel ROTATIONS rather than only the identity.
	// A rotation swaps alpha with one of R/G/B before encoding, so the decoupled
	// endpoint pair serves that colour channel instead — worth it when the odd
	// channel out is a colour rather than alpha. Costs a 4x mode 5 solve.
	bool trialRotations = true;

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

// MEASUREMENT ONLY — runs mode 7's search on one block and returns the error it
// would achieve, without packing anything. Nothing emits mode 7.
//
// Mode 7 is the only BC7 mode with two subsets AND alpha, so it is the only
// candidate that could help a block whose alpha varies: modes 1 and 3 force
// alpha opaque and are ineligible there, which leaves such a block with one
// colour line however good the encoder gets. Mode 7 is also the COARSEST
// two-subset mode — 5 colour bits plus a p-bit per endpoint across all four
// channels, and only four index positions — so whether that trade pays is an
// empirical question rather than an obvious yes. This exists so the question can
// be answered with a number before the packer and the harness decoder are
// written. `px` is 16 RGBA pixels in raster order.
// See `Bc7Test --headroom` and docs/bc7.md.
float EstimateMode7Error(const u8 px[16][4], const Bc7Options& opt);

} // namespace dungeon::baker
