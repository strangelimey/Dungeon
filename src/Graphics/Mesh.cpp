#include "Graphics/Mesh.h"

#include "Graphics/GraphicsDevice.h"

#include <cstring>

namespace dungeon::gfx {

namespace {

// Every vertex and index range starts on this boundary inside its block. D3D12
// asks less (4 bytes for either view); a round number costs nothing that shows
// and keeps a range from ever straddling a cache line it shares with another.
constexpr u64 kAlign = 256;
// Largest shared buffer. Bounds the transient staging buffer as well, since each
// block uploads through one of its own; a mesh bigger than this gets a block to
// itself. A level's chunks are ~110 MB (eval_arena, 28x24), so two blocks.
constexpr u64 kBlockBytes = 64ull << 20;

u64 AlignUp(u64 v) { return (v + kAlign - 1) & ~(kAlign - 1); }

// Where one mesh landed: its block and the byte offsets of its two ranges.
struct Placement {
	size_t block = 0;
	u64 vb = 0, ib = 0;
};

} // namespace

Mesh::Mesh(GraphicsDevice& device, const assets::MeshData& data) {
	const assets::MeshData* one[] = {&data};
	*this = std::move(*CreateMeshes(device, one)[0]);
}

std::vector<std::unique_ptr<Mesh>> CreateMeshes(
	GraphicsDevice& device, std::span<const assets::MeshData* const> meshes) {
	// Lay the meshes out: vertices then indices, each aligned, packed into blocks.
	std::vector<Placement> at(meshes.size());
	std::vector<u64> blockSize{0};
	for (size_t i = 0; i < meshes.size(); ++i) {
		const u64 need = AlignUp(meshes[i]->vertices.size() * sizeof(assets::Vertex)) +
						 AlignUp(meshes[i]->indices.size() * sizeof(u32));
		if (blockSize.back() > 0 && blockSize.back() + need > kBlockBytes)
			blockSize.push_back(0);
		at[i].block = blockSize.size() - 1;
		at[i].vb = blockSize.back();
		at[i].ib = blockSize.back() +
				   AlignUp(meshes[i]->vertices.size() * sizeof(assets::Vertex));
		blockSize.back() += need;
	}

	std::vector<std::unique_ptr<Mesh>> out(meshes.size());
	for (auto& m : out) m.reset(new Mesh());
	const D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
	// A block is read as vertices AND indices, so it rests in both read states
	// at once (a legal combination; neither is a write state).
	constexpr D3D12_RESOURCE_STATES kReadState =
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;

	for (size_t b = 0; b < blockSize.size(); ++b) {
		if (blockSize[b] == 0) continue; // only empty meshes: nothing to upload
		const D3D12_RESOURCE_DESC desc = BufferDesc(blockSize[b]);
		ComPtr<ID3D12Resource> block, staging;
		DN_HR(device.Device()->CreateCommittedResource(
			&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
			nullptr, IID_PPV_ARGS(&block)));
		DN_HR(device.Device()->CreateCommittedResource(
			&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(&staging)));

		u8* mapped = nullptr;
		const D3D12_RANGE noRead{0, 0};
		DN_HR(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
		const D3D12_GPU_VIRTUAL_ADDRESS base = block->GetGPUVirtualAddress();
		for (size_t i = 0; i < meshes.size(); ++i) {
			if (at[i].block != b) continue;
			const assets::MeshData& data = *meshes[i];
			const u64 vbSize = data.vertices.size() * sizeof(assets::Vertex);
			const u64 ibSize = data.indices.size() * sizeof(u32);
			if (vbSize) std::memcpy(mapped + at[i].vb, data.vertices.data(), vbSize);
			if (ibSize) std::memcpy(mapped + at[i].ib, data.indices.data(), ibSize);

			Mesh& mesh = *out[i];
			mesh.m_buffer = block;
			mesh.m_indexCount = static_cast<u32>(data.indices.size());
			mesh.m_vbv.BufferLocation = base + at[i].vb;
			mesh.m_vbv.SizeInBytes = static_cast<UINT>(vbSize);
			mesh.m_vbv.StrideInBytes = sizeof(assets::Vertex);
			mesh.m_ibv.BufferLocation = base + at[i].ib;
			mesh.m_ibv.SizeInBytes = static_cast<UINT>(ibSize);
			mesh.m_ibv.Format = DXGI_FORMAT_R32_UINT;
		}
		staging->Unmap(0, nullptr);

		// The block starts in COMMON, which a buffer's first copy promotes to
		// COPY_DEST implicitly - so the barrier leaves from COPY_DEST.
		device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
			list->CopyBufferRegion(block.Get(), 0, staging.Get(), 0, blockSize[b]);
			const auto barrier =
				Transition(block.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
			list->ResourceBarrier(1, &barrier);
		});
	}
	return out;
}

void Mesh::Bind(ID3D12GraphicsCommandList* list) const {
	list->IASetVertexBuffers(0, 1, &m_vbv);
	list->IASetIndexBuffer(&m_ibv);
}

} // namespace dungeon::gfx
