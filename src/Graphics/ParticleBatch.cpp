#include "Graphics/ParticleBatch.h"

#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <cmath>
#include <cstring>

using namespace DirectX;

namespace dungeon::gfx {

ParticleBatch::ParticleBatch(GraphicsDevice& device) : m_device(device) {
	// Root signature: 0 = view-projection CBV, 1 = sprite texture table.
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;

	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srvRange;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 2;
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

	const std::string shaderPath = paths::Asset("shaders\\particle.hlsl");
	ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20,
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
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	// Premultiplied alpha: additive and smoky particles share one state.
	auto& blend = pso.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_ONE;
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Depth-tested against the scene, but particles never write depth.
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));

	for (u32 i = 0; i < kFrameCount; ++i)
		m_frameAllocators[i] =
			std::make_unique<UploadAllocator>(m_device.Device(), 1024 * 1024);

	// Soft radial sprite: white with a smooth alpha falloff to the edge.
	constexpr u32 kSize = 64;
	assets::ImageData sprite;
	sprite.width = sprite.height = kSize;
	sprite.pixels.resize(static_cast<size_t>(kSize) * kSize * 4);
	for (u32 y = 0; y < kSize; ++y) {
		for (u32 x = 0; x < kSize; ++x) {
			const float dx = (x + 0.5f) / kSize - 0.5f;
			const float dy = (y + 0.5f) / kSize - 0.5f;
			float a = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy) * 2.0f);
			a = a * a * (3.0f - 2.0f * a); // smoothstep edge
			const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
			sprite.pixels[i + 0] = 255;
			sprite.pixels[i + 1] = 255;
			sprite.pixels[i + 2] = 255;
			sprite.pixels[i + 3] = static_cast<u8>(a * 255.0f);
		}
	}
	m_sprite = std::make_unique<Texture>(m_device, sprite);
}

void ParticleBatch::NewFrame(u32 frameIndex) {
	m_frameIndex = frameIndex;
	m_frameAllocators[m_frameIndex]->Reset();
}

void ParticleBatch::Render(ID3D12GraphicsCommandList* list, const Camera& camera,
						   std::span<const ParticleInstance> instances) {
	if (instances.empty()) return;
	UploadAllocator& allocator = *m_frameAllocators[m_frameIndex];

	// Camera basis for the billboards.
	const Vec3 fwd = camera.Forward();
	const XMVECTOR f = XMVector3Normalize(XMVectorSet(fwd.x, fwd.y, fwd.z, 0));
	const XMVECTOR r = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), f));
	const XMVECTOR u = XMVector3Cross(f, r);
	Vec3 right, up;
	XMStoreFloat3(&right, r);
	XMStoreFloat3(&up, u);

	// Build six vertices per particle straight into the frame arena.
	const u64 vbSize = instances.size() * 6 * sizeof(ParticleVertex);
	UploadAllocation vbAlloc = allocator.Allocate(vbSize, 16);
	auto* v = static_cast<ParticleVertex*>(vbAlloc.cpu);
	for (const ParticleInstance& p : instances) {
		const Vec3 rs = Scale(right, p.size);
		const Vec3 us = Scale(up, p.size);
		const Vec3 c0 = Sub(Sub(p.position, rs), us); // bottom-left
		const Vec3 c1 = Sub(Add(p.position, rs), us); // bottom-right
		const Vec3 c2 = Add(Add(p.position, rs), us); // top-right
		const Vec3 c3 = Add(Sub(p.position, rs), us); // top-left
		const ParticleVertex q0{c0, {0, 1}, p.color};
		const ParticleVertex q1{c1, {1, 1}, p.color};
		const ParticleVertex q2{c2, {1, 0}, p.color};
		const ParticleVertex q3{c3, {0, 0}, p.color};
		*v++ = q0; *v++ = q1; *v++ = q2;
		*v++ = q0; *v++ = q2; *v++ = q3;
	}

	// View-projection constants.
	const Mat4 viewProj = camera.ViewProj();
	UploadAllocation cbAlloc = allocator.Allocate(sizeof(Mat4));
	std::memcpy(cbAlloc.cpu, &viewProj, sizeof(viewProj));

	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->SetPipelineState(m_pso.Get());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->SetGraphicsRootConstantBufferView(0, cbAlloc.gpu);
	list->SetGraphicsRootDescriptorTable(1, m_sprite->GpuHandle());

	D3D12_VERTEX_BUFFER_VIEW vbv{};
	vbv.BufferLocation = vbAlloc.gpu;
	vbv.SizeInBytes = static_cast<UINT>(vbSize);
	vbv.StrideInBytes = sizeof(ParticleVertex);
	list->IASetVertexBuffers(0, 1, &vbv);
	list->DrawInstanced(static_cast<UINT>(instances.size() * 6), 1, 0, 0);
}

} // namespace dungeon::gfx
