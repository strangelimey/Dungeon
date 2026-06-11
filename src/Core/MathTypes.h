#pragma once

#include <DirectXMath.h>

#include <cmath>
#include <numbers>

namespace dungeon {

// DirectXMath is the de-facto math library for D3D12; we alias the storage
// types and add the small helpers the engine needs.
using Vec2 = DirectX::XMFLOAT2;
using Vec3 = DirectX::XMFLOAT3;
using Vec4 = DirectX::XMFLOAT4;
using Mat4 = DirectX::XMFLOAT4X4;
using Quat = DirectX::XMFLOAT4;

inline constexpr float kPi = std::numbers::pi_v<float>;

inline Mat4 Mat4Identity() {
    Mat4 m;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
    return m;
}

inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
}

// Shortest-arc angle interpolation in radians (for smooth turning).
inline float LerpAngle(float a, float b, float t) {
    float diff = std::fmod(b - a, 2.0f * kPi);
    if (diff > kPi) diff -= 2.0f * kPi;
    if (diff < -kPi) diff += 2.0f * kPi;
    return a + diff * t;
}

} // namespace dungeon
