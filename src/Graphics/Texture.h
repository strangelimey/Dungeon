#pragma once

#include "Assets/Image.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/GraphicsDevice.h"

namespace dungeon::gfx {

// GPU texture (RGBA8) with an SRV in the device's shader-visible heap.
class Texture {
public:
    Texture(GraphicsDevice& device, const assets::ImageData& image);

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle() const { return m_srv.gpu; }
    u32 Width() const { return m_width; }
    u32 Height() const { return m_height; }

private:
    ComPtr<ID3D12Resource> m_resource;
    SrvHandle m_srv;
    u32 m_width = 0;
    u32 m_height = 0;
};

} // namespace dungeon::gfx
