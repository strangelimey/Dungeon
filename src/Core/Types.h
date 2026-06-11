// ============================================================================
// Core/Types.h — fixed-width integer and float aliases used engine-wide.
//
// Every module spells sized types the same way (u32, i16, f32...) so that
// serialization, GPU layouts, and audio sample formats read unambiguously.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>

namespace dungeon {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

} // namespace dungeon
