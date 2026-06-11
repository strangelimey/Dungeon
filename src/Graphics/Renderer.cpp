#include "Graphics/Renderer.h"

#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <cstring>

namespace dungeon::gfx {

// ============================================================================
// GPU constant layouts
// ============================================================================
namespace {

// Must match the cbuffer layouts in scene.hlsl byte for byte (b0/b1 below).
// Mind HLSL packing: each field group pads to 16 bytes. Matrices are
// uploaded row-major and the shaders use mul(matrix, vector) throughout.
struct GpuPointLight {
	Vec4 positionRadius;
	Vec4 colorIntensity;
};

struct FrameConstants {
	Mat4 viewProj;
	Vec4 cameraPos;
	Vec4 ambient;
	Vec4 dirDirection;
	Vec4 dirColor;
	u32 pointLightCount;
	u32 pad[3];
	GpuPointLight pointLights[kMaxPointLights];
};

struct ObjectConstants {
	Mat4 world;
	Vec4 baseColor;
	u32 useTexture;
	u32 skinned;
	u32 useNormalMap;
	float heightScale;
	float specStrength;
	float specPower;
	float pad[2];
};

} // namespace

// ============================================================================
// Pipeline construction: root signature, shader compile, PSO, fallbacks.
// ============================================================================
Renderer::Renderer(GraphicsDevice& device) : m_device(device) {
	// Root signature:
	//   0: b0 frame constants (CBV)
	//   1: b1 object constants (CBV)
	//   2: b2 skinning palette (CBV)
	//   3: t0 base color texture (table)
	//   4: t1 normal+height map (table)
	D3D12_DESCRIPTOR_RANGE srvRange0{};
	srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange0.NumDescriptors = 1;
	srvRange0.BaseShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE srvRange1 = srvRange0;
	srvRange1.BaseShaderRegister = 1;

	D3D12_ROOT_PARAMETER params[5]{};
	for (int i = 0; i < 3; ++i) {
		params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &srvRange0;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &srvRange1;
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_ANISOTROPIC;
	sampler.MaxAnisotropy = 8;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 5;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> blob, errors;
	DN_HR(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
									  &errors));
	DN_HR(m_device.Device()->CreateRootSignature(0, blob->GetBufferPointer(),
												 blob->GetBufferSize(),
												 IID_PPV_ARGS(&m_rootSignature)));

	// Pipeline.
	const std::string shaderPath = paths::Asset("shaders\\scene.hlsl");
	ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"JOINTS", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"WEIGHTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = m_rootSignature.Get();
	pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	pso.InputLayout = {layout, _countof(layout)};
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = kBackBufferFormat;
	pso.DSVFormat = kDepthFormat;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;

	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// Dungeon interiors are viewed from inside hand-built geometry; culling
	// off avoids winding mistakes ever producing invisible walls.
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.DepthClipEnable = TRUE;

	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));

	for (u32 i = 0; i < kFrameCount; ++i)
		m_frameAllocators[i] =
			std::make_unique<UploadAllocator>(m_device.Device(), 4 * 1024 * 1024);

	assets::ImageData white;
	white.width = white.height = 1;
	white.pixels = {255, 255, 255, 255};
	m_whiteTexture = std::make_unique<Texture>(m_device, white);

	// Flat tangent-space normal (0,0,1) with zero height.
	assets::ImageData flat;
	flat.width = flat.height = 1;
	flat.pixels = {128, 128, 255, 0};
	m_flatNormalMap = std::make_unique<Texture>(m_device, flat);
}

// ============================================================================
// Per-frame drawing
// ============================================================================

void Renderer::NewFrame(u32 frameIndex) {
	m_frameIndex = frameIndex;
	m_frameAllocators[m_frameIndex]->Reset(); // reclaim last use of this slot
}

void Renderer::BeginScene(ID3D12GraphicsCommandList* list, const Camera& camera,
						  const LightSet& lights) {
	FrameConstants frame{};
	frame.viewProj = camera.ViewProj();
	const Vec3& cam = camera.Position();
	frame.cameraPos = {cam.x, cam.y, cam.z, 1.0f};
	frame.ambient = {lights.ambient.x, lights.ambient.y, lights.ambient.z, 1.0f};
	frame.dirDirection = {lights.directional.direction.x, lights.directional.direction.y,
						  lights.directional.direction.z, 0.0f};
	frame.dirColor = {lights.directional.color.x, lights.directional.color.y,
					  lights.directional.color.z, 0.0f};
	frame.pointLightCount =
		static_cast<u32>(std::min<size_t>(lights.points.size(), kMaxPointLights));
	for (u32 i = 0; i < frame.pointLightCount; ++i) {
		const PointLight& l = lights.points[i];
		frame.pointLights[i].positionRadius = {l.position.x, l.position.y, l.position.z,
											   l.radius};
		frame.pointLights[i].colorIntensity = {l.color.x, l.color.y, l.color.z,
											   l.intensity};
	}

	UploadAllocation alloc =
		m_frameAllocators[m_frameIndex]->Allocate(sizeof(FrameConstants));
	std::memcpy(alloc.cpu, &frame, sizeof(frame));

	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->SetPipelineState(m_pso.Get());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->SetGraphicsRootConstantBufferView(0, alloc.gpu);
}

void Renderer::DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh,
						const Mat4& world, const MaterialParams& material,
						std::span<const Mat4> palette) {
	UploadAllocator& allocator = *m_frameAllocators[m_frameIndex];

	ObjectConstants object{};
	object.world = world;
	object.baseColor = material.baseColor;
	object.useTexture = material.albedo != nullptr ? 1u : 0u;
	object.skinned = palette.empty() ? 0u : 1u;
	object.useNormalMap = material.normalMap != nullptr ? 1u : 0u;
	object.heightScale = material.heightScale;
	object.specStrength = material.specStrength;
	object.specPower = material.specPower;
	UploadAllocation objAlloc = allocator.Allocate(sizeof(ObjectConstants));
	std::memcpy(objAlloc.cpu, &object, sizeof(object));
	list->SetGraphicsRootConstantBufferView(1, objAlloc.gpu);

	// The palette CBV must always be bound; reuse the object CB when unskinned.
	if (!palette.empty()) {
		const size_t count = std::min<size_t>(palette.size(), kMaxSkinJoints);
		UploadAllocation skinAlloc = allocator.Allocate(kMaxSkinJoints * sizeof(Mat4));
		std::memcpy(skinAlloc.cpu, palette.data(), count * sizeof(Mat4));
		list->SetGraphicsRootConstantBufferView(2, skinAlloc.gpu);
	} else {
		list->SetGraphicsRootConstantBufferView(2, objAlloc.gpu);
	}

	const Texture* bound = material.albedo ? material.albedo : m_whiteTexture.get();
	list->SetGraphicsRootDescriptorTable(3, bound->GpuHandle());
	const Texture* boundNormal =
		material.normalMap ? material.normalMap : m_flatNormalMap.get();
	list->SetGraphicsRootDescriptorTable(4, boundNormal->GpuHandle());

	mesh.Bind(list);
	list->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}

} // namespace dungeon::gfx
