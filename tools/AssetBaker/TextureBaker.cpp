#include "TextureBaker.h"

#include "Core/Log.h"
#include "Core/Types.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace dungeon::baker {

namespace {

constexpr u32 kSize = 512;

// --- noise helpers -----------------------------------------------------------

float Hash(u32 x, u32 y, u32 seed) {
    u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
}

float ValueNoise(float x, float y, u32 seed) {
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const u32 xi = static_cast<u32>(std::floor(x)), yi = static_cast<u32>(std::floor(y));
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi, seed), b = Hash(xi + 1, yi, seed);
    const float c = Hash(xi, yi + 1, seed), d = Hash(xi + 1, yi + 1, seed);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

float Fbm(float x, float y, u32 seed, int octaves = 4) {
    float value = 0, amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        value += ValueNoise(x, y, seed + static_cast<u32>(i)) * amplitude;
        x *= 2.0f;
        y *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

// --- height field + image plumbing -------------------------------------------

struct HeightField {
    std::vector<float> h = std::vector<float>(kSize * kSize);
    float& At(u32 x, u32 y) { return h[(y % kSize) * kSize + (x % kSize)]; }
    float Get(u32 x, u32 y) const { return h[(y % kSize) * kSize + (x % kSize)]; }
};

struct Rgb {
    float r, g, b;
};

bool SavePng(const std::string& path, const std::vector<u8>& rgba) {
    const int ok = stbi_write_png(path.c_str(), kSize, kSize, 4, rgba.data(), kSize * 4);
    if (!ok) log::Error("Failed to write {}", path);
    else log::Info("Wrote {}", path);
    return ok != 0;
}

// Albedo = per-pixel color function modulated by height-based shading.
bool SaveAlbedo(const std::string& path, const HeightField& height,
                const std::function<Rgb(u32, u32, float)>& colorAt) {
    std::vector<u8> rgba(kSize * kSize * 4);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const float h = height.Get(x, y);
            Rgb c = colorAt(x, y, h);
            // Crevice darkening: lower areas collect grime.
            const float shade = 0.55f + 0.45f * h;
            const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
            rgba[i + 0] = static_cast<u8>(std::clamp(c.r * shade, 0.0f, 1.0f) * 255);
            rgba[i + 1] = static_cast<u8>(std::clamp(c.g * shade, 0.0f, 1.0f) * 255);
            rgba[i + 2] = static_cast<u8>(std::clamp(c.b * shade, 0.0f, 1.0f) * 255);
            rgba[i + 3] = 255;
        }
    }
    return SavePng(path, rgba);
}

// Normal map from height gradient (tiling), height in alpha for parallax.
bool SaveNormalHeight(const std::string& path, const HeightField& height,
                      float strength) {
    std::vector<u8> rgba(kSize * kSize * 4);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const float dx =
                (height.Get(x + 1, y) - height.Get(x + kSize - 1, y)) * strength;
            const float dy =
                (height.Get(x, y + 1) - height.Get(x, y + kSize - 1)) * strength;
            const float invLen = 1.0f / std::sqrt(dx * dx + dy * dy + 1.0f);
            const float nx = -dx * invLen, ny = -dy * invLen, nz = invLen;
            const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
            rgba[i + 0] = static_cast<u8>((nx * 0.5f + 0.5f) * 255);
            rgba[i + 1] = static_cast<u8>((ny * 0.5f + 0.5f) * 255);
            rgba[i + 2] = static_cast<u8>((nz * 0.5f + 0.5f) * 255);
            rgba[i + 3] = static_cast<u8>(std::clamp(height.Get(x, y), 0.0f, 1.0f) * 255);
        }
    }
    return SavePng(path, rgba);
}

// Rounded plateau: 1 in the interior, falling to 0 within `edge` of a border.
float Plateau(float v, float extent, float edge) {
    const float d = std::min(v, extent - v);
    return std::clamp(d / edge, 0.0f, 1.0f);
}

// --- material generators -------------------------------------------------------

void BrickHeight(HeightField& hf, u32 rows, u32 cols, u32 seed) {
    const float bw = static_cast<float>(kSize) / cols;
    const float bh = static_cast<float>(kSize) / rows;
    const float edge = kSize / 110.0f;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const u32 row = static_cast<u32>(y / bh);
            const float xs = x + (row % 2 ? bw * 0.5f : 0.0f);
            const float bx = std::fmod(xs, bw), by = std::fmod(static_cast<float>(y), bh);
            const u32 col = static_cast<u32>(xs / bw);
            const float top = 0.75f + Hash(col, row, seed) * 0.2f; // per-brick height
            float h = top * std::min(Plateau(bx, bw, edge), Plateau(by, bh, edge));
            h += (Fbm(x * 0.05f, y * 0.05f, seed + 9) - 0.5f) * 0.18f;
            hf.At(x, y) = std::clamp(h, 0.0f, 1.0f);
        }
    }
}

void CobbleHeight(HeightField& hf, u32 cells, u32 seed) {
    // Domed stones on a jittered grid.
    const float cell = static_cast<float>(kSize) / cells;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            float best = 0.0f;
            const int cx = static_cast<int>(x / cell), cy = static_cast<int>(y / cell);
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const u32 gx = static_cast<u32>(cx + ox + cells) % cells;
                    const u32 gy = static_cast<u32>(cy + oy + cells) % cells;
                    const float jx = (gx + 0.3f + Hash(gx, gy, seed) * 0.4f) * cell;
                    const float jy = (gy + 0.3f + Hash(gx, gy, seed + 1) * 0.4f) * cell;
                    const float radius = cell * (0.42f + Hash(gx, gy, seed + 2) * 0.16f);
                    // Tiling-aware distance.
                    float dx = std::fabs(x - jx), dy = std::fabs(y - jy);
                    dx = std::min(dx, kSize - dx);
                    dy = std::min(dy, kSize - dy);
                    const float d = std::sqrt(dx * dx + dy * dy) / radius;
                    if (d < 1.0f) best = std::max(best, std::sqrt(1.0f - d * d));
                }
            }
            hf.At(x, y) = std::clamp(
                best * 0.9f + (Fbm(x * 0.04f, y * 0.04f, seed + 7) - 0.5f) * 0.15f, 0.0f,
                1.0f);
        }
    }
}

void RoughHeight(HeightField& hf, u32 seed, float scale) {
    for (u32 y = 0; y < kSize; ++y)
        for (u32 x = 0; x < kSize; ++x)
            hf.At(x, y) = Fbm(x * scale, y * scale, seed, 5);
}

void AddCracks(HeightField& hf, u32 seed) {
    // Thin dark fissures where a low-frequency noise band crosses zero.
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const float n = Fbm(x * 0.012f, y * 0.012f, seed, 3) - 0.5f;
            const float crack = std::clamp(1.0f - std::fabs(n) * 28.0f, 0.0f, 1.0f);
            hf.At(x, y) = std::clamp(hf.At(x, y) - crack * 0.55f, 0.0f, 1.0f);
        }
    }
}

} // namespace

bool BakeTextures(const std::string& dir) {
    bool ok = true;
    auto path = [&](const char* name) { return dir + "\\" + name; };

    // --- walls -----------------------------------------------------------
    {
        HeightField hf;
        BrickHeight(hf, 8, 4, 11u);
        ok &= SaveAlbedo(path("wall_brick.png"), hf, [&](u32 x, u32 y, float) {
            const u32 row = y / (kSize / 8), col = x / (kSize / 4);
            const float v = 0.38f + Hash(col, row, 5u) * 0.16f +
                            (Fbm(x * 0.06f, y * 0.06f, 7u) - 0.5f) * 0.2f;
            return Rgb{v, v * 0.9f, v * 0.78f};
        });
        ok &= SaveNormalHeight(path("wall_brick_n.png"), hf, 5.0f);
    }
    {
        HeightField hf;
        BrickHeight(hf, 4, 3, 21u); // large rough stone blocks
        ok &= SaveAlbedo(path("wall_stone.png"), hf, [&](u32 x, u32 y, float) {
            const float v = 0.34f + (Fbm(x * 0.03f, y * 0.03f, 17u) - 0.5f) * 0.3f;
            return Rgb{v * 0.95f, v, v * 1.05f};
        });
        ok &= SaveNormalHeight(path("wall_stone_n.png"), hf, 6.0f);
    }
    {
        HeightField hf;
        BrickHeight(hf, 8, 4, 31u);
        ok &= SaveAlbedo(path("wall_moss.png"), hf, [&](u32 x, u32 y, float h) {
            const float v = 0.34f + (Fbm(x * 0.06f, y * 0.06f, 33u) - 0.5f) * 0.2f;
            Rgb c{v, v * 0.92f, v * 0.8f};
            // Moss creeps into the cracks and lower half.
            const float moss = std::clamp(
                Fbm(x * 0.02f, y * 0.02f, 35u) * 1.6f - 0.55f - h * 0.5f +
                    static_cast<float>(y) / kSize * 0.4f,
                0.0f, 1.0f);
            c.r = c.r * (1 - moss) + 0.12f * moss;
            c.g = c.g * (1 - moss) + 0.34f * moss;
            c.b = c.b * (1 - moss) + 0.10f * moss;
            return c;
        });
        ok &= SaveNormalHeight(path("wall_moss_n.png"), hf, 5.0f);
    }

    // --- floors --------------------------------------------------------------
    {
        HeightField hf;
        BrickHeight(hf, 2, 2, 41u); // big square slabs
        ok &= SaveAlbedo(path("floor_slabs.png"), hf, [&](u32 x, u32 y, float) {
            const u32 row = y / (kSize / 2), col = x / (kSize / 2);
            const float v = 0.34f + Hash(col, row, 43u) * 0.1f +
                            (Fbm(x * 0.05f, y * 0.05f, 45u) - 0.5f) * 0.22f;
            return Rgb{v * 0.92f, v * 0.92f, v};
        });
        ok &= SaveNormalHeight(path("floor_slabs_n.png"), hf, 5.0f);
    }
    {
        HeightField hf;
        CobbleHeight(hf, 7, 51u);
        ok &= SaveAlbedo(path("floor_cobble.png"), hf, [&](u32 x, u32 y, float h) {
            const float v = 0.30f + h * 0.12f +
                            (Fbm(x * 0.05f, y * 0.05f, 53u) - 0.5f) * 0.18f;
            return Rgb{v, v * 0.96f, v * 0.88f};
        });
        ok &= SaveNormalHeight(path("floor_cobble_n.png"), hf, 6.5f);
    }

    // --- ceilings --------------------------------------------------------------
    {
        HeightField hf;
        RoughHeight(hf, 61u, 0.035f);
        ok &= SaveAlbedo(path("ceiling_rough.png"), hf, [&](u32 x, u32 y, float) {
            const float v = 0.30f + (Fbm(x * 0.04f, y * 0.04f, 63u) - 0.5f) * 0.2f;
            return Rgb{v, v * 0.95f, v * 0.88f};
        });
        ok &= SaveNormalHeight(path("ceiling_rough_n.png"), hf, 4.0f);
    }
    {
        HeightField hf;
        RoughHeight(hf, 71u, 0.03f);
        AddCracks(hf, 73u);
        ok &= SaveAlbedo(path("ceiling_cracked.png"), hf, [&](u32 x, u32 y, float h) {
            const float v = 0.28f + h * 0.1f +
                            (Fbm(x * 0.04f, y * 0.04f, 75u) - 0.5f) * 0.16f;
            return Rgb{v, v * 0.94f, v * 0.9f};
        });
        ok &= SaveNormalHeight(path("ceiling_cracked_n.png"), hf, 5.0f);
    }
    return ok;
}

} // namespace dungeon::baker
