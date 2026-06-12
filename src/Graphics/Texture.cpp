#include "Graphics/Texture.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dungeon::gfx {

namespace {

// 2x2 box-filter downsample (RGBA8). Good enough for albedo and for normal
// maps too, since the shader renormalizes after sampling.
assets::ImageData Downsample(const assets::ImageData& src) {
	assets::ImageData dst;
	dst.width = std::max(1u, src.width / 2);
	dst.height = std::max(1u, src.height / 2);
	dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * 4);
	for (u32 y = 0; y < dst.height; ++y) {
		for (u32 x = 0; x < dst.width; ++x) {
			const u32 sx = std::min(x * 2, src.width - 1);
			const u32 sy = std::min(y * 2, src.height - 1);
			const u32 sx1 = std::min(sx + 1, src.width - 1);
			const u32 sy1 = std::min(sy + 1, src.height - 1);
			for (u32 c = 0; c < 4; ++c) {
				const u32 sum =
					src.pixels[(static_cast<size_t>(sy) * src.width + sx) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy) * src.width + sx1) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy1) * src.width + sx) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy1) * src.width + sx1) * 4 + c];
				dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4 + c] =
					static_cast<u8>(sum / 4);
			}
		}
	}
	return dst;
}

} // namespace

Texture::Texture(GraphicsDevice& device, const assets::ImageData& image) {
	// Build the full CPU mip chain on the spot (runtime-generated textures;
	// file textures arrive pre-mipped and BC7-compressed via the MipChain
	// constructor).
	assets::MipChain chain;
	chain.width = image.width;
	chain.height = image.height;
	chain.format = assets::TextureFormat::Rgba8;

	assets::ImageData level = image;
	while (true) {
		assets::TextureLevel out;
		out.width = level.width;
		out.height = level.height;
		const bool last = level.width == 1 && level.height == 1;
		assets::ImageData next;
		if (!last) next = Downsample(level);
		out.data = std::move(level.pixels);
		chain.levels.push_back(std::move(out));
		if (last) break;
		level = std::move(next);
	}
	Upload(device, chain);
}

Texture::Texture(GraphicsDevice& device, const assets::MipChain& chain) {
	DN_ASSERT(!chain.levels.empty(), "empty mip chain");
	Upload(device, chain);
}

void Texture::Upload(GraphicsDevice& device, const assets::MipChain& chain) {
	m_width = chain.width;
	m_height = chain.height;
	const u32 mipCount = static_cast<u32>(chain.levels.size());
	const DXGI_FORMAT format = chain.format == assets::TextureFormat::Bc7
								   ? DXGI_FORMAT_BC7_UNORM
								   : DXGI_FORMAT_R8G8B8A8_UNORM;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = m_width;
	desc.Height = m_height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = static_cast<UINT16>(mipCount);
	desc.Format = format;
	desc.SampleDesc.Count = 1;

	const D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	DN_HR(device.Device()->CreateCommittedResource(
		&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr, IID_PPV_ARGS(&m_resource)));

	// One staging buffer holding every mip at its required alignment. The
	// level data is tightly packed, so each source row is exactly
	// rowSizes[m] bytes (for BC7 a "row" is a row of 4x4 blocks).
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
	std::vector<UINT> rowCounts(mipCount);
	std::vector<UINT64> rowSizes(mipCount);
	UINT64 totalBytes = 0;
	device.Device()->GetCopyableFootprints(&desc, 0, mipCount, 0, footprints.data(),
										   rowCounts.data(), rowSizes.data(),
										   &totalBytes);

	ComPtr<ID3D12Resource> staging;
	const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC stagingDesc = BufferDesc(totalBytes);
	DN_HR(device.Device()->CreateCommittedResource(
		&uploadHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));

	u8* mapped = nullptr;
	const D3D12_RANGE noRead{0, 0};
	DN_HR(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
	for (u32 m = 0; m < mipCount; ++m) {
		const auto& fp = footprints[m];
		const assets::TextureLevel& level = chain.levels[m];
		DN_ASSERT(level.data.size() >= rowSizes[m] * rowCounts[m],
				  "mip level smaller than its footprint");
		for (u32 y = 0; y < rowCounts[m]; ++y)
			std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
						level.data.data() + static_cast<size_t>(y) * rowSizes[m],
						static_cast<size_t>(rowSizes[m]));
	}
	staging->Unmap(0, nullptr);

	device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
		for (u32 m = 0; m < mipCount; ++m) {
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = m_resource.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = m;

			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = staging.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = footprints[m];

			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}
		const auto barrier =
			Transition(m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
					   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &barrier);
	});

	m_srv = device.AllocateSrv();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = mipCount;
	device.Device()->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srv.cpu);
}

} // namespace dungeon::gfx
