#include "Graphics/Texture.h"

#include <cstring>

namespace dungeon::gfx {

Texture::Texture(GraphicsDevice& device, const assets::ImageData& image)
    : m_width(image.width), m_height(image.height) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = image.width;
    desc.Height = image.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    DN_HR(device.Device()->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_resource)));

    // Staging upload with 256-byte row alignment.
    const u32 rowPitch =
        (image.width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const u64 stagingSize = static_cast<u64>(rowPitch) * image.height;

    ComPtr<ID3D12Resource> staging;
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC stagingDesc = BufferDesc(stagingSize);
    DN_HR(device.Device()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));

    u8* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    DN_HR(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
    for (u32 y = 0; y < image.height; ++y)
        std::memcpy(mapped + static_cast<size_t>(y) * rowPitch,
                    image.pixels.data() + static_cast<size_t>(y) * image.width * 4,
                    static_cast<size_t>(image.width) * 4);
    staging->Unmap(0, nullptr);

    device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = image.width;
        src.PlacedFootprint.Footprint.Height = image.height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = rowPitch;

        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const auto barrier =
            Transition(m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
    });

    m_srv = device.AllocateSrv();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device.Device()->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srv.cpu);
}

} // namespace dungeon::gfx
