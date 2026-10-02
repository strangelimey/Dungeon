#include "Graphics/Texture.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dungeon::gfx {

using assets::Downsample; // the shared 2x2 box filter (Assets/Image.h)

Texture::Texture(GraphicsDevice& device, const assets::ImageData& image, bool srgb) {
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
	Upload(device, chain, srgb);
}

Texture::Texture(GraphicsDevice& device, const assets::MipChain& chain, bool srgb) {
	DN_ASSERT(!chain.levels.empty(), "empty mip chain");
	Upload(device, chain, srgb);
}

Texture::~Texture() {
	if (m_device) m_device->FreeSrv(m_srv.index);
}

std::unique_ptr<Texture> Texture::RenderTarget(GraphicsDevice& device, u32 size) {
	auto t = std::unique_ptr<Texture>(new Texture());
	t->m_width = t->m_height = size;
	ID3D12Device* d = device.Device();

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = size;
	desc.Height = size;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = kBackBufferFormat;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE clear{}; // transparent (0,0,0,0) so the icon sits on its slot
	clear.Format = kBackBufferFormat;
	const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	DN_HR(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
									 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
									 IID_PPV_ARGS(&t->m_resource)));

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = 1;
	DN_HR(d->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&t->m_rtvHeap)));
	d->CreateRenderTargetView(t->m_resource.Get(), nullptr,
							  t->m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

	// AllocateSrv may hand back a recycled slot whose old descriptor is still
	// referenced by in-flight frames; drain the GPU before overwriting it (the
	// Upload path gets this for free from ExecuteImmediate). RenderTarget
	// creation is rare (editor open / asset dialog), so the stall is fine.
	device.WaitIdle();
	t->m_device = &device;
	t->m_srv = device.AllocateSrv();
	d->CreateShaderResourceView(t->m_resource.Get(), nullptr, t->m_srv.cpu);
	return t;
}

void Texture::Upload(GraphicsDevice& device, const assets::MipChain& chain, bool srgb) {
	m_width = chain.width;
	m_height = chain.height;
	const u32 mipCount = static_cast<u32>(chain.levels.size());
	const bool bc7 = chain.format == assets::TextureFormat::Bc7;
	const DXGI_FORMAT format =
		bc7 ? (srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM)
			: (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);

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

	// Safe to overwrite a recycled slot's descriptor here: ExecuteImmediate
	// above blocked until the GPU went idle.
	m_device = &device;
	m_srv = device.AllocateSrv();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = mipCount;
	device.Device()->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srv.cpu);
}

} // namespace dungeon::gfx
