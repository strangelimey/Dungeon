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
//   6  t3..t10 shadow cubes       (one table, kShadowSlots contiguous SRVs)
//   7  t11 occlusion/rough/metal  (descriptor table; ORM map for PBR)
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
#include <utility>
#include <vector>

namespace dungeon::gfx {

// Hard cap on joints per skinned mesh; must match MAX_SKIN_JOINTS in
// scene.hlsl (the palette is a fixed-size cbuffer).
inline constexpr u32 kMaxSkinJoints = 128;

// Point-light shadow cube slots, resolution falling off with slot index —
// the game assigns slot 0 to the light nearest the camera. Eight slots so a
// sconce-lined hall keeps most of its fires shadow-casting (with four, the
// carried torch + three nearest fires monopolized the cubes and every other
// light was pure fill, washing the shadows out); the ShadowSlotCache +
// half-rate fire flicker keep the extra cubes' re-render cost down.
inline constexpr u32 kShadowSlots = 8;
inline constexpr u32 kShadowResolution[kShadowSlots] = {512, 256, 256, 256,
														128, 128, 128, 128};

// Everything material-related for one draw. Textures may be null (baseColor
// only); `normalMap` (xyz = tangent-space normal, w = height) enables bump
// mapping, and `heightScale` > 0 adds parallax on top. `metalRough` is the ORM
// map (R = occlusion, G = roughness, B = metallic) and, when set, scales the
// `metallic`/`roughness` factors per-texel. Defaults suit dry matte stone;
// lower roughness for wet/polished things, metallic = 1 for bare metal.
// `doubleSided` = true keeps CULL_MODE_NONE (hand-built procedural geometry
// that may have inconsistent winding); authored, consistently-wound meshes set
// it false to enable back-face culling.
struct MaterialParams {
	const Texture* albedo = nullptr;
	const Texture* normalMap = nullptr;
	const Texture* metalRough = nullptr;
	Vec4 baseColor{1, 1, 1, 1};
	Vec3 emissive{0, 0, 0}; // additive self-lit glow (runes pulse; 0 = no glow)
	// > 0: the emissive lights a rune tablet's CARVED GROOVE (crisp, plus a soft
	// halo round it) at this strength, instead of the floor's rim aura. The
	// groove is read from the rune set's occlusion map (RuneBaker writes
	// 1 - 0.45 x carve there), so it means something only on a rune set.
	float emissiveGroove = 0.0f;
	float heightScale = 0.0f;
	float metallic = 0.0f;
	float roughness = 0.9f;
	// Alpha-test cutout threshold; 0 = opaque (default). > 0 discards texels
	// whose albedo alpha is below it — the gaps in a masked set (e.g. wood
	// planks) read as empty space. Uses the opaque PSO; no blending/sorting.
	float alphaCutoff = 0.0f;
	bool doubleSided = true;
	// See-through (glass). The draw is QUEUED, not issued: FlushTransparent
	// draws the queue after the opaque scene, farthest first, each entry twice
	// (back faces, then front), with no depth write. It FILTERS what is behind
	// it (dual-source blending): albedo x baseColor RGB is the tint (white =
	// clear) and its alpha the density, the same at every angle; the glass adds
	// only its specular, a little scatter and a faint sheen at grazing edges.
	// Casts no shadow.
	bool transparent = false;
	// A LIQUID inside glass (Game/Liquid.h): a transparent draw clipped at the
	// plane y = liquidLevel in the mesh's OWN space (the shader gets it in
	// world space, so it tilts with the bottle), whose back faces seen through
	// the cut are lit as the flat surface. FlushTransparent draws it between its
	// container's far and near walls.
	bool liquid = false;
	float liquidLevel = 0.0f;
};

// What the transparent queue did, for the `glass` dev command.
struct TransparentStats {
	u32 queued = 0;    // draws the main scene flushed last frame
	u32 frames = 0;    // main-scene frames that drew any glass, ever (AllocTest -Glass)
	u32 peak = 0;      // most ever queued between two flushes
	u32 overflows = 0; // draws past the queue's capacity, drawn at once unsorted
	u32 dropped = 0;   // draws a pass queued and never flushed (a missing flush)
};

// Forward 3D pass: one pipeline, per-frame light constants, optional texture
// and optional GPU skinning per draw.
class Renderer {
public:
	explicit Renderer(GraphicsDevice& device);

	// Writes the per-frame constants and binds the scene pipeline. hdrTarget
	// selects the kSceneColorFormat PSOs and turns OFF the shader's inline
	// tonemap — the main scene pass renders linear HDR into the PostProcess
	// target and tonemaps in the post composite. LDR passes (model previews,
	// icon bakes) keep the default and render tonemapped into their UNORM RTs.
	void BeginScene(ID3D12GraphicsCommandList* list, const Camera& camera,
					const LightSet& lights, const Atmosphere& atmosphere = {},
					bool hdrTarget = false);

	// --- shadow pass ---------------------------------------------------------
	// Renders cube distance maps before the scene pass. For each light that
	// holds a shadow slot, call BeginShadowFace for faces 0..5 and submit the
	// scene geometry between calls (DrawMesh works unchanged); finish with
	// EndShadows, then rebind the back buffer before BeginScene.
	void BeginShadowFace(ID3D12GraphicsCommandList* list, u32 slot, u32 face,
						 const Vec3& lightPos, float radius);
	void EndShadows(ID3D12GraphicsCommandList* list);

	// Draws a mesh; `palette` is empty for static meshes or the skinning
	// palette for skinned ones. A `transparent` material is queued instead (and
	// skipped outright in the shadow pass) - see FlushTransparent.
	void DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh, const Mat4& world,
				  const MaterialParams& material, std::span<const Mat4> palette = {});

	// Draws every transparent mesh queued since BeginScene, farthest from the
	// camera first. EVERY pass that may draw one calls this at its end, with its
	// own target still bound: the main scene (before the particles), the icon
	// bakes and the model preview. A pass that forgets loses its glass - the next
	// BeginScene drops the leftovers and counts them in Stats().dropped.
	void FlushTransparent(ID3D12GraphicsCommandList* list);
	const TransparentStats& Stats() const { return m_transparentStats; }

	// Call when the device frame index advances (resets that frame's allocator).
	void NewFrame(u32 frameIndex);

	// Rewrites a single-mip RGBA8 texture's pixels in place, through this frame's
	// upload arena: records PIXEL_SHADER_RESOURCE -> COPY_DEST, the copy, and back,
	// on `list`. The mid-frame alternative to constructing a new Texture (which
	// allocates, takes a fresh SRV slot and drains the GPU). `rgba8` is tightly
	// packed, Width() * 4 bytes a row. Record it before any draw that samples the
	// texture this frame.
	void UpdateTexture(ID3D12GraphicsCommandList* list, const Texture& texture,
					   std::span<const u8> rgba8);

private:
	void CreateShadowResources();
	// Uploads the object constants (and binds the palette + textures) for one
	// draw, then issues it with whatever PSO is bound.
	void IssueDraw(ID3D12GraphicsCommandList* list, const Mesh& mesh, const Mat4& world,
				   const MaterialParams& material, D3D12_GPU_VIRTUAL_ADDRESS paletteVa);
	// The skinning palette's GPU address this frame (uploaded once, cached).
	D3D12_GPU_VIRTUAL_ADDRESS UploadPalette(std::span<const Mat4> palette);

	GraphicsDevice& m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;       // scene, CULL_NONE (double-sided)
	ComPtr<ID3D12PipelineState> m_psoCull;   // scene, CULL_BACK (authored meshes)
	// Same pair targeting kSceneColorFormat (the PostProcess HDR intermediate).
	ComPtr<ID3D12PipelineState> m_psoHdr;
	ComPtr<ID3D12PipelineState> m_psoCullHdr;
	// Transparent: premultiplied blend, depth test without write. [hdr][cull]
	// where cull 0 = front faces culled (draws the BACK faces), 1 = back culled.
	ComPtr<ID3D12PipelineState> m_psoGlass[2][2];
	ComPtr<ID3D12PipelineState> m_shadowPso;
	std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
	std::unique_ptr<Texture> m_whiteTexture;
	std::unique_ptr<Texture> m_flatNormalMap;
	std::unique_ptr<Texture> m_blackTexture;   // "clear air" turbidity fallback
	std::unique_ptr<Texture> m_defaultMRTexture; // AO=1, rough=1, metal=0
	bool m_shadowPass = false;                 // DrawMesh skips PSO swap in shadow pass
	bool m_hdrPass = false;                    // DrawMesh picks the HDR PSO pair
	ID3D12PipelineState* m_currentPso = nullptr; // bound PSO, to skip redundant swaps
	// Skinning palettes uploaded once per frame: a skinned mesh is re-submitted
	// up to 25x (shadow faces + scene) with the same pose, so cache the upload
	// keyed by the animator's palette buffer and reuse the GPU address.
	// Distinct skinned meshes one frame is expected to show. A FLOOR for the
	// reserve in NewFrame, not a limit — every monster on screen at once, with room.
	static constexpr size_t kPaletteCacheReserve = 64;
	std::vector<std::pair<const void*, D3D12_GPU_VIRTUAL_ADDRESS>> m_paletteCache;

	// The transparent queue: FIXED capacity, reserved in the constructor, so
	// queueing and sorting allocate nothing (std::sort on it does not either).
	// The palette rides as its uploaded GPU address, not a span - an icon bake's
	// rest-pose animator is a temporary that is gone by the flush.
	struct QueuedDraw {
		const Mesh* mesh = nullptr;
		Mat4 world;
		MaterialParams material;
		D3D12_GPU_VIRTUAL_ADDRESS paletteVa = 0;
		float distance = 0.0f; // squared, camera to the draw's origin
	};
	static constexpr size_t kTransparentCapacity = 256;
	std::vector<QueuedDraw> m_transparent;     // capacity kTransparentCapacity, never grown
	std::vector<u16> m_transparentOrder;       // sort scratch, same capacity
	Vec3 m_cameraPos{};                        // the current pass's eye, for the sort
	TransparentStats m_transparentStats;

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
