// ============================================================================
// Bc7Decode.cpp — decoding BC7 straight from the format's field tables.
//
// Each supported mode is transcribed from its spec row: how many subsets, how
// many bits each endpoint component gets, where the p-bits live and who shares
// them, and the index precision. The field ORDER within a block is always the
// same shape — mode marker, partition, rotation/selector, then every colour
// component's endpoints grouped by channel, then alpha, then p-bits, then the
// index arrays — and the anchor pixels drop the top bit of their index because
// the encoder is required to have made it zero.
// ============================================================================
#include "Bc7Decode.h"

#include "AssetBaker/Bc7Tables.h"

#include <algorithm>
#include <cstring>

namespace dungeon::bc7test {

namespace {

using baker::bc7::Expand;
using baker::bc7::kAnchor2;
using baker::bc7::kPartition2;
using baker::bc7::kWeights2;
using baker::bc7::kWeights3;
using baker::bc7::kWeights4;

// LSB-first bit reader over the 16-byte block.
struct BitReader {
	const u8* in;
	int bit = 0;
	u32 Read(int count) {
		u32 v = 0;
		for (int i = 0; i < count; ++i) {
			if ((in[bit >> 3] >> (bit & 7)) & 1) v |= 1u << i;
			++bit;
		}
		return v;
	}
};

// The hardware blend between two 8-bit endpoint components.
int Interp(int a, int b, int weight) {
	return (a * (64 - weight) + b * weight + 32) >> 6;
}

void FillMagenta(u8 out[16][4]) {
	for (int i = 0; i < 16; ++i) {
		out[i][0] = 255;
		out[i][1] = 0;
		out[i][2] = 255;
		out[i][3] = 255;
	}
}

// ---- Modes 1 and 3 (the two-subset, RGB-only modes) -------------------------
// Both: 6 partition bits, then every channel's four endpoint components grouped
// together (subset0.lo, subset0.hi, subset1.lo, subset1.hi), then the p-bits,
// then the indices. Alpha is implicitly opaque. They differ in three numbers:
//
//   mode 1 — 6 colour bits, ONE p-bit shared by a subset's two endpoints
//            (so 2 p-bits total), 3-bit indices.
//   mode 3 — 7 colour bits, a p-bit PER ENDPOINT (4 total), 2-bit indices.
//
// In both cases the code plus its p-bit forms a value one bit wider, which the
// hardware then replicates up to 8.
void DecodeTwoSubset(BitReader& r, u8 out[16][4], int colourBits, int indexBits,
					 bool perEndpointP, const int* weights) {
	const int shape = static_cast<int>(r.Read(6));

	int code[4][3]; // [endpoint][channel]
	for (int c = 0; c < 3; ++c)
		for (int e = 0; e < 4; ++e) code[e][c] = static_cast<int>(r.Read(colourBits));

	// Shared: one p-bit per subset, so endpoints 0,1 share and 2,3 share.
	// Per-endpoint: four p-bits, in the same endpoint order as the codes.
	int p[4];
	if (perEndpointP) {
		for (int e = 0; e < 4; ++e) p[e] = static_cast<int>(r.Read(1));
	} else {
		const int p0 = static_cast<int>(r.Read(1));
		const int p1 = static_cast<int>(r.Read(1));
		p[0] = p[1] = p0;
		p[2] = p[3] = p1;
	}

	int ep[4][3];
	for (int e = 0; e < 4; ++e)
		for (int c = 0; c < 3; ++c)
			ep[e][c] = Expand((code[e][c] << 1) | p[e], colourBits + 1);

	// Anchors: pixel 0 for subset 0, the shape's fix-up pixel for subset 1. Both
	// store one bit fewer.
	const int anchor1 = kAnchor2[shape];
	int idx[16];
	for (int i = 0; i < 16; ++i) {
		const bool anchor = (i == 0) || (i == anchor1);
		idx[i] = static_cast<int>(r.Read(anchor ? indexBits - 1 : indexBits));
	}

	for (int i = 0; i < 16; ++i) {
		const int s = kPartition2[shape][i];
		const int w = weights[idx[i]];
		for (int c = 0; c < 3; ++c)
			out[i][c] = static_cast<u8>(Interp(ep[s * 2][c], ep[s * 2 + 1][c], w));
		out[i][3] = 255;
	}
}

// ---- Mode 5 -----------------------------------------------------------------
// 1 subset, 2 rotation bits, RGB endpoints of 7 bits, alpha endpoints of a full
// 8, no p-bits, and TWO independent 2-bit index arrays — colour and alpha.
// Layout: rotation[2] R[2x7] G[2x7] B[2x7] A[2x8] colourIdx[31] alphaIdx[31].
// Pixel 0 is the anchor for both arrays.
void DecodeMode5(BitReader& r, u8 out[16][4]) {
	const int rotation = static_cast<int>(r.Read(2));

	int rgb[2][3];
	for (int c = 0; c < 3; ++c)
		for (int e = 0; e < 2; ++e) rgb[e][c] = static_cast<int>(r.Read(7));

	int a[2];
	a[0] = static_cast<int>(r.Read(8));
	a[1] = static_cast<int>(r.Read(8));

	for (int e = 0; e < 2; ++e)
		for (int c = 0; c < 3; ++c) rgb[e][c] = Expand(rgb[e][c], 7);
	// Alpha is already 8 bits — nothing to replicate.

	int cidx[16], aidx[16];
	for (int i = 0; i < 16; ++i) cidx[i] = static_cast<int>(r.Read(i == 0 ? 1 : 2));
	for (int i = 0; i < 16; ++i) aidx[i] = static_cast<int>(r.Read(i == 0 ? 1 : 2));

	for (int i = 0; i < 16; ++i) {
		for (int c = 0; c < 3; ++c)
			out[i][c] = static_cast<u8>(Interp(rgb[0][c], rgb[1][c], kWeights2[cidx[i]]));
		out[i][3] = static_cast<u8>(Interp(a[0], a[1], kWeights2[aidx[i]]));

		// The rotation names the channel that was encoded THROUGH the alpha
		// slot; undo it by swapping that channel back with alpha.
		if (rotation != 0) std::swap(out[i][rotation - 1], out[i][3]);
	}
}

// ---- Mode 6 -----------------------------------------------------------------
// 1 subset, RGBA endpoints of 7 bits each plus a p-bit PER ENDPOINT (so 8 bits
// of real precision), 4-bit indices.
// Layout: R[2x7] G[2x7] B[2x7] A[2x7] P[2] indices.
void DecodeMode6(BitReader& r, u8 out[16][4]) {
	int code[2][4];
	for (int c = 0; c < 4; ++c)
		for (int e = 0; e < 2; ++e) code[e][c] = static_cast<int>(r.Read(7));

	int p[2];
	p[0] = static_cast<int>(r.Read(1));
	p[1] = static_cast<int>(r.Read(1));

	int ep[2][4];
	for (int e = 0; e < 2; ++e)
		for (int c = 0; c < 4; ++c) ep[e][c] = Expand((code[e][c] << 1) | p[e], 8);

	int idx[16];
	for (int i = 0; i < 16; ++i) idx[i] = static_cast<int>(r.Read(i == 0 ? 3 : 4));

	for (int i = 0; i < 16; ++i)
		for (int c = 0; c < 4; ++c)
			out[i][c] = static_cast<u8>(Interp(ep[0][c], ep[1][c], kWeights4[idx[i]]));
}

} // namespace

int DecodeBc7Block(const u8 block[16], u8 out[16][4]) {
	BitReader r{block};

	// The mode is unary: N zero bits then a one. Eight zeros is the reserved
	// encoding and decodes to opaque black on real hardware.
	int mode = 0;
	while (mode < 8 && r.Read(1) == 0) ++mode;
	if (mode >= 8) {
		for (int i = 0; i < 16; ++i) {
			out[i][0] = out[i][1] = out[i][2] = 0;
			out[i][3] = 255;
		}
		return -1;
	}

	switch (mode) {
	case 1: DecodeTwoSubset(r, out, 6, 3, false, kWeights3); return 1;
	case 3: DecodeTwoSubset(r, out, 7, 2, true, kWeights2); return 3;
	case 5: DecodeMode5(r, out); return 5;
	case 6: DecodeMode6(r, out); return 6;
	default: FillMagenta(out); return -1;
	}
}

assets::ImageData DecodeBc7(const u8* blocks, u32 width, u32 height) {
	assets::ImageData img;
	img.width = width;
	img.height = height;
	img.pixels.assign(static_cast<size_t>(width) * height * 4, 0);

	const u32 blocksX = (width + 3) / 4;
	const u32 blocksY = (height + 3) / 4;
	for (u32 by = 0; by < blocksY; ++by) {
		for (u32 bx = 0; bx < blocksX; ++bx) {
			u8 px[16][4];
			DecodeBc7Block(&blocks[(static_cast<size_t>(by) * blocksX + bx) * 16], px);
			for (u32 j = 0; j < 4; ++j) {
				for (u32 i = 0; i < 4; ++i) {
					const u32 x = bx * 4 + i, y = by * 4 + j;
					if (x >= width || y >= height) continue; // edge padding
					std::memcpy(&img.pixels[(static_cast<size_t>(y) * width + x) * 4],
								px[j * 4 + i], 4);
				}
			}
		}
	}
	return img;
}

} // namespace dungeon::bc7test
