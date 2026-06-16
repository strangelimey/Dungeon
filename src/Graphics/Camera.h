// ============================================================================
// Graphics/Camera.h — perspective camera.
//
// Left-handed, +Y up; yaw 0 looks down +Z and forward = (sin yaw, 0, cos yaw)
// (matches Party facing math). The dungeon crawler drives position + yaw;
// pitch stays available for effects (head bob, look up/down).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"

namespace dungeon::gfx {
class Camera {
public:
	void SetPosition(const Vec3& position) { m_position = position; }
	void SetYawPitch(float yaw, float pitch) { m_yaw = yaw; m_pitch = pitch; }
	void SetLens(float fovY, float aspect, float nearZ, float farZ) {
		m_fovY = fovY; m_aspect = aspect; m_nearZ = nearZ; m_farZ = farZ;
	}

	const Vec3& Position() const { return m_position; }
	float Yaw() const { return m_yaw; }

	Vec3 Forward() const {
		return {std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch),
				std::cos(m_yaw) * std::cos(m_pitch)};
	}

	// Row-convention view-projection (v' = v * V * P), stored row-major.
	Mat4 ViewProj() const {
		using namespace DirectX;
		const Vec3 fwd = Forward();
		const XMVECTOR eye = XMLoadFloat3(&m_position);
		const XMVECTOR dir = XMVectorSet(fwd.x, fwd.y, fwd.z, 0.0f);
		const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
		const XMMATRIX view = XMMatrixLookToLH(eye, dir, up);
		const XMMATRIX proj =
			XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
		// The world compass is right-handed (east = +X, north = -Z, up = +Y), but
		// LookToLH treats the basis as left-handed, which mirrors the view left/
		// right (east would render on screen-left when facing north). Negate clip-
		// space X to un-mirror so the first-person view matches the map overlay and
		// the facing labels. NOTE: this flips apparent winding — the back-face-
		// culled scene PSO compensates with FrontCounterClockwise (see Renderer.cpp).
		const XMMATRIX mirrorX = XMMatrixScaling(-1.0f, 1.0f, 1.0f);
		Mat4 out;
		XMStoreFloat4x4(&out, view * proj * mirrorX);
		return out;
	}

private:
	Vec3 m_position{0, 0, 0};
	float m_yaw = 0.0f;
	float m_pitch = 0.0f;
	float m_fovY = 70.0f * kPi / 180.0f;
	float m_aspect = 16.0f / 9.0f;
	float m_nearZ = 0.05f;
	float m_farZ = 100.0f;
};

} // namespace dungeon::gfx
