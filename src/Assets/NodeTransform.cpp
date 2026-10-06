// ============================================================================
// Assets/NodeTransform.cpp - a glTF node's transform, baked into its mesh.
//
// LoadGltf keeps each mesh in its NODE's space and hands the node's world
// transform over beside it (MeshData::worldTransform). Every consumer that
// wants the model's own space comes here, so the rule is written once: seven
// hand copies of the loop (the world's loaders, the asset picker, import-model)
// all sent normals through the plain matrix and none reversed a mirrored
// node's winding (code-review C253). Not a latent case - three of the bought
// daggers carry a non-uniform node scale, and french_dagger's also mirrors.
//
// Its own TU, and PURE (Model.h's data and DirectXMath, nothing else), so
// RollTest links the shipping rule rather than a copy of it - the
// Graphics/LightTiles.cpp bargain.
// ============================================================================
#include "Assets/Model.h"

#include <utility>

using namespace DirectX;

namespace dungeon::assets {

Mat4 NodeTransform(const MeshData& mesh) {
	// glTF: "the transform of the node that the skinned mesh is attached to
	// MUST be ignored" - its joints place it, and its inverse binds already
	// take the vertices as they are in the file.
	return mesh.skinned ? Mat4Identity() : mesh.worldTransform;
}

void BakeNodeTransform(MeshData& mesh) {
	if (mesh.skinned) return; // left exactly as it is (NodeTransform above)
	const XMMATRIX m = XMLoadFloat4x4(&mesh.worldTransform);
	// THE NORMAL MATRIX. Row vectors: a point goes p * M, so a plane's normal
	// goes n * (M^-1)^T - the inverse-transpose. The plain matrix is only right
	// for a rotation and a uniform scale; under a non-uniform one it leans
	// every normal toward the stretched axis. Taken as the COFACTOR matrix
	// (rows r1 x r2, r2 x r0, r0 x r1 of the upper 3x3, = det x (M^-1)^T) times
	// the determinant's sign: the same direction with no division, so a node
	// scaled to a few millimetres (the torches: det ~ 1e-8) costs no
	// precision, and a degenerate one gives a zero normal, not a NaN.
	const XMVECTOR r0 = m.r[0], r1 = m.r[1], r2 = m.r[2];
	const float det = XMVectorGetX(XMVector3Dot(r0, XMVector3Cross(r1, r2)));
	const float sign = det < 0.0f ? -1.0f : 1.0f;
	XMMATRIX normalM = XMMatrixIdentity();
	normalM.r[0] = XMVectorScale(XMVector3Cross(r1, r2), sign);
	normalM.r[1] = XMVectorScale(XMVector3Cross(r2, r0), sign);
	normalM.r[2] = XMVectorScale(XMVector3Cross(r0, r1), sign);
	for (Vertex& v : mesh.vertices) {
		XMStoreFloat3(&v.position, XMVector3Transform(XMLoadFloat3(&v.position), m));
		XMStoreFloat3(&v.normal,
					  XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&v.normal), normalM)));
	}
	// A MIRROR (negative determinant) turns every triangle's corner order
	// round, so what was counter-clockwise seen from outside is now clockwise:
	// put it back, or a back-culled draw shows the mesh inside out.
	if (det < 0.0f)
		for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
			std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
	mesh.worldTransform = Mat4Identity(); // baked: a second bake changes nothing
}

} // namespace dungeon::assets
