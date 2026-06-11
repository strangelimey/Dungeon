#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <vector>

namespace dungeon::gfx {

inline constexpr u32 kMaxPointLights = 16;

struct PointLight {
    Vec3 position{};
    float radius = 8.0f;     // attenuation reaches zero here
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
};

struct DirectionalLight {
    Vec3 direction{0, -1, 0};
    Vec3 color{0, 0, 0}; // black = disabled
};

struct LightSet {
    Vec3 ambient{0.05f, 0.05f, 0.07f};
    DirectionalLight directional;
    std::vector<PointLight> points; // first kMaxPointLights are used
};

} // namespace dungeon::gfx
