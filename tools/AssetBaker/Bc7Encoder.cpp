// ============================================================================
// Bc7Encoder.cpp — a compact BC7 encoder trialling three modes per block.
//
// Each 4x4 block is encoded every eligible way and the cheapest reconstruction
// wins:
//   * Mode 6 — one subset, RGBA 7.7.7.7 endpoints + per-endpoint p-bit, 4-bit
//     indices. The workhorse for photographic albedo: the most index positions
//     (16) of any mode we emit, spent on a single 4-D colour line.
//   * Mode 1 — two subsets, RGB 6.6.6 endpoints + one shared p-bit per subset,
//     3-bit indices, chosen from BC7's 64 fixed partition shapes. Gives a block
//     straddling two materials (brick/mortar) its OWN colour line per region
//     instead of one line smeared through the middle. RGB-only: alpha is forced
//     opaque, so it is a candidate only for fully-opaque blocks.
//   * Mode 5 — one subset, RGB 7.7.7 endpoints and a SEPARATE alpha 8.8 pair,
//     each with its own 2-bit index set. The mode for this project's
//     normal+height maps: those pack height into alpha, uncorrelated with the
//     normal in RGB, and mode 6's single 4-D line cannot serve both — a height
//     edge drags the normal off the line and vice versa. Mode 5 decouples them,
//     paying 2-bit indices (4 steps) on each. Those blocks are also exactly the
//     ones mode 1 can never take, so before this they had a single option.
//
// Per subset the solve is the same: principal axis of the pixels (power
// iteration on the covariance) -> extreme projections as endpoints -> iterate
// (quantize -> assign best indices -> least-squares refit). Mode 1 prescreens
// the 64 partitions by a cheap bounding-box score and fully evaluates the best
// few. The partition / anchor tables and the interpolation weights are hardware
// constants shared with the test harness — see Bc7Tables.h.
//
// EVERY MODE'S ERROR IS COMPARABLE BY CONSTRUCTION: each is the sum of squared
// differences over the same 16 pixels x 4 channels (mode 1 contributes no alpha
// error because it only runs where alpha is already 255). That is what makes
// "keep the lower error" meaningful ACROSS modes — and each estimate is
// computed with the hardware blend on the reconstructed endpoints, so it equals
// a real decode's error to the bit. tools/Bc7Test asserts that per block;
// without it, adding a mode would be a leap of faith.
//
// Blocks are independent, so encoding is fanned out over block rows. The output
// is byte-identical at any thread count.
// ============================================================================
#include "Bc7Encoder.h"

#include "Bc7Tables.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace dungeon::baker {

namespace {

using bc7::Expand;
using bc7::kAnchor2;
using bc7::kPartition2;
using bc7::kWeights2;
using bc7::kWeights3;
using bc7::kWeights4;
using bc7::Lerp8;

// All 16 pixels, for the single-subset modes.
constexpr int kAllPixels[16] = {0, 1, 2,  3,  4,  5,  6,  7,
								8, 9, 10, 11, 12, 13, 14, 15};

struct Vec4f {
	float v[4]{};
	float& operator[](int i) { return v[i]; }
	float operator[](int i) const { return v[i]; }
};

float Dot(const Vec4f& a, const Vec4f& b, int nch) {
	float s = 0;
	for (int c = 0; c < nch; ++c) s += a[c] * b[c];
	return s;
}

// One quantized channel: the stored code and its 8-bit reconstruction.
struct Quant {
	int code;
	float recon;
};

// Quantizes one float channel (0..255) to `cb` colour bits given a fixed p-bit
// (modes 1 and 6). Mirrors the hardware: the stored value is (code<<1)|p over
// cb+1 bits, then bit-expanded to 8.
Quant QuantizeChannel(float value, int cb, int pBit) {
	const int maxCombined = (1 << (cb + 1)) - 1;
	const float target = value / 255.0f * static_cast<float>(maxCombined);
	const int code = std::clamp(
		static_cast<int>(std::lround((target - pBit) * 0.5f)), 0, (1 << cb) - 1);
	const int combined = (code << 1) | pBit;
	return {code, static_cast<float>(Expand(combined, cb + 1))};
}

// Quantizes one float channel to `bits` bits with NO p-bit (mode 5): the stored
// code IS the value, bit-expanded to 8. At bits==8 (mode 5's alpha) this is
// exact, which is the point of giving alpha its own endpoints.
Quant QuantizeChannelNoP(float value, int bits) {
	const int maxCode = (1 << bits) - 1;
	const int code = std::clamp(
		static_cast<int>(std::lround(value / 255.0f * static_cast<float>(maxCode))), 0,
		maxCode);
	return {code, static_cast<float>(Expand(code, bits))};
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
void FitFloatEndpoints(const Vec4f px[16], const int* mem, int n, int nch, Vec4f& e0,
					   Vec4f& e1) {
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

// Quantizes an RGBA endpoint at a GIVEN p-bit (cb=7), returning the
// endpoint-quantization error.
float QuantizeEndpoint6(const Vec4f& e, int pBit, int q[4], Vec4f& recon) {
	float err = 0;
	for (int c = 0; c < 4; ++c) {
		const Quant qt = QuantizeChannel(e[c], 7, pBit);
		q[c] = qt.code;
		recon[c] = qt.recon;
		const float d = recon[c] - e[c];
		err += d * d;
	}
	return err;
}

// The cheap p-bit choice: whichever reconstructs this endpoint more closely.
// Only a PROXY for the block error — Bc7Options::trialPBits skips it and
// carries every combination through to the real thing.
int ProxyPBit6(const Vec4f& e) {
	int q[4];
	Vec4f r;
	return QuantizeEndpoint6(e, 0, q, r) <= QuantizeEndpoint6(e, 1, q, r) ? 0 : 1;
}

float EncodeMode6(const Vec4f px[16], u8 out[16], const Bc7Options& opt) {
	Vec4f e0, e1;
	FitFloatEndpoints(px, kAllPixels, 16, 4, e0, e1);

	int q0[4]{}, q1[4]{}, p0 = 0, p1 = 0, idx[16]{};
	float err = 0;
	for (int iter = 0; iter < 3; ++iter) {
		// Candidate (p0,p1) pairs: every combination when trialling, else the
		// one each endpoint's own quantization error prefers.
		int pairs[4][2], npairs = 0;
		if (opt.trialPBits) {
			for (int a = 0; a <= 1; ++a)
				for (int b = 0; b <= 1; ++b) {
					pairs[npairs][0] = a;
					pairs[npairs][1] = b;
					++npairs;
				}
		} else {
			pairs[0][0] = ProxyPBit6(e0);
			pairs[0][1] = ProxyPBit6(e1);
			npairs = 1;
		}

		err = 1e30f;
		for (int k = 0; k < npairs; ++k) {
			int tq0[4], tq1[4], tidx[16];
			Vec4f r0, r1;
			QuantizeEndpoint6(e0, pairs[k][0], tq0, r0);
			QuantizeEndpoint6(e1, pairs[k][1], tq1, r1);
			float terr = 0;
			for (int i = 0; i < 16; ++i) {
				float e;
				tidx[i] = BestIndex(px[i], r0, r1, 4, kWeights4, 16, e);
				terr += e;
			}
			if (terr < err) {
				err = terr;
				p0 = pairs[k][0];
				p1 = pairs[k][1];
				std::memcpy(q0, tq0, sizeof(q0));
				std::memcpy(q1, tq1, sizeof(q1));
				std::memcpy(idx, tidx, sizeof(idx));
			}
		}

		if (iter < 2) LeastSquaresFit(px, kAllPixels, 16, 4, kWeights4, idx, e0, e1);
	}

	// Anchor constraint: index 0's MSB must be 0. Swapping the endpoints and
	// inverting every index is decode-identical, so `err` still describes it.
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

// Float fit -> quantize (shared p) -> index -> refit, three times (RGB).
SubsetSolve SolveSubsetMode1(const Vec4f px[16], const int* mem, int n,
							 const Bc7Options& opt) {
	Vec4f e0, e1;
	FitFloatEndpoints(px, mem, n, 3, e0, e1);

	SubsetSolve best{};
	for (int iter = 0; iter < 3; ++iter) {
		// Candidate shared p-bits. The cheap path picks the one minimising both
		// endpoints' quantization error; trialling carries both to block error.
		int ps[2] = {0, 1}, nps = 2;
		if (!opt.trialPBits) {
			float qe[2] = {0, 0};
			for (int p = 0; p <= 1; ++p)
				for (int c = 0; c < 3; ++c) {
					const Quant a = QuantizeChannel(e0[c], 6, p);
					const Quant b = QuantizeChannel(e1[c], 6, p);
					qe[p] += (a.recon - e0[c]) * (a.recon - e0[c]);
					qe[p] += (b.recon - e1[c]) * (b.recon - e1[c]);
				}
			ps[0] = qe[0] <= qe[1] ? 0 : 1;
			nps = 1;
		}

		best.error = 1e30f;
		for (int k = 0; k < nps; ++k) {
			const int p = ps[k];
			int q[2][3];
			Vec4f r0, r1;
			for (int c = 0; c < 3; ++c) {
				const Quant a = QuantizeChannel(e0[c], 6, p);
				const Quant b = QuantizeChannel(e1[c], 6, p);
				q[0][c] = a.code;
				q[1][c] = b.code;
				r0[c] = a.recon;
				r1[c] = b.recon;
			}
			int idx[16];
			float err = 0;
			for (int i = 0; i < n; ++i) {
				float e;
				idx[i] = BestIndex(px[mem[i]], r0, r1, 3, kWeights3, 8, e);
				err += e;
			}
			if (err < best.error) {
				std::memcpy(best.q, q, sizeof(q));
				best.pBit = p;
				std::memcpy(best.idx, idx, sizeof(int) * n);
				best.error = err;
			}
		}

		if (iter < 2) LeastSquaresFit(px, mem, n, 3, kWeights3, best.idx, e0, e1);
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

// Encodes the best of opt.shapeTrials partition shapes; returns its error (or
// +inf if no shape was usable). `out` is written only on a finite result.
float EncodeMode1(const Vec4f px[16], u8 out[16], const Bc7Options& opt) {
	const int trials = std::clamp(opt.shapeTrials, 1, 64);

	// Rank shapes by prescore, keep the cheapest few.
	std::array<int, 64> order;
	for (int s = 0; s < 64; ++s) order[s] = s;
	std::array<float, 64> score;
	for (int s = 0; s < 64; ++s) score[s] = PrescoreShape(px, s);
	std::partial_sort(order.begin(), order.begin() + trials, order.end(),
					  [&](int a, int b) { return score[a] < score[b]; });

	float bestErr = 1e30f;
	int bestShape = -1;
	SubsetSolve bestS0{}, bestS1{};
	int memo0[16], memo1[16], n0 = 0, n1 = 0; // members of the best shape

	for (int t = 0; t < trials; ++t) {
		const int shape = order[t];
		int m0[16], m1[16], c0 = 0, c1 = 0;
		for (int i = 0; i < 16; ++i)
			(kPartition2[shape][i] == 0 ? m0[c0++] : m1[c1++]) = i;
		if (c0 == 0 || c1 == 0) continue; // shapes always split, but be safe

		const SubsetSolve s0 = SolveSubsetMode1(px, m0, c0, opt);
		const SubsetSolve s1 = SolveSubsetMode1(px, m1, c1, opt);
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

// ---- Mode 5: one subset, RGB 7.7.7 + separate alpha 8.8, 2-bit indices ------
//
// Field order (BC7 spec, LSB first):
//   mode[6] rotation[2] R0[7] R1[7] G0[7] G1[7] B0[7] B1[7] A0[8] A1[8]
//   colour-index[31] alpha-index[31]   = 128 bits. No p-bits.
// Both index sets are 2-bit and BOTH drop the MSB at pixel 0 (the single
// subset's anchor), which is where the two 31s come from.
//
// ROTATION IS ALWAYS 0 HERE. A non-zero rotation swaps alpha with one of R/G/B
// before encoding, which would let a block whose ODD channel is a colour use
// the decoupled path. For height-in-alpha content channel 3 already IS the odd
// one out, so the rotations are left unexplored rather than guessed at —
// tools/Bc7Test can settle whether they would pay for the 4x solve.

float EncodeMode5(const Vec4f px[16], u8 out[16]) {
	// --- RGB: the usual principal-axis solve, 7-bit endpoints, no p-bit.
	Vec4f e0, e1;
	FitFloatEndpoints(px, kAllPixels, 16, 3, e0, e1);

	int q0[3]{}, q1[3]{}, cidx[16]{};
	float cerr = 0;
	for (int iter = 0; iter < 3; ++iter) {
		Vec4f r0, r1;
		for (int c = 0; c < 3; ++c) {
			const Quant a = QuantizeChannelNoP(e0[c], 7);
			const Quant b = QuantizeChannelNoP(e1[c], 7);
			q0[c] = a.code;
			q1[c] = b.code;
			r0[c] = a.recon;
			r1[c] = b.recon;
		}
		cerr = 0;
		for (int i = 0; i < 16; ++i) {
			float e;
			cidx[i] = BestIndex(px[i], r0, r1, 3, kWeights2, 4, e);
			cerr += e;
		}
		if (iter < 2) LeastSquaresFit(px, kAllPixels, 16, 3, kWeights2, cidx, e0, e1);
	}

	// --- Alpha: the same machinery over ONE channel, alpha moved into slot 0.
	// The endpoints are 8-bit, so they are exact and every bit of the error
	// comes from having only 4 steps between them.
	Vec4f ap[16]{};
	for (int i = 0; i < 16; ++i) ap[i][0] = px[i][3];

	Vec4f a0, a1;
	FitFloatEndpoints(ap, kAllPixels, 16, 1, a0, a1);

	int qa0 = 0, qa1 = 0, aidx[16]{};
	float aerr = 0;
	for (int iter = 0; iter < 3; ++iter) {
		const Quant a = QuantizeChannelNoP(a0[0], 8);
		const Quant b = QuantizeChannelNoP(a1[0], 8);
		qa0 = a.code;
		qa1 = b.code;
		Vec4f r0{}, r1{};
		r0[0] = a.recon;
		r1[0] = b.recon;
		aerr = 0;
		for (int i = 0; i < 16; ++i) {
			float e;
			aidx[i] = BestIndex(ap[i], r0, r1, 1, kWeights2, 4, e);
			aerr += e;
		}
		if (iter < 2) LeastSquaresFit(ap, kAllPixels, 16, 1, kWeights2, aidx, a0, a1);
	}

	// Anchor fix-up, independently per index set: pixel 0 drops each set's MSB,
	// so each must have it clear. Swap-and-invert is decode-identical.
	if (cidx[0] & 2) {
		std::swap(q0, q1);
		for (int i = 0; i < 16; ++i) cidx[i] = 3 - cidx[i];
	}
	if (aidx[0] & 2) {
		std::swap(qa0, qa1);
		for (int i = 0; i < 16; ++i) aidx[i] = 3 - aidx[i];
	}

	std::memset(out, 0, 16);
	BitWriter bits{out};
	bits.Write(0x20, 6); // mode 5 (five zeros then a one, LSB first)
	bits.Write(0, 2);    // rotation: none
	for (int c = 0; c < 3; ++c) {
		bits.Write(static_cast<u32>(q0[c]), 7);
		bits.Write(static_cast<u32>(q1[c]), 7);
	}
	bits.Write(static_cast<u32>(qa0), 8);
	bits.Write(static_cast<u32>(qa1), 8);
	for (int i = 0; i < 16; ++i) bits.Write(static_cast<u32>(cidx[i]), i == 0 ? 1 : 2);
	for (int i = 0; i < 16; ++i) bits.Write(static_cast<u32>(aidx[i]), i == 0 ? 1 : 2);
	return cerr + aerr;
}

// ---- Mode selection ---------------------------------------------------------

Bc7BlockStat EncodeBlock(const Vec4f px[16], u8 out[16], const Bc7Options& opt) {
	// Modes 0-3 force alpha opaque, so mode 1 is eligible only when every pixel
	// already is. Modes 5 and 6 both carry alpha and are always eligible.
	bool opaque = true;
	for (int i = 0; i < 16 && opaque; ++i) opaque = (px[i][3] >= 255.0f);

	Bc7BlockStat best{0, 1e30f};
	u8 candidate[16];
	const auto consider = [&](u32 mode, float err) {
		if (err < best.error) {
			std::memcpy(out, candidate, 16);
			best = {mode, err};
		}
	};

	if (opt.modes & kBc7Mode6) consider(6, EncodeMode6(px, candidate, opt));
	if ((opt.modes & kBc7Mode1) && opaque) consider(1, EncodeMode1(px, candidate, opt));
	if (opt.modes & kBc7Mode5) consider(5, EncodeMode5(px, candidate));

	// A mask that excluded every eligible mode would leave `out` unwritten;
	// mode 6 is the universal fallback.
	if (best.mode == 0) consider(6, EncodeMode6(px, candidate, opt));
	return best;
}

// Reads one block's 16 pixels, replicating at the image edge.
void GatherBlock(const assets::ImageData& image, u32 bx, u32 by, Vec4f px[16]) {
	for (u32 j = 0; j < 4; ++j) {
		for (u32 i = 0; i < 4; ++i) {
			const u32 x = std::min(bx * 4 + i, image.width - 1);
			const u32 y = std::min(by * 4 + j, image.height - 1);
			const u8* p = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
			for (int c = 0; c < 4; ++c) px[j * 4 + i][c] = static_cast<float>(p[c]);
		}
	}
}

} // namespace

std::vector<u8> EncodeBc7(const assets::ImageData& image, const Bc7Options& opt,
						  std::vector<Bc7BlockStat>* stats) {
	const u32 blocksX = (image.width + 3) / 4;
	const u32 blocksY = (image.height + 3) / 4;
	const size_t blockCount = static_cast<size_t>(blocksX) * blocksY;
	std::vector<u8> out(blockCount * 16);
	if (stats) stats->assign(blockCount, Bc7BlockStat{});

	// Blocks are independent, so this is a plain fan-out over block ROWS: a row
	// is contiguous in `out`, and one row of a 2K texture is far more work than
	// the hand-off costs. Deliberately NOT a Core/ThreadManager client — that
	// registry is for long-lived workers with cadences, watchdogs and a
	// supervisor, and this wants a batch that starts, saturates, and joins.
	unsigned nthreads = opt.threads ? opt.threads : std::thread::hardware_concurrency();
	nthreads = std::clamp(nthreads, 1u, std::max(1u, blocksY));

	std::atomic<u32> nextRow{0};
	const auto encodeRows = [&] {
		for (u32 by = nextRow.fetch_add(1); by < blocksY; by = nextRow.fetch_add(1)) {
			for (u32 bx = 0; bx < blocksX; ++bx) {
				Vec4f px[16];
				GatherBlock(image, bx, by, px);
				const size_t b = static_cast<size_t>(by) * blocksX + bx;
				const Bc7BlockStat st = EncodeBlock(px, &out[b * 16], opt);
				if (stats) (*stats)[b] = st;
			}
		}
	};

	if (nthreads <= 1) {
		encodeRows();
	} else {
		std::vector<std::jthread> pool;
		pool.reserve(nthreads);
		for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(encodeRows);
	} // jthread joins on destruction

	return out;
}

} // namespace dungeon::baker
