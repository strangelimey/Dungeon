// ============================================================================
// Graphics/Renderer.h — the forward 3D scene pass.
//
// One root signature, one PSO. Per frame: BeginScene writes the camera +
// light constants once, then each DrawMesh allocates its object constants
// (and skinning palette, if any) from the frame's UploadAllocator arena and
// issues one draw. Shading lives in assets/shaders/scene.hlsl (compiled at
// startup — edit the .hlsl and relaunch, no C++ rebuild needed).
//
// Root signature layout (must match scene.hlsl):
//   0  b0  frame constants   (root CBV — camera, ambient, lights)
//   1  b1  object constants  (root CBV — world, color, flags, height scale)
//   2  b2  skinning palette  (root CBV — kMaxSkinJoints matrices)
//   3  t0  base color texture     (descriptor table)
//   4  t1  normal+height map      (descriptor table; A = height for parallax)
//   s0     static anisotropic wrap sampler
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Graphics/Camera.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Lights.h"
#include "Graphics/Mesh.h"
#include "Graphics/Texture.h"
#include "Graphics/UploadAllocator.h"

#include <memory>
#include <span>

namespace dungeon::gfx {

// Hard cap on joints per skinned mesh; must match MAX_SKIN_JOINTS in
// scene.hlsl (the palette is a fixed-size cbuffer).
inline constexpr u32 kMaxSkinJoints = 128;

// Everything material-related for one draw. Textures may be null (baseColor
// only); `normalMap` (xyz = tangent-space normal, w = height) enables bump
// mapping, and `heightScale` > 0 adds parallax on top. Specular defaults suit
// dry stone; raise strength for wet/glossy things (slime, polished metal).
struct MaterialParams {
	const Texture* albedo = nullptr;
	const Texture* normalMap = nullptr;
	Vec4 baseColor{1, 1, 1, 1};
	float heightScale = 0.0f;
	float specStrength = 0.05f;
	float specPower = 24.0f;
};

// Forward 3D pass: one pipeline, per-frame light constants, optional texture
// and optional GPU skinning per draw.
class Renderer {
public:
	explicit Renderer(GraphicsDevice& device);

	// Writes the per-frame constants and binds the scene pipeline.
	void BeginScene(ID3D12GraphicsCommandList* list, const Camera& camera,
					const LightSet& lights);

	// Draws a mesh; `palette` is empty for static meshes or the skinning
	// palette for skinned ones.
	void DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh, const Mat4& world,
				  const MaterialParams& material, std::span<const Mat4> palette = {});

	// Call when the device frame index advances (resets that frame's allocator).
	void NewFrame(u32 frameIndex);

private:
	GraphicsDevice& m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
	std::unique_ptr<Texture> m_whiteTexture;
	std::unique_ptr<Texture> m_flatNormalMap;
	u32 m_frameIndex = 0;
};

} // namespace dungeon::gfx
