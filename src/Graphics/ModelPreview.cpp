// ============================================================================
// Graphics/ModelPreview.cpp — see ModelPreview.h.
// ============================================================================
#include "Graphics/ModelPreview.h"

#include "Graphics/Camera.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/Lights.h"

namespace dungeon::gfx {

namespace {
constexpr float kPreviewClear[4] = {0.06f, 0.06f, 0.09f, 1.0f};
}

void BeginOffscreen(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
					D3D12_CPU_DESCRIPTOR_HANDLE dsv, u32 size, const float clear[4]) {
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const D3D12_VIEWPORT vp{0, 0, static_cast<float>(size), static_cast<float>(size),
						   0.0f, 1.0f};
	list->RSSetViewports(1, &vp);
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
	list->RSSetScissorRects(1, &scissor);
	list->ClearRenderTargetView(rtv, clear, 0, nullptr);
	list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

ModelPreview::ModelPreview(GraphicsDevice& device, u32 size)
	: m_device(device), m_size(size) {
	ID3D12Device* d = device.Device();
	const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

	// Color target (RGBA8, render-target capable), born in SRV state.
	D3D12_RESOURCE_DESC color{};
	color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	color.Width = size;
	color.Height = size;
	color.DepthOrArraySize = 1;
	color.MipLevels = 1;
	color.Format = kBackBufferFormat;
	color.SampleDesc.Count = 1;
	color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE colorClear{};
	colorClear.Format = kBackBufferFormat;
	for (int i = 0; i < 4; ++i) colorClear.Color[i] = kPreviewClear[i];
	DN_HR(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color,
									 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
									 &colorClear, IID_PPV_ARGS(&m_color)));

	// Depth.
	D3D12_RESOURCE_DESC depth = color;
	depth.Format = kDepthFormat;
	depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE depthClear{};
	depthClear.Format = kDepthFormat;
	depthClear.DepthStencil.Depth = 1.0f;
	DN_HR(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
									 D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
									 IID_PPV_ARGS(&m_depth)));

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = 1;
	DN_HR(d->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
	d->CreateRenderTargetView(m_color.Get(), nullptr,
							  m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = 1;
	DN_HR(d->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)));
	d->CreateDepthStencilView(m_depth.Get(), nullptr,
							  m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

	// Shader-visible SRV (in the device's shared heap, like every Texture).
	m_srv = device.AllocateSrv();
	d->CreateShaderResourceView(m_color.Get(), nullptr, m_srv.cpu);
}

void ModelPreview::Render(ID3D12GraphicsCommandList* list, Renderer& renderer,
						  const Mesh& mesh, const MaterialParams& material, float scale,
						  float orbit, float aspect, std::span<const Mat4> palette) {
	using namespace DirectX;

	D3D12_RESOURCE_BARRIER toRT = Transition(m_color.Get(),
											 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
											 D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourceBarrier(1, &toRT);

	BeginOffscreen(list, m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
				   m_dsvHeap->GetCPUDescriptorHandleForHeapStart(), m_size,
				   kPreviewClear);

	// Shared framing for BOTH previews (the asset-creation dialog / dev `preview`,
	// and the monster-config dialog). Models are grounded (min y = 0), centred in
	// XZ, ~2 m tall. Frame level-on from the front with headroom above and below
	// (the vertical FOV covers ~-0.3..2.5 m, so a full-height humanoid isn't clipped
	// at the skull or feet); this also suits the imported props the asset dialog shows.
	Camera cam;
	cam.SetLens(45.0f * kPi / 180.0f, aspect, 0.05f, 50.0f);
	cam.SetPosition({0.0f, 1.1f, -3.3f});
	cam.SetYawPitch(0.0f, 0.0f);

	LightSet lights;
	lights.ambient = {0.20f, 0.20f, 0.23f};
	lights.points.push_back(
		{{1.6f, 2.6f, -2.2f}, 16.0f, {1.0f, 0.96f, 0.9f}, 3.2f, -1, false});

	renderer.BeginScene(list, cam, lights);
	Mat4 world;
	XMStoreFloat4x4(&world, XMMatrixScaling(scale, scale, scale) * XMMatrixRotationY(orbit));
	renderer.DrawMesh(list, mesh, world, material, palette);

	D3D12_RESOURCE_BARRIER toSRV = Transition(m_color.Get(),
											  D3D12_RESOURCE_STATE_RENDER_TARGET,
											  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &toSRV);
}

} // namespace dungeon::gfx
