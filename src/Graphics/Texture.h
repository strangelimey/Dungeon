// ============================================================================
// Graphics/Texture.h — immutable GPU texture.
//
// Two construction paths: from a single image (a CPU box-filter mip chain is
// built on the spot — fine for small runtime-generated textures) or from a
// pre-baked assets::MipChain (DDS from AssetBaker — no filtering at load).
// Either way the upload goes through a transient staging buffer and blocks
// until the GPU copy finishes (load-time only — never construct one
// mid-frame). Always RGBA8; for normal+height maps the alpha channel carries
// the parallax height field.
//
// The destructor returns the SRV slot to the device's free list
// (GraphicsDevice::FreeSrv), so texture churn — font atlas rebakes, level
// transitions, quality swaps — recycles the shader-visible heap instead of
// exhausting it. The GraphicsDevice must therefore outlive every Texture.
// ============================================================================
#pragma once

#include "Assets/Image.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/GraphicsDevice.h"

#include <memory>
#include <vector>

namespace dungeon::gfx {
class Texture {
public:
	// `srgb` selects an sRGB view (gamma-decoded on sample) — set it for color
	// (albedo) maps; leave it false for linear data (normal, height, ORM).
	Texture(GraphicsDevice& device, const assets::ImageData& image, bool srgb = false);
	Texture(GraphicsDevice& device, const assets::MipChain& mips, bool srgb = false);
	~Texture(); // returns the SRV slot to the device's free list

	// Non-copyable/movable: the destructor frees the SRV slot, and every user
	// holds Textures behind unique_ptr anyway.
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;

	// A render-target-backed texture: an RGBA8 ALLOW_RENDER_TARGET surface born in
	// SRV state, with a transparent (0,0,0,0) clear. Render into it via Rtv() (then
	// barrier back to PIXEL_SHADER_RESOURCE) and sample it like any other Texture
	// (GpuHandle()). Used for baked 3D item-icon thumbnails.
	static std::unique_ptr<Texture> RenderTarget(GraphicsDevice& device, u32 size);

	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle() const { return m_srv.gpu; }
	D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const {
		return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	}
	ID3D12Resource* Resource() const { return m_resource.Get(); }
	u32 Width() const { return m_width; }
	u32 Height() const { return m_height; }

private:
	Texture() = default; // for RenderTarget()
	void Upload(GraphicsDevice& device, const assets::MipChain& chain, bool srgb);

	GraphicsDevice* m_device = nullptr; // for FreeSrv in the destructor
	ComPtr<ID3D12Resource> m_resource;
	SrvHandle m_srv;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap; // only set for RenderTarget() textures
	u32 m_width = 0;
	u32 m_height = 0;
};

} // namespace dungeon::gfx
