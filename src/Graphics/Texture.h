// ============================================================================
// Graphics/Texture.h — immutable GPU texture.
//
// Construction does everything: builds a CPU mip chain (box filter), uploads
// all levels through a transient staging buffer, and creates an SRV in the
// device's shader-visible heap. Upload blocks until the GPU copy finishes
// (load-time only — never construct one mid-frame). Always RGBA8; for
// normal+height maps the alpha channel carries the parallax height field.
// ============================================================================
#pragma once

#include "Assets/Image.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/GraphicsDevice.h"

namespace dungeon::gfx {
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
