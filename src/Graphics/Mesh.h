// ============================================================================
// Graphics/Mesh.h — immutable GPU mesh.
//
// Uploads an assets::MeshData's vertex/index arrays to default-heap memory
// at construction (blocking; load-time only). The single engine-wide vertex
// layout (position/normal/uv/joints/weights) is declared in the Renderer's
// input layout — change assets::Vertex and that layout together.
//
// MANY MESHES AT ONCE: CreateMeshes uploads a whole set together, and that is
// how the dungeon's geometry chunks arrive (hundreds per level). One at a time,
// every mesh paid for two committed resources, a staging buffer, a fresh command
// allocator and list, a submit and a full GPU wait - measured at ~0.7 ms a
// buffer, 526 ms of a 931 ms level change, of which the wait itself was only an
// eighth. Batched, the meshes are packed into a few large shared buffers
// (kBlockBytes each) with one staging buffer, one submit and one wait per block.
// A mesh holds a reference to its block, so a block lives until the last mesh
// in it is destroyed: replacing some chunks (an editor paint) leaves their old
// space unused in a block the rest of the level still holds, until the next
// full rebuild. The single-mesh constructor goes through the same path as a
// batch of one, so it too packs vertices and indices into one resource.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Graphics/D3DUtil.h"

#include <memory>
#include <span>
#include <vector>

namespace dungeon::gfx {

class GraphicsDevice;
class Mesh {
public:
	Mesh(GraphicsDevice& device, const assets::MeshData& data);

	void Bind(ID3D12GraphicsCommandList* list) const;
	u32 IndexCount() const { return m_indexCount; }

private:
	Mesh() = default;
	friend std::vector<std::unique_ptr<Mesh>> CreateMeshes(
		GraphicsDevice& device, std::span<const assets::MeshData* const> meshes);

	ComPtr<ID3D12Resource> m_buffer; // shared by every mesh packed into it
	D3D12_VERTEX_BUFFER_VIEW m_vbv{};
	D3D12_INDEX_BUFFER_VIEW m_ibv{};
	u32 m_indexCount = 0;
};

// Uploads every mesh in `meshes` together (see the header comment); the result
// is parallel to the input. Blocks until the GPU holds the data, like the
// single-mesh constructor.
std::vector<std::unique_ptr<Mesh>> CreateMeshes(
	GraphicsDevice& device, std::span<const assets::MeshData* const> meshes);

} // namespace dungeon::gfx
