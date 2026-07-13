#include "Graphics/PostProcess.h"

#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <algorithm>

namespace dungeon::gfx {

namespace {

// Must match the cbuffer layout in post.hlsl (b0, eight 32-bit root constants).
struct PostConstants {
	float texelX, texelY; // 1 / source dimensions
	float dirX, dirY;     // blur direction: (1,0) or (0,1)
	float threshold;      // bloom bright-pass threshold (linear HDR)
	float knee;           // smoothstep width above the threshold
	float strength;       // bloom add weight in the composite
	float exposure;       // pre-tonemap exposure
};

// First-cut bloom tuning — iterate in post.hlsl/here + relaunch.
constexpr float kBloomThreshold = 1.0f;
constexpr float kBloomKnee = 1.0f;
constexpr float kBloomStrength = 0.55f;
constexpr float kExposure = 1.0f;

} // namespace

// ============================================================================
// Pipeline construction — one root signature, three full-screen-triangle PSOs.
// ============================================================================
PostProcess::PostProcess(GraphicsDevice& device) : m_device(device) {
	// Root signature:
	//   0: b0 pass constants (root constants — PostConstants)
	//   1: t0 source texture (table)
	//   2: t1 bloom texture  (table; composite only, always bound)
	D3D12_DESCRIPTOR_RANGE range0{};
	range0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range0.NumDescriptors = 1;
	range0.BaseShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE range1 = range0;
	range1.BaseShaderRegister = 1;

	D3D12_ROOT_PARAMETER params[3]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = sizeof(PostConstants) / 4;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	const D3D12_DESCRIPTOR_RANGE* ranges[2] = {&range0, &range1};
	for (int i = 1; i < 3; ++i) {
		params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i].DescriptorTable.NumDescriptorRanges = 1;
		params[i].DescriptorTable.pDescriptorRanges = ranges[i - 1];
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;

	ComPtr<ID3DBlob> blob, errors;
	DN_HR(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
									  &errors));
	DN_HR(m_device.Device()->CreateRootSignature(0, blob->GetBufferPointer(),
												 blob->GetBufferSize(),
												 IID_PPV_ARGS(&m_rootSignature)));

	// The three passes share one vertex shader (a bufferless full-screen
	// triangle from SV_VertexID), so no input layout anywhere.
	const std::string shaderPath = paths::Asset("shaders\\post.hlsl");
	ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> bright = CompileShader(shaderPath, "PSBright", "ps_5_1");
	ComPtr<ID3DBlob> blur = CompileShader(shaderPath, "PSBlur", "ps_5_1");
	ComPtr<ID3DBlob> composite = CompileShader(shaderPath, "PSComposite", "ps_5_1");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = m_rootSignature.Get();
	pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	// Depth stays disabled: the composite draws with the back buffer's DSV
	// still bound (BindBackBuffer), which is legal with depth+stencil off.

	pso.PS = {bright->GetBufferPointer(), bright->GetBufferSize()};
	pso.RTVFormats[0] = kSceneColorFormat;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoBright)));

	pso.PS = {blur->GetBufferPointer(), blur->GetBufferSize()};
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoBlur)));

	pso.PS = {composite->GetBufferPointer(), composite->GetBufferSize()};
	pso.RTVFormats[0] = kBackBufferFormat;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso,
														 IID_PPV_ARGS(&m_psoComposite)));

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = 3; // scene + bloom ping-pong pair
	DN_HR(m_device.Device()->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));

	// SRV slots are allocated once and keep their heap indices across resizes;
	// CreateTargets rewrites the views in place (after a WaitIdle — see the
	// AllocateSrv recycling rule in GraphicsDevice.h).
	m_sceneSrv = m_device.AllocateSrv();
	m_bloomSrv[0] = m_device.AllocateSrv();
	m_bloomSrv[1] = m_device.AllocateSrv();

	CreateTargets();
}

PostProcess::~PostProcess() {
	m_device.FreeSrv(m_sceneSrv.index);
	m_device.FreeSrv(m_bloomSrv[0].index);
	m_device.FreeSrv(m_bloomSrv[1].index);
}

void PostProcess::CreateTargets() {
	m_width = m_device.Width();
	m_height = m_device.Height();
	const u32 halfW = std::max(1u, m_width / 2);
	const u32 halfH = std::max(1u, m_height / 2);

	ID3D12Device* device = m_device.Device();
	const u32 rtvSize =
		device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	auto makeTarget = [&](u32 w, u32 h, const D3D12_CLEAR_VALUE* clear,
						  ComPtr<ID3D12Resource>& out) {
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = kSceneColorFormat;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		out.Reset();
		DN_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
											  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
											  clear, IID_PPV_ARGS(&out)));
	};

	// The scene target is the only one that clears (BeginScene); the bloom
	// pair is fully overwritten by each pass, so no optimized clear there.
	D3D12_CLEAR_VALUE sceneClear{};
	sceneClear.Format = kSceneColorFormat;
	makeTarget(m_width, m_height, &sceneClear, m_scene);
	makeTarget(halfW, halfH, nullptr, m_bloom[0]);
	makeTarget(halfW, halfH, nullptr, m_bloom[1]);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(m_scene.Get(), nullptr, rtv);
	rtv.ptr += rtvSize;
	device->CreateRenderTargetView(m_bloom[0].Get(), nullptr, rtv);
	rtv.ptr += rtvSize;
	device->CreateRenderTargetView(m_bloom[1].Get(), nullptr, rtv);

	device->CreateShaderResourceView(m_scene.Get(), nullptr, m_sceneSrv.cpu);
	device->CreateShaderResourceView(m_bloom[0].Get(), nullptr, m_bloomSrv[0].cpu);
	device->CreateShaderResourceView(m_bloom[1].Get(), nullptr, m_bloomSrv[1].cpu);
}

// ============================================================================
// Per-frame passes
// ============================================================================

void PostProcess::BeginScene(ID3D12GraphicsCommandList* list) {
	// Follow the window size. Rare, and the swapchain resize path has already
	// idled the GPU this frame, but the SRV rewrite rule wants its own drain.
	if (m_width != m_device.Width() || m_height != m_device.Height()) {
		m_device.WaitIdle();
		CreateTargets();
	}

	const auto barrier = Transition(m_scene.Get(),
									D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
									D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourceBarrier(1, &barrier);

	const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
		m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_device.DepthDsv();
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	list->ClearRenderTargetView(rtv, clear, 0, nullptr);
	list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width),
								  static_cast<float>(m_height), 0.0f, 1.0f};
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width),
							 static_cast<LONG>(m_height)};
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
}

void PostProcess::Resolve(ID3D12GraphicsCommandList* list) {
	const u32 halfW = std::max(1u, m_width / 2);
	const u32 halfH = std::max(1u, m_height / 2);
	const u32 rtvSize = m_device.Device()->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto bloomRtv = [&](int i) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<size_t>(1 + i) * rtvSize;
		return rtv;
	};

	{
		const auto barrier = Transition(m_scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
										D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &barrier);
	}

	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// One full-screen-triangle pass: transition the destination to RT, draw
	// sampling `src` (t0), transition back to SRV so the next pass reads it.
	auto pass = [&](ID3D12PipelineState* pso, int dst, const SrvHandle& src,
					const PostConstants& constants) {
		auto toRt = Transition(m_bloom[dst].Get(),
							   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
							   D3D12_RESOURCE_STATE_RENDER_TARGET);
		list->ResourceBarrier(1, &toRt);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = bloomRtv(dst);
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(halfW),
									  static_cast<float>(halfH), 0.0f, 1.0f};
		const D3D12_RECT scissor{0, 0, static_cast<LONG>(halfW),
								 static_cast<LONG>(halfH)};
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->SetPipelineState(pso);
		list->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &constants, 0);
		list->SetGraphicsRootDescriptorTable(1, src.gpu);
		list->SetGraphicsRootDescriptorTable(2, m_bloomSrv[0].gpu); // t1 unused here
		list->DrawInstanced(3, 1, 0, 0);
		auto toSrv = Transition(m_bloom[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
								D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &toSrv);
	};

	PostConstants constants{};
	constants.threshold = kBloomThreshold;
	constants.knee = kBloomKnee;
	constants.strength = kBloomStrength;
	constants.exposure = kExposure;

	// Bright-pass: full-res scene -> half-res bloom[0] (the downsample is the
	// bilinear fetch itself).
	constants.texelX = 1.0f / static_cast<float>(m_width);
	constants.texelY = 1.0f / static_cast<float>(m_height);
	pass(m_psoBright.Get(), 0, m_sceneSrv, constants);

	// Two separable gaussian iterations over the half-res pair; the result
	// lands back in bloom[0] for the composite.
	constants.texelX = 1.0f / static_cast<float>(halfW);
	constants.texelY = 1.0f / static_cast<float>(halfH);
	for (int i = 0; i < 2; ++i) {
		constants.dirX = 1.0f;
		constants.dirY = 0.0f;
		pass(m_psoBlur.Get(), 1, m_bloomSrv[0], constants);
		constants.dirX = 0.0f;
		constants.dirY = 1.0f;
		pass(m_psoBlur.Get(), 0, m_bloomSrv[1], constants);
	}

	// Composite: scene + bloom, ACES tonemap + gamma, onto the back buffer.
	m_device.BindBackBuffer(list);
	list->SetPipelineState(m_psoComposite.Get());
	list->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &constants, 0);
	list->SetGraphicsRootDescriptorTable(1, m_sceneSrv.gpu);
	list->SetGraphicsRootDescriptorTable(2, m_bloomSrv[0].gpu);
	list->DrawInstanced(3, 1, 0, 0);
}

} // namespace dungeon::gfx
