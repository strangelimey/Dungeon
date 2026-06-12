// ============================================================================
// Bc7Encoder.cpp — a compact BC7 encoder using mode 6 exclusively.
//
// Mode 6 (one subset, RGBA 7.7.7.7 endpoints + per-endpoint p-bit, 4-bit
// indices) handles photographic albedo and packed normal+height maps well,
// and restricting to it keeps the encoder small and fast. Per 4x4 block:
//   1. principal axis of the pixels (power iteration on the covariance)
//   2. endpoints = extreme projections onto that axis
//   3. iterate: quantize endpoints -> assign best indices -> least-squares
//      refit endpoints from the indices
//   4. anchor fixup (index 0 must have its MSB clear) and bit packing
// ============================================================================
#include "Bc7Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dungeon::baker {

namespace {

// BC7 4-bit interpolation weights (out of 64).
constexpr int kWeights[16] = {0,  4,  9,  13, 17, 21, 26, 30,
							  34, 38, 43, 47, 51, 55, 60, 64};

struct Vec4f {
	float v[4];
	float& operator[](int i) { return v[i]; }
	float operator[](int i) const { return v[i]; }
};

float Dot(const Vec4f& a, const Vec4f& b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// Quantizes a float endpoint (0..255 per channel) to 7 bits + the shared
// p-bit; tries both p values and keeps the closer reconstruction.
void QuantizeEndpoint(const Vec4f& e, int q7[4], int& pBit, Vec4f& recon) {
	float bestErr = 1e30f;
	for (int p = 0; p <= 1; ++p) {
		int q[4];
		Vec4f r;
		float err = 0;
		for (int c = 0; c < 4; ++c) {
			q[c] = std::clamp(static_cast<int>(std::lround((e[c] - p) * 0.5f)), 0, 127);
			r[c] = static_cast<float>((q[c] << 1) | p);
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

// Best 4-bit index per pixel against the reconstructed endpoint segment.
void ComputeIndices(const Vec4f px[16], const Vec4f& r0, const Vec4f& r1, int idx[16]) {
	Vec4f seg;
	for (int c = 0; c < 4; ++c) seg[c] = r1[c] - r0[c];
	const float len2 = Dot(seg, seg);
	if (len2 < 1e-6f) {
		std::fill(idx, idx + 16, 0);
		return;
	}
	for (int i = 0; i < 16; ++i) {
		Vec4f d;
		for (int c = 0; c < 4; ++c) d[c] = px[i][c] - r0[c];
		const float t = std::clamp(Dot(d, seg) / len2, 0.0f, 1.0f) * 64.0f;
		int best = 0;
		float bestDist = 1e30f;
		for (int w = 0; w < 16; ++w) {
			const float dist = std::fabs(t - kWeights[w]);
			if (dist < bestDist) {
				bestDist = dist;
				best = w;
			}
		}
		idx[i] = best;
	}
}

// Refits float endpoints from fixed indices (per-channel 2x2 least squares).
void LeastSquaresFit(const Vec4f px[16], const int idx[16], Vec4f& e0, Vec4f& e1) {
	float a00 = 0, a01 = 0, a11 = 0;
	float b0[4] = {}, b1[4] = {};
	for (int i = 0; i < 16; ++i) {
		const float w = kWeights[idx[i]] / 64.0f;
		const float iw = 1.0f - w;
		a00 += iw * iw;
		a01 += iw * w;
		a11 += w * w;
		for (int c = 0; c < 4; ++c) {
			b0[c] += iw * px[i][c];
			b1[c] += w * px[i][c];
		}
	}
	const float det = a00 * a11 - a01 * a01;
	if (std::fabs(det) < 1e-4f) return; // degenerate (all one index) — keep endpoints
	const float inv = 1.0f / det;
	for (int c = 0; c < 4; ++c) {
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

void EncodeBlock(const Vec4f px[16], u8 out[16]) {
	std::memset(out, 0, 16);

	// Principal axis through the mean (power iteration on the covariance).
	Vec4f mean{};
	for (int i = 0; i < 16; ++i)
		for (int c = 0; c < 4; ++c) mean[c] += px[i][c];
	for (int c = 0; c < 4; ++c) mean[c] /= 16.0f;

	float cov[4][4] = {};
	for (int i = 0; i < 16; ++i)
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				cov[r][c] += (px[i][r] - mean[r]) * (px[i][c] - mean[c]);

	Vec4f axis{{1, 1, 1, 1}};
	for (int iter = 0; iter < 8; ++iter) {
		Vec4f next{};
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c) next[r] += cov[r][c] * axis[c];
		const float len = std::sqrt(Dot(next, next));
		if (len < 1e-6f) break; // flat block — axis direction is irrelevant
		for (int c = 0; c < 4; ++c) axis[c] = next[c] / len;
	}

	// Extreme projections onto the axis as initial endpoints.
	float tMin = 1e30f, tMax = -1e30f;
	for (int i = 0; i < 16; ++i) {
		Vec4f d;
		for (int c = 0; c < 4; ++c) d[c] = px[i][c] - mean[c];
		const float t = Dot(d, axis);
		tMin = std::min(tMin, t);
		tMax = std::max(tMax, t);
	}
	Vec4f e0, e1;
	for (int c = 0; c < 4; ++c) {
		e0[c] = std::clamp(mean[c] + axis[c] * tMin, 0.0f, 255.0f);
		e1[c] = std::clamp(mean[c] + axis[c] * tMax, 0.0f, 255.0f);
	}

	// Quantize -> index -> refit, twice, then final quantize + index.
	int q0[4], q1[4], p0 = 0, p1 = 0, idx[16];
	Vec4f r0, r1;
	for (int iter = 0; iter < 3; ++iter) {
		QuantizeEndpoint(e0, q0, p0, r0);
		QuantizeEndpoint(e1, q1, p1, r1);
		ComputeIndices(px, r0, r1, idx);
		if (iter < 2) LeastSquaresFit(px, idx, e0, e1);
	}

	// Anchor constraint: index 0's MSB must be 0; swap endpoints if not.
	if (idx[0] & 8) {
		std::swap(q0, q1);
		std::swap(p0, p1);
		for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
	}

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
