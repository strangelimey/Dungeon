// ============================================================================
// Graphics/ParticleBatch.h — camera-facing billboard particles.
//
// The game simulates particles (see game::FireEffect) and hands this class a
// flat instance list each frame; quads are built CPU-side into the per-frame
// upload arena and drawn in one call after the opaque scene. Colors are
// PREMULTIPLIED: additive particles (flame, sparks) carry alpha 0, smoke
// carries its opacity — one blend state covers both. The caller sorts
// instances back-to-front (only smoke actually needs it; additive blending
// is order-independent).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Graphics/Camera.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Texture.h"
#include "Graphics/UploadAllocator.h"

#include <memory>
#include <span>

namespace dungeon::gfx {

struct ParticleInstance {
	Vec3 position;
	float size;  // world-space half-extent of the billboard
	Vec4 color;  // premultiplied (see header comment)
};

class ParticleBatch {
public:
	explicit ParticleBatch(GraphicsDevice& device);

	void NewFrame(u32 frameIndex);

	// Draws the instances with the back buffer + depth already bound (call
	// after the opaque scene pass).
	void Render(ID3D12GraphicsCommandList* list, const Camera& camera,
				std::span<const ParticleInstance> instances);

private:
	struct ParticleVertex {
		Vec3 position;
		Vec2 uv;
		Vec4 color;
	};

	GraphicsDevice& m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
	std::unique_ptr<Texture> m_sprite; // soft radial falloff in alpha
	u32 m_frameIndex = 0;
};

} // namespace dungeon::gfx
