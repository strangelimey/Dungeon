// ============================================================================
// Graphics/Renderer.h — the forward 3D scene pass + point-light shadow pass.
//
// One root signature shared by two PSOs (scene and shadow). Per frame: the
// shadow pass renders cube distance maps for the slotted lights, then
// BeginScene writes the camera + light constants once and each DrawMesh
// allocates its object constants (and skinning palette, if any) from the
// frame's UploadAllocator arena and issues one draw. Shading lives in
// assets/shaders/scene.hlsl and shadow.hlsl (compiled at startup — edit the
// .hlsl and relaunch, no C++ rebuild needed).
//
// Root signature layout (must match scene.hlsl / shadow.hlsl):
//   0  b0  frame constants   (root CBV — camera, ambient, lights, fog)
//   1  b1  object constants  (root CBV — world, color, flags, height scale)
//   2  b2  skinning palette  (root CBV — kMaxSkinJoints matrices)
//   3  t0  base color texture     (descriptor table)
//   4  t1  normal+height map      (descriptor table; A = height for parallax)
//   5  t2  air turbidity grid     (descriptor table; dust raymarch density)
//   6  t3..t6  shadow cubes       (one table, kShadowSlots contiguous SRVs)
//   s0     static anisotropic wrap sampler
//   s1     static clamped bilinear sampler (turbidity grid + shadow cubes)
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

// Point-light shadow cube slots, resolution falling off with slot index —
// the game assigns slot 0 to the light nearest the camera.
inline constexpr u32 kShadowSlots = 4;
inline constexpr u32 kShadowResolution[kShadowSlots] = {512, 256, 256, 128};

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
					const LightSet& lights, const Atmosphere& atmosphere = {});

	// --- shadow pass ---------------------------------------------------------
	// Renders cube distance maps before the scene pass. For each light that
	// holds a shadow slot, call BeginShadowFace for faces 0..5 and submit the
	// scene geometry between calls (DrawMesh works unchanged); finish with
	// EndShadows, then rebind the back buffer before BeginScene.
	void BeginShadowFace(ID3D12GraphicsCommandList* list, u32 slot, u32 face,
						 const Vec3& lightPos, float radius);
	void EndShadows(ID3D12GraphicsCommandList* list);

	// Draws a mesh; `palette` is empty for static meshes or the skinning
	// palette for skinned ones.
	void DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh, const Mat4& world,
				  const MaterialParams& material, std::span<const Mat4> palette = {});

	// Call when the device frame index advances (resets that frame's allocator).
	void NewFrame(u32 frameIndex);

private:
	void CreateShadowResources();

	GraphicsDevice& m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	ComPtr<ID3D12PipelineState> m_shadowPso;
	std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
	std::unique_ptr<Texture> m_whiteTexture;
	std::unique_ptr<Texture> m_flatNormalMap;
	std::unique_ptr<Texture> m_blackTexture; // "clear air" turbidity fallback

	// Shadow cube targets (R16_FLOAT distance) + shared per-slot depth.
	ComPtr<ID3D12Resource> m_shadowCube[kShadowSlots];
	ComPtr<ID3D12Resource> m_shadowDepth[kShadowSlots];
	ComPtr<ID3D12DescriptorHeap> m_shadowRtvHeap; // kShadowSlots * 6 faces
	ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap; // one per slot
	SrvHandle m_shadowSrv[kShadowSlots];          // contiguous (one root table)
	bool m_shadowInRtState[kShadowSlots]{};

	u32 m_frameIndex = 0;
};

} // namespace dungeon::gfx
