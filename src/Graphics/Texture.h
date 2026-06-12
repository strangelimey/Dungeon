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
// ============================================================================
#pragma once

#include "Assets/Image.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/GraphicsDevice.h"

#include <vector>

namespace dungeon::gfx {
class Texture {
public:
	Texture(GraphicsDevice& device, const assets::ImageData& image);
	Texture(GraphicsDevice& device, const assets::MipChain& mips);

	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle() const { return m_srv.gpu; }
	u32 Width() const { return m_width; }
	u32 Height() const { return m_height; }

private:
	void Upload(GraphicsDevice& device, const std::vector<assets::ImageData>& mips);

	ComPtr<ID3D12Resource> m_resource;
	SrvHandle m_srv;
	u32 m_width = 0;
	u32 m_height = 0;
};

} // namespace dungeon::gfx
