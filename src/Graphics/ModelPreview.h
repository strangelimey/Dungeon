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
#include "Graphics/Renderer.h" // MaterialParams

#include <span>

namespace dungeon::gfx {

// Binds rtv+dsv, sets the viewport/scissor to a `size`x`size` square, and clears
// both — the shared offscreen-pass setup for the editor's ModelPreview and the
// item-icon bake (DungeonWorld). The caller owns the RT/depth resource barriers
// and issues BeginScene + draws afterward.
void BeginOffscreen(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
					D3D12_CPU_DESCRIPTOR_HANDLE dsv, u32 size, const float clear[4]);

class ModelPreview {
public:
	ModelPreview(GraphicsDevice& device, u32 size = 512);

	// Renders `mesh`/`material` into the offscreen target, uniformly `scale`d and
	// spun by `orbit` radians about +Y. `aspect` (width/height of the pane the image
	// is blitted into) sets the camera aspect so a non-square pane doesn't distort
	// the model — the square RT is rendered for that aspect and the blit cancels it.
	// `palette` is empty for a static mesh or the skinning palette for an animated
	// one (an anim::Animator's Palette()). Uses `renderer` for the actual draw.
	void Render(ID3D12GraphicsCommandList* list, Renderer& renderer, const Mesh& mesh,
				const MaterialParams& material, float scale, float orbit,
				float aspect = 1.0f, std::span<const Mat4> palette = {});

	// The rendered image's SRV, for SpriteBatch::DrawSprite.
	D3D12_GPU_DESCRIPTOR_HANDLE Srv() const { return m_srv.gpu; }
	u32 Size() const { return m_size; }

private:
	GraphicsDevice& m_device;
	u32 m_size;
	ComPtr<ID3D12Resource> m_color;
	ComPtr<ID3D12Resource> m_depth;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	SrvHandle m_srv;
};

} // namespace dungeon::gfx
