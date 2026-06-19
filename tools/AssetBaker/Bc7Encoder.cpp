// ============================================================================
// Bc7Encoder.cpp — a compact BC7 encoder trialling two modes per block.
//
// Each 4x4 block is encoded two ways and the cheaper reconstruction wins:
//   * Mode 6 — one subset, RGBA 7.7.7.7 endpoints + per-endpoint p-bit, 4-bit
//     indices. The workhorse for photographic albedo and packed normal+height;
//     the ONLY trialled mode that carries alpha, so blocks whose alpha varies
//     (height-in-alpha normal maps) can only use it.
//   * Mode 1 — two subsets, RGB 6.6.6 endpoints + one shared p-bit per subset,
//     3-bit indices, chosen from BC7's 64 fixed partition shapes. Gives a block
//     straddling two materials (brick/mortar) its OWN colour line per region
//     instead of one line smeared through the middle. RGB-only: alpha is forced
//     opaque, so it is a candidate only for fully-opaque blocks.
//
// Per subset the solve is the same: principal axis of the pixels (power
// iteration on the covariance) -> extreme projections as endpoints -> iterate
// (quantize -> assign best indices -> least-squares refit). Mode 1 prescreens
// the 64 partitions by a cheap bounding-box score and fully evaluates the best
// few. The partition / anchor (fix-up) tables and the 3-bit interpolation
// weights are the BC7 hardware constants (values from Microsoft's DirectXTex,
// MIT-licensed) — the decoder indexes the same tables, so they must be exact.
// ============================================================================
#include "Bc7Encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dungeon::baker {

namespace {

// BC7 interpolation weights (out of 64): 4-bit (mode 6), 3-bit (mode 1).
constexpr int kWeights4[16] = {0,  4,  9,  13, 17, 21, 26, 30,
							   34, 38, 43, 47, 51, 55, 60, 64};
constexpr int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};

// BC7 2-subset partition shapes: shape -> per-pixel subset (0/1) for the 16
// pixels of the block. (DirectXTex g_aPartitionTable[1].)
constexpr u8 kPartition2[64][16] = {
	{0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
	{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
	{0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
	{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1},
	{0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
	{0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
	{0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
	{0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1},
	{0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
	{0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
	{0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
	{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
	{0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
	{0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
	{0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
	{0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
	{0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
	{0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
	{0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0},
	{0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
	{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
	{0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
	{0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0},
	{0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
	{0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
	{0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
	{0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1},
	{0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
	{0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0},
	{0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
	{0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0},
	{0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
	{0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
	{0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
	{0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1},
	{0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
	{0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0},
	{0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0},
	{0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
	{0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1},
	{0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
	{0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
	{0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
	{0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1},
	{0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
	{0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1},
	{0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
	{0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
	{0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
	{0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0},
	{0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
};

// Second-subset anchor (fix-up) pixel per shape; subset 0's anchor is always
// pixel 0. (DirectXTex g_aFixUp[1], middle column.)
constexpr u8 kAnchor2[64] = {
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
	15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
	6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};

// Number of partition shapes fully evaluated (best by bounding-box prescore).
constexpr int kShapeTrials = 8;

struct Vec4f {
	float v[4];
	float& operator[](int i) { return v[i]; }
	float operator[](int i) const { return v[i]; }
};

float Dot(const Vec4f& a, const Vec4f& b, int nch) {
	float s = 0;
	for (int c = 0; c < nch; ++c) s += a[c] * b[c];
	return s;
}

// BC7 bit replication: expand a `bits`-bit endpoint component to 8 bits the way
// the hardware does. bits==8 (mode 6 endpoint = 7 colour bits + p-bit) is a
// no-op; bits==7 (mode 1 = 6 + shared p) replicates the top bit down.
int Expand(int value, int bits) {
	if (bits >= 8) return value & 0xff;
	return ((value << (8 - bits)) | (value >> (2 * bits - 8))) & 0xff;
}

// Quantizes one float channel (0..255) to `cb` colour bits given a fixed p-bit,
// returning the cb-bit code and the 8-bit reconstruction. Mirrors the hardware:
// the stored value is (code<<1)|p over cb+1 bits, then bit-expanded to 8.
struct Quant {
	int code;
	float recon;
};
Quant QuantizeChannel(float value, int cb, int pBit) {
	const int maxCombined = (1 << (cb + 1)) - 1;
	const float target = value / 255.0f * static_cast<float>(maxCombined);
	const int code = std::clamp(
		static_cast<int>(std::lround((target - pBit) * 0.5f)), 0, (1 << cb) - 1);
	const int combined = (code << 1) | pBit;
	return {code, static_cast<float>(Expand(combined, cb + 1))};
}

// Reconstructed colour at interpolation step `weight` between two 8-bit
// endpoints (the hardware's rounded blend).
float Lerp8(float a, float b, int weight) {
	return std::floor((a * (64 - weight) + b * weight + 32.0f) / 64.0f);
}

// Best index for one pixel against a reconstructed endpoint segment, plus the
// squared error it incurs (over nch channels).
int BestIndex(const Vec4f& px, const Vec4f& r0, const Vec4f& r1, int nch,
			  const int* weights, int wcount, float& errOut) {
	Vec4f seg;
	for (int c = 0; c < nch; ++c) seg[c] = r1[c] - r0[c];
	const float len2 = Dot(seg, seg, nch);
	float tScaled = 0;
	if (len2 >= 1e-6f) {
		Vec4f d;
		for (int c = 0; c < nch; ++c) d[c] = px[c] - r0[c];
		tScaled = std::clamp(Dot(d, seg, nch) / len2, 0.0f, 1.0f) * 64.0f;
	}
	int best = 0;
	float bestDist = 1e30f;
	for (int w = 0; w < wcount; ++w) {
		const float dist = std::fabs(tScaled - static_cast<float>(weights[w]));
		if (dist < bestDist) {
			bestDist = dist;
			best = w;
		}
	}
	float err = 0;
	for (int c = 0; c < nch; ++c) {
		const float d = Lerp8(r0[c], r1[c], weights[best]) - px[c];
		err += d * d;
	}
	errOut = err;
	return best;
}

// Principal-axis float endpoints for a set of member pixels over nch channels.
void FitFloatEndpoints(const Vec4f px[16], const int* mem, int n, int nch,
					   Vec4f& e0, Vec4f& e1) {
	Vec4f mean{};
	for (int i = 0; i < n; ++i)
		for (int c = 0; c < nch; ++c) mean[c] += px[mem[i]][c];
	for (int c = 0; c < nch; ++c) mean[c] /= static_cast<float>(n);

	float cov[4][4] = {};
	for (int i = 0; i < n; ++i)
		for (int r = 0; r < nch; ++r)
			for (int c = 0; c < nch; ++c)
				cov[r][c] += (px[mem[i]][r] - mean[r]) * (px[mem[i]][c] - mean[c]);

	Vec4f axis{};
	for (int c = 0; c < nch; ++c) axis[c] = 1;
	for (int iter = 0; iter < 8; ++iter) {
		Vec4f next{};
		for (int r = 0; r < nch; ++r)
			for (int c = 0; c < nch; ++c) next[r] += cov[r][c] * axis[c];
		const float len = std::sqrt(Dot(next, next, nch));
		if (len < 1e-6f) break; // flat subset — axis direction is irrelevant
		for (int c = 0; c < nch; ++c) axis[c] = next[c] / len;
	}

	float tMin = 1e30f, tMax = -1e30f;
	for (int i = 0; i < n; ++i) {
		Vec4f d;
		for (int c = 0; c < nch; ++c) d[c] = px[mem[i]][c] - mean[c];
		const float t = Dot(d, axis, nch);
		tMin = std::min(tMin, t);
		tMax = std::max(tMax, t);
	}
	for (int c = 0; c < nch; ++c) {
		e0[c] = std::clamp(mean[c] + axis[c] * tMin, 0.0f, 255.0f);
		e1[c] = std::clamp(mean[c] + axis[c] * tMax, 0.0f, 255.0f);
	}
}

// Refits float endpoints from fixed per-member indices (per-channel 2x2 least
// squares), over nch channels.
void LeastSquaresFit(const Vec4f px[16], const int* mem, int n, int nch,
					 const int* weights, const int* idx, Vec4f& e0, Vec4f& e1) {
	float a00 = 0, a01 = 0, a11 = 0;
	float b0[4] = {}, b1[4] = {};
	for (int i = 0; i < n; ++i) {
		const float w = weights[idx[i]] / 64.0f;
		const float iw = 1.0f - w;
		a00 += iw * iw;
		a01 += iw * w;
		a11 += w * w;
		for (int c = 0; c < nch; ++c) {
			b0[c] += iw * px[mem[i]][c];
			b1[c] += w * px[mem[i]][c];
		}
	}
	const float det = a00 * a11 - a01 * a01;
	if (std::fabs(det) < 1e-4f) return; // degenerate (all one index) — keep
	const float inv = 1.0f / det;
	for (int c = 0; c < nch; ++c) {
		e0[c] = std::clamp((a11 * b0[c] - a01 * b1[c]) * inv, 0.0f, 255.0f);
		e1[c] = std::clamp((a00 * b1[c] - a01 * b0[c]) * inv, 0.0f, 255.0f);
	}
}

// LSB-first bit packing into the 16-byte block.
struct BitWriter {
	u8* out;
	int bit = 0;
	void Write(u32 value, int count) {
		for (int i = 0; i < count; ++i) {
			if ((value >> i) & 1) out[bit >> 3] |= static_cast<u8>(1 << (bit & 7));
			++bit;
		}
	}
};

// ---- Mode 6: one subset, RGBA, 7-bit endpoints + p-bit, 4-bit indices -------

// Per-endpoint p-bit quantize (mode 6: cb=7, the p-bit is per endpoint).
void QuantizeEndpoint6(const Vec4f& e, int q7[4], int& pBit, Vec4f& recon) {
	float bestErr = 1e30f;
	for (int p = 0; p <= 1; ++p) {
		int q[4];
		Vec4f r;
		float err = 0;
		for (int c = 0; c < 4; ++c) {
			const Quant qt = QuantizeChannel(e[c], 7, p);
			q[c] = qt.code;
			r[c] = qt.recon;
			const float d = r[c] - e[c];
			err += d * d;
		}
		if (err < bestErr) {
			bestErr = err;
			pBit = p;
			std::memcpy(q7, q, sizeof(q));
			recon = r;
		}
	}
}

float EncodeMode6(const Vec4f px[16], u8 out[16]) {
	static const int kAll[16] = {0, 1, 2, 3, 4,  5,  6,  7,
								 8, 9, 10, 11, 12, 13, 14, 15};
	Vec4f e0, e1;
	FitFloatEndpoints(px, kAll, 16, 4, e0, e1);

	int q0[4], q1[4], p0 = 0, p1 = 0, idx[16];
	Vec4f r0, r1;
	float err = 0;
	for (int iter = 0; iter < 3; ++iter) {
		QuantizeEndpoint6(e0, q0, p0, r0);
		QuantizeEndpoint6(e1, q1, p1, r1);
		err = 0;
		for (int i = 0; i < 16; ++i) {
			float e;
			idx[i] = BestIndex(px[i], r0, r1, 4, kWeights4, 16, e);
			err += e;
		}
		if (iter < 2) LeastSquaresFit(px, kAll, 16, 4, kWeights4, idx, e0, e1);
	}

	// Anchor constraint: index 0's MSB must be 0; swap endpoints if not.
	if (idx[0] & 8) {
		std::swap(q0, q1);
		std::swap(p0, p1);
		for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
	}

	std::memset(out, 0, 16);
	BitWriter bits{out};
	bits.Write(0x40, 7); // mode 6 (six zeros then a one, LSB first)
	for (int c = 0; c < 4; ++c) {
		bits.Write(static_cast<u32>(q0[c]), 7);
		bits.Write(static_cast<u32>(q1[c]), 7);
	}
	bits.Write(static_cast<u32>(p0), 1);
	bits.Write(static_cast<u32>(p1), 1);
	bits.Write(static_cast<u32>(idx[0]), 3); // anchor: implicit 0 MSB
	for (int i = 1; i < 16; ++i) bits.Write(static_cast<u32>(idx[i]), 4);
	return err;
}

// ---- Mode 1: two subsets, RGB, 6-bit endpoints + shared p-bit, 3-bit idx ----

// One solved subset: 6-bit codes per endpoint, the shared p-bit, the chosen
// per-member indices, and the subset's squared error.
struct SubsetSolve {
	int q[2][3]; // [endpoint][channel]
	int pBit;
	int idx[16]; // by member position
	float error;
};

// Float fit -> quantize (best shared p) -> index -> refit, twice (RGB).
SubsetSolve SolveSubsetMode1(const Vec4f px[16], const int* mem, int n) {
	Vec4f e0, e1;
	FitFloatEndpoints(px, mem, n, 3, e0, e1);

	SubsetSolve best{};
	for (int iter = 0; iter < 3; ++iter) {
		// Pick the shared p-bit minimising both endpoints' quantization error.
		int bestQ[2][3], bestP = 0;
		Vec4f bestR0{}, bestR1{};
		float bestQErr = 1e30f;
		for (int p = 0; p <= 1; ++p) {
			int q[2][3];
			Vec4f r0, r1;
			float qErr = 0;
			for (int c = 0; c < 3; ++c) {
				const Quant a = QuantizeChannel(e0[c], 6, p);
				const Quant b = QuantizeChannel(e1[c], 6, p);
				q[0][c] = a.code;
				q[1][c] = b.code;
				r0[c] = a.recon;
				r1[c] = b.recon;
				qErr += (a.recon - e0[c]) * (a.recon - e0[c]);
				qErr += (b.recon - e1[c]) * (b.recon - e1[c]);
			}
			if (qErr < bestQErr) {
				bestQErr = qErr;
				bestP = p;
				std::memcpy(bestQ, q, sizeof(q));
				bestR0 = r0;
				bestR1 = r1;
			}
		}

		int idx[16];
		float err = 0;
		for (int i = 0; i < n; ++i) {
			float e;
			idx[i] = BestIndex(px[mem[i]], bestR0, bestR1, 3, kWeights3, 8, e);
			err += e;
		}

		std::memcpy(best.q, bestQ, sizeof(bestQ));
		best.pBit = bestP;
		std::memcpy(best.idx, idx, sizeof(int) * n);
		best.error = err;

		if (iter < 2) LeastSquaresFit(px, mem, n, 3, kWeights3, idx, e0, e1);
	}
	return best;
}

// Cheap per-shape score: sum over subsets of the RGB bounding-box extent. Lower
// means the partition cleanly separates the block's colours.
float PrescoreShape(const Vec4f px[16], int shape) {
	float lo[2][3], hi[2][3];
	for (int s = 0; s < 2; ++s)
		for (int c = 0; c < 3; ++c) {
			lo[s][c] = 1e30f;
			hi[s][c] = -1e30f;
		}
	for (int i = 0; i < 16; ++i) {
		const int s = kPartition2[shape][i];
		for (int c = 0; c < 3; ++c) {
			lo[s][c] = std::min(lo[s][c], px[i][c]);
			hi[s][c] = std::max(hi[s][c], px[i][c]);
		}
	}
	float score = 0;
	for (int s = 0; s < 2; ++s)
		for (int c = 0; c < 3; ++c)
			if (hi[s][c] >= lo[s][c]) score += hi[s][c] - lo[s][c];
	return score;
}

// Encodes the best of kShapeTrials partition shapes; returns its error (or +inf
// if no shape was usable). `out` is written only on a finite result.
float EncodeMode1(const Vec4f px[16], u8 out[16]) {
	// Rank shapes by prescore, keep the cheapest few.
	std::array<int, 64> order;
	for (int s = 0; s < 64; ++s) order[s] = s;
	std::array<float, 64> score;
	for (int s = 0; s < 64; ++s) score[s] = PrescoreShape(px, s);
	std::partial_sort(order.begin(), order.begin() + kShapeTrials, order.end(),
					  [&](int a, int b) { return score[a] < score[b]; });

	float bestErr = 1e30f;
	int bestShape = -1;
	SubsetSolve bestS0{}, bestS1{};
	int memo0[16], memo1[16], n0 = 0, n1 = 0; // members of the best shape

	for (int t = 0; t < kShapeTrials; ++t) {
		const int shape = order[t];
		int m0[16], m1[16], c0 = 0, c1 = 0;
		for (int i = 0; i < 16; ++i)
			(kPartition2[shape][i] == 0 ? m0[c0++] : m1[c1++]) = i;
		if (c0 == 0 || c1 == 0) continue; // shapes always split, but be safe

		const SubsetSolve s0 = SolveSubsetMode1(px, m0, c0);
		const SubsetSolve s1 = SolveSubsetMode1(px, m1, c1);
		const float err = s0.error + s1.error;
		if (err < bestErr) {
			bestErr = err;
			bestShape = shape;
			bestS0 = s0;
			bestS1 = s1;
			std::memcpy(memo0, m0, sizeof(int) * c0);
			std::memcpy(memo1, m1, sizeof(int) * c1);
			n0 = c0;
			n1 = c1;
		}
	}
	if (bestShape < 0) return 1e30f;

	// Expand the per-member indices back to the 16-pixel raster order.
	int idx[16];
	for (int i = 0; i < n0; ++i) idx[memo0[i]] = bestS0.idx[i];
	for (int i = 0; i < n1; ++i) idx[memo1[i]] = bestS1.idx[i];

	// Per-subset anchor fix-up: the anchor pixel's index MSB must be 0; else
	// swap that subset's endpoints and invert its indices.
	const int anchor1 = kAnchor2[bestShape];
	SubsetSolve* solves[2] = {&bestS0, &bestS1};
	const int* mems[2] = {memo0, memo1};
	const int counts[2] = {n0, n1};
	const int anchors[2] = {0, anchor1};
	for (int s = 0; s < 2; ++s) {
		if (idx[anchors[s]] & 4) {
			std::swap(solves[s]->q[0], solves[s]->q[1]);
			for (int i = 0; i < counts[s]; ++i)
				idx[mems[s][i]] = 7 - idx[mems[s][i]];
		}
	}

	std::memset(out, 0, 16);
	BitWriter bits{out};
	bits.Write(0x2, 2); // mode 1 (one zero then a one, LSB first)
	bits.Write(static_cast<u32>(bestShape), 6);
	// Endpoints: per channel, in order s0e0, s0e1, s1e0, s1e1 (6 bits each).
	const int* qs[4] = {bestS0.q[0], bestS0.q[1], bestS1.q[0], bestS1.q[1]};
	for (int c = 0; c < 3; ++c)
		for (int e = 0; e < 4; ++e) bits.Write(static_cast<u32>(qs[e][c]), 6);
	bits.Write(static_cast<u32>(bestS0.pBit), 1);
	bits.Write(static_cast<u32>(bestS1.pBit), 1);
	// Indices (3 bits each; the two anchors implicitly drop their MSB).
	for (int i = 0; i < 16; ++i) {
		const bool anchor = (i == 0) || (i == anchor1);
		bits.Write(static_cast<u32>(idx[i]), anchor ? 2 : 3);
	}
	return bestErr;
}

void EncodeBlock(const Vec4f px[16], u8 out[16]) {
	const float err6 = EncodeMode6(px, out);

	// Modes 0-3 force alpha opaque, so mode 1 is eligible only when every
	// pixel is fully opaque (excludes height-in-alpha normal maps).
	bool opaque = true;
	for (int i = 0; i < 16 && opaque; ++i) opaque = (px[i][3] >= 255.0f);
	if (!opaque) return;

	u8 candidate[16];
	if (EncodeMode1(px, candidate) < err6) std::memcpy(out, candidate, 16);
}

} // namespace

std::vector<u8> EncodeBc7(const assets::ImageData& image) {
	const u32 blocksX = (image.width + 3) / 4;
	const u32 blocksY = (image.height + 3) / 4;
	std::vector<u8> out(static_cast<size_t>(blocksX) * blocksY * 16);

	for (u32 by = 0; by < blocksY; ++by) {
		for (u32 bx = 0; bx < blocksX; ++bx) {
			Vec4f px[16];
			for (u32 j = 0; j < 4; ++j) {
				for (u32 i = 0; i < 4; ++i) {
					// Clamp into the image so edge blocks replicate pixels.
					const u32 x = std::min(bx * 4 + i, image.width - 1);
					const u32 y = std::min(by * 4 + j, image.height - 1);
					const u8* p =
						&image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
					for (int c = 0; c < 4; ++c)
						px[j * 4 + i][c] = static_cast<float>(p[c]);
				}
			}
			EncodeBlock(px, &out[(static_cast<size_t>(by) * blocksX + bx) * 16]);
		}
	}
	return out;
}

} // namespace dungeon::baker
