// ============================================================================
// Graphics/ModelPreview.h — an offscreen 3D thumbnail of a single model.
//
// A square render target that draws one mesh with the scene PBR shader (an
// orbit-able camera + one fill light), then exposes itself as an SRV the 2D
// pass can blit — the editor's asset preview pane. Render() runs mid-frame
// (after GraphicsDevice::BeginFrame, before the SpriteBatch pass); it redirects
// the output merger, so the caller rebinds the back buffer afterwards
// (GraphicsDevice::BindBackBuffer). Modeled on the renderer's shadow-cube
// targets (committed RT + depth + RTV/DSV heaps + a shader-visible SRV).
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"
#include "Graphics/Mesh.h"
#include "Graphics/ParticleBatch.h" // optional preview particles (ParticleInstance)
#include "Graphics/Renderer.h"      // MaterialParams

#include <span>

namespace dungeon::gfx {

class Camera;

// Binds rtv+dsv, sets the viewport/scissor to a `size`x`size` square, and clears
// both — the shared offscreen-pass setup for the editor's ModelPreview and the
// item-icon bake (DungeonWorld). The caller owns the RT/depth resource barriers
// and issues BeginScene + draws afterward.
void BeginOffscreen(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
					D3D12_CPU_DESCRIPTOR_HANDLE dsv, u32 size, const float clear[4]);

// One drawable piece of a preview: a mesh + its material. A simple model is one
// of these; an authored multi-material model (weapon, boulder, ...) is several.
struct PreviewSubmesh {
	const Mesh* mesh = nullptr;
	MaterialParams material;
};

class ModelPreview {
public:
	ModelPreview(GraphicsDevice& device, u32 size = 512);
	~ModelPreview() { m_device.FreeSrv(m_srv.index); }

	// Renders the submeshes into the offscreen target, uniformly `scale`d and spun
	// by `orbit` radians about +Y (all share one world transform). `aspect`
	// (width/height of the pane the image is blitted into) sets the camera aspect so
	// a non-square pane doesn't distort the model. `palette` is empty for a static
	// mesh or the skinning palette for an animated one (anim::Animator::Palette()).
	// Optionally draws `billboards` (via `particles`) with the SAME camera after the
	// meshes — for the torch preview's flame/smoke; caller must have called
	// particles->NewFrame() this frame.
	// When `fitMin`/`fitMax` (a model-space AABB) are both given, the model is scaled
	// to fill the pane and centred on that box (for small/loose props); `scale` then
	// multiplies the fit. Otherwise it is drawn grounded head-on at `scale`. `orbit`
	// spins it either way (pass a time-varying angle to revolve).
	// `orient` (auto-fit only): a rotation applied to the centred model before the
	// spin, which is then about +Y ALONE - a turntable. Without it the fit tumbles
	// on two axes, so a flat prop is never edge-on for long (the editor's look).
	// `pivot` (grounded view only): the model-space point whose XZ the model
	// stands and turns on, in place of the origin - a rig that rests off its
	// file's origin (the skeleton kit) passes its root joint's rest position, so
	// it is centred in the pane as it is on its square (DungeonWorld::
	// MonsterModelWorld). Its Y is ignored: the model stays grounded.
	void Render(ID3D12GraphicsCommandList* list, Renderer& renderer,
				std::span<const PreviewSubmesh> subs, float scale, float orbit,
				float aspect = 1.0f, std::span<const Mat4> palette = {},
				ParticleBatch* particles = nullptr,
				std::span<const ParticleInstance> billboards = {},
				const Vec3* fitMin = nullptr, const Vec3* fitMax = nullptr,
				const Mat4* orient = nullptr, const Vec3* pivot = nullptr);
	// Convenience single-mesh overload (delegates to the span version).
	void Render(ID3D12GraphicsCommandList* list, Renderer& renderer, const Mesh& mesh,
				const MaterialParams& material, float scale, float orbit,
				float aspect = 1.0f, std::span<const Mat4> palette = {},
				ParticleBatch* particles = nullptr,
				std::span<const ParticleInstance> billboards = {},
				const Vec3* pivot = nullptr);

	// Where model-space point `p` lands in the image (uv, 0..1 from the top-left)
	// when Render is given the same scale / orbit / aspect / fit / orient - the
	// one framing both use, so an overlay drawn there sits on the model (the
	// details dialog's flame on a torch head). False behind the camera.
	static bool Project(const Vec3& p, float scale, float orbit, float aspect,
						const Vec3* fitMin, const Vec3* fitMax, const Mat4* orient,
						Vec2& uv);

	// The rendered image's SRV, for SpriteBatch::DrawSprite.
	D3D12_GPU_DESCRIPTOR_HANDLE Srv() const { return m_srv.gpu; }
	u32 Size() const { return m_size; }

private:
	static Camera PreviewCamera(float aspect);
	static Mat4 PreviewWorld(float scale, float orbit, const Vec3* fitMin, const Vec3* fitMax,
							 const Mat4* orient, const Vec3* pivot = nullptr);

	GraphicsDevice& m_device;
	u32 m_size;
	ComPtr<ID3D12Resource> m_color;
	ComPtr<ID3D12Resource> m_depth;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	SrvHandle m_srv;
	LightSet m_lights; // the fixed studio rig, built once (see the constructor)
};

} // namespace dungeon::gfx
