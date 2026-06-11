#include "Game/ProceduralTextures.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {

// Deterministic hash noise in [0,1).
float Hash(u32 x, u32 y, u32 seed) {
    u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
}

// Smooth value noise.
float ValueNoise(float x, float y, u32 seed) {
    const u32 xi = static_cast<u32>(std::floor(x));
    const u32 yi = static_cast<u32>(std::floor(y));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float sx = fx * fx * (3 - 2 * fx);
    const float sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi, seed);
    const float b = Hash(xi + 1, yi, seed);
    const float c = Hash(xi, yi + 1, seed);
    const float d = Hash(xi + 1, yi + 1, seed);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

float Fbm(float x, float y, u32 seed) {
    float value = 0, amplitude = 0.5f;
    for (int i = 0; i < 4; ++i) {
        value += ValueNoise(x, y, seed + static_cast<u32>(i)) * amplitude;
        x *= 2.0f;
        y *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

void PutPixel(assets::ImageData& img, u32 x, u32 y, float r, float g, float b) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    img.pixels[i + 0] = static_cast<u8>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 1] = static_cast<u8>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 2] = static_cast<u8>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 3] = 255;
}

assets::ImageData MakeImage(u32 size) {
    assets::ImageData img;
    img.width = img.height = size;
    img.pixels.resize(static_cast<size_t>(size) * size * 4);
    return img;
}

} // namespace

assets::ImageData MakeBrickWallTexture(u32 size) {
    assets::ImageData img = MakeImage(size);
    const float brickW = size / 4.0f;
    const float brickH = size / 8.0f;
    const float mortar = size / 96.0f;

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const u32 row = static_cast<u32>(y / brickH);
            // Offset every other row by half a brick.
            const float xs = x + (row % 2 ? brickW * 0.5f : 0.0f);
            const float bx = std::fmod(xs, brickW);
            const float by = std::fmod(static_cast<float>(y), brickH);
            const u32 col = static_cast<u32>(xs / brickW);

            const bool isMortar = bx < mortar || by < mortar;
            const float grain =
                Fbm(x * 0.06f, y * 0.06f, 7u) * 0.25f +
                Hash(x, y, 3u) * 0.06f;

            if (isMortar) {
                const float v = 0.16f + grain * 0.4f;
                PutPixel(img, x, y, v, v * 0.95f, v * 0.9f);
            } else {
                const float brickTint = Hash(col, row, 11u);
                const float v = 0.32f + brickTint * 0.18f + grain;
                PutPixel(img, x, y, v * 1.0f, v * 0.92f, v * 0.82f);
            }
        }
    }
    return img;
}

assets::ImageData MakeFloorSlabTexture(u32 size) {
    assets::ImageData img = MakeImage(size);
    const float slab = size / 2.0f;
    const float gap = size / 80.0f;

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const float sx = std::fmod(static_cast<float>(x), slab);
            const float sy = std::fmod(static_cast<float>(y), slab);
            const u32 col = x / static_cast<u32>(slab);
            const u32 row = y / static_cast<u32>(slab);

            const float grain = Fbm(x * 0.045f, y * 0.045f, 23u) * 0.3f;
            if (sx < gap || sy < gap) {
                const float v = 0.10f + grain * 0.3f;
                PutPixel(img, x, y, v, v, v * 0.95f);
            } else {
                const float tint = Hash(col, row, 31u) * 0.12f;
                const float v = 0.30f + tint + grain;
                PutPixel(img, x, y, v * 0.9f, v * 0.9f, v * 0.95f);
            }
        }
    }
    return img;
}

assets::ImageData MakeCeilingTexture(u32 size) {
    assets::ImageData img = MakeImage(size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const float v = 0.18f + Fbm(x * 0.05f, y * 0.05f, 47u) * 0.22f;
            PutPixel(img, x, y, v, v * 0.96f, v * 0.9f);
        }
    }
    return img;
}

} // namespace dungeon::game
