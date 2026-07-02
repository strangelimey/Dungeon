// ============================================================================
// Graphics/ModelPreview.cpp — see ModelPreview.h.
// ============================================================================
#include "Graphics/ModelPreview.h"

#include "Graphics/Camera.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/Lights.h"

#include <algorithm>

namespace dungeon::gfx {

namespace {
constexpr float kPreviewClear[4] = {0.06f, 0.06f, 0.09f, 1.0f};
constexpr float kLookHeight = 1.1f; // camera look-ray height (matches SetPosition below)
constexpr float kFitSize = 1.8f;    // auto-fit target: fill ~1.8 m of the framed height
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
						  float orbit, float aspect, std::span<const Mat4> palette,
						  ParticleBatch* particles,
						  std::span<const ParticleInstance> billboards) {
	const PreviewSubmesh one{&mesh, material};
	Render(list, renderer, std::span<const PreviewSubmesh>{&one, 1}, scale, orbit, aspect,
		   palette, particles, billboards);
}

void ModelPreview::Render(ID3D12GraphicsCommandList* list, Renderer& renderer,
						  std::span<const PreviewSubmesh> subs, float scale, float orbit,
						  float aspect, std::span<const Mat4> palette, ParticleBatch* particles,
						  std::span<const ParticleInstance> billboards, const Vec3* fitMin,
						  const Vec3* fitMax) {
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
	// Bright, balanced studio light so any prop reads (a rough marble column or a
	// flat item tablet catches far less than a metal sconce or a skinned monster):
	// stronger ambient + a warm key from the upper right and a cool fill from the
	// lower left.
	lights.ambient = {0.42f, 0.42f, 0.46f};
	lights.points.push_back(
		{{1.8f, 2.6f, -2.4f}, 24.0f, {1.0f, 0.96f, 0.9f}, 6.0f, -1, false});
	lights.points.push_back(
		{{-2.0f, 1.2f, -2.6f}, 24.0f, {0.80f, 0.86f, 1.0f}, 3.5f, -1, false});

	renderer.BeginScene(list, cam, lights);
	Mat4 world;
	if (fitMin && fitMax) {
		// Auto-fit: centre the AABB at origin, scale to fill, spin, lift to eye height.
		const Vec3 c{(fitMin->x + fitMax->x) * 0.5f, (fitMin->y + fitMax->y) * 0.5f,
					 (fitMin->z + fitMax->z) * 0.5f};
		const float ext = std::max({fitMax->x - fitMin->x, fitMax->y - fitMin->y,
									fitMax->z - fitMin->z});
		const float k = (ext > 1e-4f ? kFitSize / ext : 1.0f) * scale;
		// Tumble on TWO axes (Y spin + a slower X pitch off the same clock) so a flat
		// prop like a blade never sits edge-on for long.
		XMStoreFloat4x4(&world, XMMatrixTranslation(-c.x, -c.y, -c.z) *
									XMMatrixRotationX(orbit * 0.6f) * XMMatrixRotationY(orbit) *
									XMMatrixScaling(k, k, k) *
									XMMatrixTranslation(0.0f, kLookHeight, 0.0f));
	} else {
		XMStoreFloat4x4(&world, XMMatrixScaling(scale, scale, scale) * XMMatrixRotationY(orbit));
	}
	for (const PreviewSubmesh& s : subs)
		if (s.mesh) renderer.DrawMesh(list, *s.mesh, world, s.material, palette);

	// Optional flame/smoke billboards, drawn with the same camera after the meshes.
	if (particles && !billboards.empty()) particles->Render(list, cam, billboards);

	D3D12_RESOURCE_BARRIER toSRV = Transition(m_color.Get(),
											  D3D12_RESOURCE_STATE_RENDER_TARGET,
											  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &toSRV);
}

} // namespace dungeon::gfx
