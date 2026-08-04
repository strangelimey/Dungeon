// ============================================================================
// Bc7Decode.h — a BC7 decoder, written for the test harness only.
//
// This exists to be a SECOND OPINION on what the encoder wrote. It is derived
// from the format's field tables rather than from Bc7Encoder.cpp's bit writers:
// the encoder's error estimate never touches the packed bytes and this never
// touches anything else, so when the two agree on a block's error, the packing,
// the anchor handling, the p-bit placement, the subset assignment and the
// endpoint reconstruction all have to be right together.
//
// WHAT THIS CANNOT CATCH, stated plainly: a misreading of the spec shared by
// both sides — if the encoder writes a field in the wrong order and this reads
// it back in that same wrong order, they agree and the GPU does not. For modes
// 1 and 6 that risk is already retired (the game renders their output). A NEW
// mode's field layout is only proven once the GPU has drawn it.
// ============================================================================
#pragma once

#include "Assets/Image.h"
#include "Core/Types.h"

namespace dungeon::bc7test {

// Decodes one 16-byte block into 16 RGBA pixels in raster order. Returns the
// mode read out of the block, or -1 for a mode this decoder does not implement
// (in which case the pixels come back magenta, so an unhandled mode is loud
// rather than plausible).
int DecodeBc7Block(const u8 block[16], u8 out[16][4]);

// Decodes a whole BC7 surface. `blocks` must hold ceil(w/4)*ceil(h/4)*16 bytes.
assets::ImageData DecodeBc7(const u8* blocks, u32 width, u32 height);

} // namespace dungeon::bc7test
