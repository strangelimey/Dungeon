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
						  const Mesh& mesh, const MaterialParams& material, float orbit) {
	using namespace DirectX;

	D3D12_RESOURCE_BARRIER toRT = Transition(m_color.Get(),
											 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
											 D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourceBarrier(1, &toRT);

	const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
		m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
		m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const D3D12_VIEWPORT vp{0, 0, static_cast<float>(m_size),
						   static_cast<float>(m_size), 0.0f, 1.0f};
	list->RSSetViewports(1, &vp);
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_size),
							 static_cast<LONG>(m_size)};
	list->RSSetScissorRects(1, &scissor);
	list->ClearRenderTargetView(rtv, kPreviewClear, 0, nullptr);
	list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// Imported models are grounded (min y = 0), centred in XZ, ~2 m tall — frame
	// one from the front, looking slightly down at its middle.
	Camera cam;
	cam.SetLens(40.0f * kPi / 180.0f, 1.0f, 0.05f, 50.0f);
	cam.SetPosition({0.0f, 1.15f, -3.0f});
	cam.SetYawPitch(0.0f, -0.14f);

	LightSet lights;
	lights.ambient = {0.20f, 0.20f, 0.23f};
	lights.points.push_back(
		{{1.6f, 2.6f, -2.2f}, 16.0f, {1.0f, 0.96f, 0.9f}, 3.2f, -1, false});

	renderer.BeginScene(list, cam, lights);
	Mat4 world;
	XMStoreFloat4x4(&world, XMMatrixRotationY(orbit));
	renderer.DrawMesh(list, mesh, world, material);

	D3D12_RESOURCE_BARRIER toSRV = Transition(m_color.Get(),
											  D3D12_RESOURCE_STATE_RENDER_TARGET,
											  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &toSRV);
}

} // namespace dungeon::gfx
