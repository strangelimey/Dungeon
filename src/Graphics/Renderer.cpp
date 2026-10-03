#include "Graphics/Renderer.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <algorithm>
#include <cmath>
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
	Vec4 shadow; // x = shadow cube slot (-1 = unshadowed), y = shadow strength
};

struct FrameConstants {
	Mat4 viewProj;
	Vec4 cameraPos;
	Vec4 ambient;
	Vec4 dirDirection;
	Vec4 dirColor;
	u32 pointLightCount;
	u32 tonemapInline; // 1 = tonemap in the scene PS (LDR targets), 0 = HDR out
	u32 pad[2];
	Vec4 fogGrid;     // xy = 1 / atmosphere world extent, z = density, w = haze ambient
	Vec4 hazeColor;   // rgb = dust albedo tint
	Vec4 shadowLight; // shadow pass only: xyz = light pos, w = 1 / radius
	Vec4 sightCell;   // see-through peek: xy = cell world min XZ, zw = max (inactive when zw <= xy)
	Vec4 sightTint;   // rgb = ghost tint (school colour), a = strength (0 = off)
	Vec4 sightHole;   // round peephole: x = centre world Y, y = radius (m), z = across-axis is X (>0.5), w unused
	Vec4 dustPuffs[kMaxDustPuffs]; // brief dust: xy = centre world XZ, z = radius (m), w = turbidity
	GpuPointLight pointLights[kMaxPointLights];
};

// View-projection for one face of a point light's shadow cube (standard D3D
// cube face order/orientation, 90-degree FOV, far plane at the light radius).
Mat4 CubeFaceViewProj(u32 face, const Vec3& lightPos, float radius) {
	using namespace DirectX;
	static const XMVECTORF32 kDirs[6] = {
		{{1, 0, 0, 0}}, {{-1, 0, 0, 0}}, {{0, 1, 0, 0}},
		{{0, -1, 0, 0}}, {{0, 0, 1, 0}}, {{0, 0, -1, 0}},
	};
	static const XMVECTORF32 kUps[6] = {
		{{0, 1, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, -1, 0}},
		{{0, 0, 1, 0}}, {{0, 1, 0, 0}}, {{0, 1, 0, 0}},
	};
	const XMVECTOR eye = XMVectorSet(lightPos.x, lightPos.y, lightPos.z, 1.0f);
	const XMMATRIX view = XMMatrixLookToLH(eye, kDirs[face], kUps[face]);
	const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.05f, radius);
	Mat4 out;
	XMStoreFloat4x4(&out, view * proj);
	return out;
}

struct ObjectConstants {
	Mat4 world;
	Vec4 baseColor;
	u32 useTexture;
	u32 skinned;
	u32 useNormalMap;
	float heightScale;
	float metallic;
	float roughness;
	u32 useMRMap;
	float alphaCutoff; // 0 = opaque; > 0 = alpha-test cutout threshold
	Vec4 emissive;
	u32 transparent; // 0 = opaque, 1 = glass, 2 = liquid (clipped at liquidPlane)
	u32 pad1[3];
	Vec4 liquidPlane; // world-space plane n.xyz, w = d: above it (n.p + d > 0) is clipped
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
	//   5: t2 air turbidity grid (table)
	//   6: t3..t10 shadow cubes (one table, contiguous descriptors)
	//   7: t11 occlusion/roughness/metallic ORM map (table)
	D3D12_DESCRIPTOR_RANGE srvRange0{};
	srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange0.NumDescriptors = 1;
	srvRange0.BaseShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE srvRange1 = srvRange0;
	srvRange1.BaseShaderRegister = 1;
	D3D12_DESCRIPTOR_RANGE srvRange2 = srvRange0;
	srvRange2.BaseShaderRegister = 2;
	D3D12_DESCRIPTOR_RANGE srvRangeShadow = srvRange0;
	srvRangeShadow.BaseShaderRegister = 3;
	srvRangeShadow.NumDescriptors = kShadowSlots;
	D3D12_DESCRIPTOR_RANGE srvRangeMR = srvRange0;
	srvRangeMR.BaseShaderRegister = 3 + kShadowSlots; // t11: first SRV after the cubes

	D3D12_ROOT_PARAMETER params[8]{};
	for (int i = 0; i < 3; ++i) {
		params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	const D3D12_DESCRIPTOR_RANGE* ranges[5] = {&srvRange0, &srvRange1, &srvRange2,
											   &srvRangeShadow, &srvRangeMR};
	for (int i = 3; i < 8; ++i) {
		params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i].DescriptorTable.NumDescriptorRanges = 1;
		params[i].DescriptorTable.pDescriptorRanges = ranges[i - 3];
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	D3D12_STATIC_SAMPLER_DESC samplers[2]{};
	samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
	samplers[0].MaxAnisotropy = 8;
	samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
	samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	// s1: clamped bilinear for the turbidity grid (wrap would leak dust
	// from one map edge to the other).
	samplers[1] = samplers[0];
	samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplers[1].MaxAnisotropy = 0;
	samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].ShaderRegister = 1;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 8;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 2;
	rsDesc.pStaticSamplers = samplers;
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

	// Back-face-culled variant for authored, consistently-wound meshes (imported
	// models). Procedural geometry keeps the double-sided m_pso above.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC culledPso = pso;
	culledPso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	// The scene camera mirrors clip-space X (Camera::ViewProj) so the first-person
	// view matches the right-handed world compass; that flip reverses apparent
	// winding, so front faces are now counter-clockwise. (The shadow pass uses its
	// own non-mirrored projection and the CULL_NONE m_pso, so it is unaffected.)
	culledPso.RasterizerState.FrontCounterClockwise = TRUE;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&culledPso,
														 IID_PPV_ARGS(&m_psoCull)));

	// The same pair again targeting the HDR scene intermediate (the main pass
	// renders there and PostProcess composites into the back buffer; the LDR
	// pair above keeps serving the model previews and icon bakes).
	pso.RTVFormats[0] = kSceneColorFormat;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoHdr)));
	culledPso.RTVFormats[0] = kSceneColorFormat;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&culledPso,
														 IID_PPV_ARGS(&m_psoCullHdr)));

	// Transparent: DUAL-SOURCE blending, result = added + behind x filter per
	// channel (scene.hlsl PSGlass / GlassOutput) - glass FILTERS what is behind
	// it rather than painting over it, so a red potion tints the room red and
	// clear glass passes it untouched. Depth TESTED against the opaque scene but
	// not written, so glass behind glass still draws. Two cull modes, used in
	// turn per entry by FlushTransparent: front-culled draws the far wall,
	// back-culled the near one. FrontCounterClockwise as the culled opaque PSO
	// (the scene camera's mirrored X), so "back" means the same thing to both.
	ComPtr<ID3DBlob> psGlass = CompileShader(shaderPath, "PSGlass", "ps_5_1");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC glassPso = culledPso;
	glassPso.PS = {psGlass->GetBufferPointer(), psGlass->GetBufferSize()};
	D3D12_RENDER_TARGET_BLEND_DESC& blend = glassPso.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_ONE;
	blend.DestBlend = D3D12_BLEND_SRC1_COLOR;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	glassPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	for (int hdr = 0; hdr < 2; ++hdr) {
		glassPso.RTVFormats[0] = hdr ? kSceneColorFormat : kBackBufferFormat;
		for (int cull = 0; cull < 2; ++cull) {
			glassPso.RasterizerState.CullMode =
				cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
			DN_HR(m_device.Device()->CreateGraphicsPipelineState(
				&glassPso, IID_PPV_ARGS(&m_psoGlass[hdr][cull])));
		}
	}
	m_transparent.reserve(kTransparentCapacity);
	m_transparentOrder.reserve(kTransparentCapacity);

	// Shadow pass pipeline: same root signature and input layout, writing
	// normalized light->fragment distance into an R16_FLOAT cube face.
	const std::string shadowPath = paths::Asset("shaders\\shadow.hlsl");
	ComPtr<ID3DBlob> shadowVs = CompileShader(shadowPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> shadowPs = CompileShader(shadowPath, "PSMain", "ps_5_1");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = pso;
	shadowPsoDesc.VS = {shadowVs->GetBufferPointer(), shadowVs->GetBufferSize()};
	shadowPsoDesc.PS = {shadowPs->GetBufferPointer(), shadowPs->GetBufferSize()};
	shadowPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&shadowPsoDesc,
														 IID_PPV_ARGS(&m_shadowPso)));

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

	// Zero-turbidity fallback: perfectly clear air when no map is supplied.
	assets::ImageData black;
	black.width = black.height = 1;
	black.pixels = {0, 0, 0, 255};
	m_blackTexture = std::make_unique<Texture>(m_device, black);

	// Neutral ORM (occlusion=1, roughness=1, metallic=0): bound when a draw has
	// no ORM map, so the metallic/roughness factors pass through unscaled.
	assets::ImageData orm;
	orm.width = orm.height = 1;
	orm.pixels = {255, 255, 0, 255};
	m_defaultMRTexture = std::make_unique<Texture>(m_device, orm);

	CreateShadowResources();
}

void Renderer::CreateShadowResources() {
	ID3D12Device* device = m_device.Device();

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = kShadowSlots * 6;
	DN_HR(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_shadowRtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = kShadowSlots;
	DN_HR(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_shadowDsvHeap)));

	const u32 rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	const u32 dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	for (u32 slot = 0; slot < kShadowSlots; ++slot) {
		const u32 res = kShadowResolution[slot];

		// Distance cube: 6-slice array, viewed as a cube by the scene pass.
		D3D12_RESOURCE_DESC cube{};
		cube.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		cube.Width = res;
		cube.Height = res;
		cube.DepthOrArraySize = 6;
		cube.MipLevels = 1;
		cube.Format = DXGI_FORMAT_R16_FLOAT;
		cube.SampleDesc.Count = 1;
		cube.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		// Must match the clear in BeginShadowFace exactly (all four channels,
		// even though R16_FLOAT keeps only red) or the fast-clear path is lost.
		D3D12_CLEAR_VALUE cubeClear{};
		cubeClear.Format = DXGI_FORMAT_R16_FLOAT;
		cubeClear.Color[0] = cubeClear.Color[1] = 1.0f; // max distance = no occluder
		cubeClear.Color[2] = cubeClear.Color[3] = 1.0f;

		const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		DN_HR(device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &cube,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cubeClear,
			IID_PPV_ARGS(&m_shadowCube[slot])));

		// One RTV per face.
		for (u32 face = 0; face < 6; ++face) {
			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = DXGI_FORMAT_R16_FLOAT;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.FirstArraySlice = face;
			rtv.Texture2DArray.ArraySize = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE handle =
				m_shadowRtvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<size_t>(slot * 6 + face) * rtvSize;
			device->CreateRenderTargetView(m_shadowCube[slot].Get(), &rtv, handle);
		}

		// Shared per-slot depth buffer (reused for each face).
		D3D12_RESOURCE_DESC depth = cube;
		depth.DepthOrArraySize = 1;
		depth.Format = DXGI_FORMAT_D32_FLOAT;
		depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE depthClear{};
		depthClear.Format = DXGI_FORMAT_D32_FLOAT;
		depthClear.DepthStencil.Depth = 1.0f;
		DN_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
											  D3D12_RESOURCE_STATE_DEPTH_WRITE,
											  &depthClear,
											  IID_PPV_ARGS(&m_shadowDepth[slot])));
		D3D12_CPU_DESCRIPTOR_HANDLE dsv =
			m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
		dsv.ptr += static_cast<size_t>(slot) * dsvSize;
		device->CreateDepthStencilView(m_shadowDepth[slot].Get(), nullptr, dsv);
	}

	// Cube SRVs — allocated back to back (boot-time, before any free-list
	// recycling can fragment the heap) so every slot binds as ONE root table.
	for (u32 slot = 0; slot < kShadowSlots; ++slot) {
		m_shadowSrv[slot] = m_device.AllocateSrv();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R16_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.TextureCube.MipLevels = 1;
		device->CreateShaderResourceView(m_shadowCube[slot].Get(), &srv,
										 m_shadowSrv[slot].cpu);
	}
}

// ============================================================================
// Per-frame drawing
// ============================================================================

void Renderer::NewFrame(u32 frameIndex) {
	m_frameIndex = frameIndex;
	m_frameAllocators[m_frameIndex]->Reset(); // reclaim last use of this slot
	m_paletteCache.clear();                   // arena reset invalidates last frame's VAs
	// clear() keeps the capacity, so this only ever allocates when a frame shows
	// more distinct skinned meshes than any frame before it — which is a settled
	// frame reaching for the heap. Reserve a working floor once; the vector can
	// still grow past it, and a report from here would then mean a scene denser
	// than kPaletteCacheReserve expects rather than a bug.
	m_paletteCache.reserve(kPaletteCacheReserve);
}

void Renderer::UpdateTexture(ID3D12GraphicsCommandList* list, const Texture& texture,
							 std::span<const u8> rgba8) {
	ID3D12Resource* resource = texture.Resource();
	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	DN_ASSERT(desc.MipLevels == 1, "UpdateTexture rewrites mip 0 only");
	const u32 w = texture.Width(), h = texture.Height();
	const u64 rowBytes = static_cast<u64>(w) * 4;
	DN_ASSERT(rgba8.size() >= rowBytes * h, "UpdateTexture: too few pixels");
	// A placed footprint wants its rows on 256-byte pitches and its start on a
	// 512-byte boundary; the arena hands out exactly that.
	const u64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
					  ~static_cast<u64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	UploadAllocator& arena = *m_frameAllocators[m_frameIndex];
	const UploadAllocation staged =
		arena.Allocate(pitch * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	u8* dstRows = static_cast<u8*>(staged.cpu);
	for (u32 y = 0; y < h; ++y)
		std::memcpy(dstRows + y * pitch, rgba8.data() + y * rowBytes,
					static_cast<size_t>(rowBytes));

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = arena.Resource();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = staged.gpu - arena.Resource()->GetGPUVirtualAddress();
	src.PlacedFootprint.Footprint = {desc.Format, w, h, 1, static_cast<UINT>(pitch)};

	// The barrier into COPY_DEST also orders the copy after every earlier frame's
	// reads of the texture on this queue.
	const auto toCopy = Transition(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
								   D3D12_RESOURCE_STATE_COPY_DEST);
	list->ResourceBarrier(1, &toCopy);
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	const auto toRead = Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST,
								   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &toRead);
}

void Renderer::BeginScene(ID3D12GraphicsCommandList* list, const Camera& camera,
						  const LightSet& lights, const Atmosphere& atmosphere,
						  bool hdrTarget) {
	FrameConstants frame{};
	frame.viewProj = camera.ViewProj();
	frame.tonemapInline = hdrTarget ? 0u : 1u;
	const Vec3& cam = camera.Position();
	frame.cameraPos = {cam.x, cam.y, cam.z, 1.0f};
	frame.ambient = {lights.ambient.x, lights.ambient.y, lights.ambient.z, 1.0f};
	frame.fogGrid = {1.0f / std::max(atmosphere.worldExtent.x, 1e-3f),
					 1.0f / std::max(atmosphere.worldExtent.y, 1e-3f),
					 atmosphere.turbidityMap ? atmosphere.density : 0.0f,
					 atmosphere.hazeAmbient};
	frame.hazeColor = {atmosphere.hazeColor.x, atmosphere.hazeColor.y,
					   atmosphere.hazeColor.z, 0.0f};
	frame.sightCell = atmosphere.sightCell;
	frame.sightTint = atmosphere.sightTint;
	frame.sightHole = atmosphere.sightHole;
	for (u32 i = 0; i < kMaxDustPuffs; ++i) frame.dustPuffs[i] = atmosphere.dustPuffs[i];
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
		frame.pointLights[i].shadow = {static_cast<float>(l.shadowSlot),
									   l.shadowStrength, 0, 0};
	}

	UploadAllocation alloc =
		m_frameAllocators[m_frameIndex]->Allocate(sizeof(FrameConstants));
	std::memcpy(alloc.cpu, &frame, sizeof(frame));

	// Glass queued by a pass that never flushed belongs to a target that is no
	// longer bound; drawing it here would paint it into this one.
	if (!m_transparent.empty()) {
		if (m_transparentStats.dropped == 0)
			log::Warn("transparent draws dropped: a pass queued glass and never "
					  "called FlushTransparent");
		m_transparentStats.dropped += static_cast<u32>(m_transparent.size());
		m_transparent.clear();
	}
	m_cameraPos = cam;

	m_shadowPass = false;
	m_hdrPass = hdrTarget;
	ID3D12PipelineState* pso = hdrTarget ? m_psoHdr.Get() : m_pso.Get();
	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->SetPipelineState(pso);
	m_currentPso = pso;
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->SetGraphicsRootConstantBufferView(0, alloc.gpu);

	const Texture* turbidity =
		atmosphere.turbidityMap ? atmosphere.turbidityMap : m_blackTexture.get();
	list->SetGraphicsRootDescriptorTable(5, turbidity->GpuHandle());
	list->SetGraphicsRootDescriptorTable(6, m_shadowSrv[0].gpu); // t3..t6
}

// ============================================================================
// Shadow pass
// ============================================================================

void Renderer::BeginShadowFace(ID3D12GraphicsCommandList* list, u32 slot, u32 face,
							   const Vec3& lightPos, float radius) {
	DN_ASSERT(slot < kShadowSlots && face < 6, "shadow slot/face out of range");

	if (!m_shadowInRtState[slot]) {
		const auto barrier = Transition(m_shadowCube[slot].Get(),
										D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
										D3D12_RESOURCE_STATE_RENDER_TARGET);
		list->ResourceBarrier(1, &barrier);
		m_shadowInRtState[slot] = true;
	}

	ID3D12Device* device = m_device.Device();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_shadowRtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<size_t>(slot * 6 + face) *
			   device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	dsv.ptr += static_cast<size_t>(slot) *
			   device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const float clear[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // max distance
	list->ClearRenderTargetView(rtv, clear, 0, nullptr);
	list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	const float res = static_cast<float>(kShadowResolution[slot]);
	const D3D12_VIEWPORT viewport{0, 0, res, res, 0.0f, 1.0f};
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(res), static_cast<LONG>(res)};
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);

	// Per-face constants: the face view-projection + light for distance write.
	FrameConstants frame{};
	frame.viewProj = CubeFaceViewProj(face, lightPos, radius);
	frame.shadowLight = {lightPos.x, lightPos.y, lightPos.z, 1.0f / radius};
	UploadAllocation alloc =
		m_frameAllocators[m_frameIndex]->Allocate(sizeof(FrameConstants));
	std::memcpy(alloc.cpu, &frame, sizeof(frame));

	m_shadowPass = true; // DrawMesh keeps the shadow PSO bound (no per-mat swap)
	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->SetPipelineState(m_shadowPso.Get());
	m_currentPso = m_shadowPso.Get();
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->SetGraphicsRootConstantBufferView(0, alloc.gpu);
	// The shadow shaders read no textures, but bind safe defaults anyway so
	// every root table is valid.
	list->SetGraphicsRootDescriptorTable(3, m_whiteTexture->GpuHandle());
	list->SetGraphicsRootDescriptorTable(4, m_flatNormalMap->GpuHandle());
	list->SetGraphicsRootDescriptorTable(5, m_blackTexture->GpuHandle());
	list->SetGraphicsRootDescriptorTable(7, m_defaultMRTexture->GpuHandle());
}

void Renderer::EndShadows(ID3D12GraphicsCommandList* list) {
	D3D12_RESOURCE_BARRIER barriers[kShadowSlots];
	u32 count = 0;
	for (u32 slot = 0; slot < kShadowSlots; ++slot) {
		if (!m_shadowInRtState[slot]) continue;
		barriers[count++] = Transition(m_shadowCube[slot].Get(),
									   D3D12_RESOURCE_STATE_RENDER_TARGET,
									   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_shadowInRtState[slot] = false;
	}
	if (count > 0) list->ResourceBarrier(count, barriers);
}

void Renderer::DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh,
						const Mat4& world, const MaterialParams& material,
						std::span<const Mat4> palette) {
	if (material.transparent) {
		if (m_shadowPass) return; // glass casts no shadow
		const D3D12_GPU_VIRTUAL_ADDRESS paletteVa =
			palette.empty() ? 0 : UploadPalette(palette);
		if (m_transparent.size() >= kTransparentCapacity) {
			// Full: draw it now, unsorted, rather than grow (which would allocate in
			// a settled frame). Wrong order beats missing; `glass` counts it.
			++m_transparentStats.overflows;
			for (int cull = 0; cull < 2; ++cull) {
				ID3D12PipelineState* pso = m_psoGlass[m_hdrPass ? 1 : 0][cull].Get();
				list->SetPipelineState(pso);
				m_currentPso = pso;
				IssueDraw(list, mesh, world, material, paletteVa);
			}
			return;
		}
		const float dx = world._41 - m_cameraPos.x, dy = world._42 - m_cameraPos.y,
					dz = world._43 - m_cameraPos.z;
		m_transparent.push_back({&mesh, world, material, paletteVa, dx * dx + dy * dy + dz * dz});
		return;
	}

	// Authored (single-sided) meshes back-face cull; procedural geometry stays
	// double-sided. Never swap during the shadow pass (it owns m_shadowPso), and
	// only issue a state change when the PSO actually differs.
	if (!m_shadowPass) {
		ID3D12PipelineState* want =
			material.doubleSided ? (m_hdrPass ? m_psoHdr.Get() : m_pso.Get())
								 : (m_hdrPass ? m_psoCullHdr.Get() : m_psoCull.Get());
		if (want != m_currentPso) {
			list->SetPipelineState(want);
			m_currentPso = want;
		}
	}
	IssueDraw(list, mesh, world, material, palette.empty() ? 0 : UploadPalette(palette));
}

void Renderer::FlushTransparent(ID3D12GraphicsCommandList* list) {
	const size_t n = m_transparent.size();
	// The main scene is the one HDR pass; a bake or preview flush must not
	// overwrite what the world drew.
	if (m_hdrPass) {
		m_transparentStats.queued = static_cast<u32>(n);
		if (n > 0) ++m_transparentStats.frames;
	}
	m_transparentStats.peak = std::max(m_transparentStats.peak, static_cast<u32>(n));
	if (n == 0) return;
	m_transparentOrder.clear();
	for (size_t i = 0; i < n; ++i) m_transparentOrder.push_back(static_cast<u16>(i));
	// Farthest first, so each layer blends over what is behind it. Ties (the
	// submeshes of one model share its origin) keep submission order; that is
	// the tie-break rather than std::stable_sort, which may allocate a buffer.
	std::sort(m_transparentOrder.begin(), m_transparentOrder.end(), [&](u16 a, u16 b) {
		const float da = m_transparent[a].distance, db = m_transparent[b].distance;
		return da > db || (da == db && a < b);
	});
	const int hdr = m_hdrPass ? 1 : 0;
	auto draw = [&](const QueuedDraw& d, int cull) {
		ID3D12PipelineState* pso = m_psoGlass[hdr][cull].Get();
		if (pso != m_currentPso) {
			list->SetPipelineState(pso);
			m_currentPso = pso;
		}
		IssueDraw(list, *d.mesh, d.world, d.material, d.paletteVa);
	};
	// One OBJECT at a time - a run of entries at the same distance, which the
	// parts of one model are (they share its origin). Within it: every far wall
	// (front faces culled), then the liquid both ways, then every near wall, so
	// a bottle reads back wall, contents, front wall. With no liquid that is
	// simply each part's far wall then its near one.
	for (size_t r = 0; r < n;) {
		size_t end = r + 1;
		while (end < n && m_transparent[m_transparentOrder[end]].distance ==
							  m_transparent[m_transparentOrder[r]].distance)
			++end;
		for (size_t k = r; k < end; ++k) {
			const QueuedDraw& d = m_transparent[m_transparentOrder[k]];
			if (!d.material.liquid) draw(d, 0);
		}
		for (size_t k = r; k < end; ++k) {
			const QueuedDraw& d = m_transparent[m_transparentOrder[k]];
			if (d.material.liquid) {
				draw(d, 0);
				draw(d, 1);
			}
		}
		for (size_t k = r; k < end; ++k) {
			const QueuedDraw& d = m_transparent[m_transparentOrder[k]];
			if (!d.material.liquid) draw(d, 1);
		}
		r = end;
	}
	m_transparent.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadPalette(std::span<const Mat4> palette) {
	// A skinned mesh holds the same pose across all of a frame's submissions
	// (shadow faces + scene), so upload its palette once and reuse the address.
	for (const auto& [key, va] : m_paletteCache)
		if (key == palette.data()) return va;
	const size_t count = std::min<size_t>(palette.size(), kMaxSkinJoints);
	UploadAllocation skinAlloc =
		m_frameAllocators[m_frameIndex]->Allocate(kMaxSkinJoints * sizeof(Mat4));
	std::memcpy(skinAlloc.cpu, palette.data(), count * sizeof(Mat4));
	m_paletteCache.emplace_back(palette.data(), skinAlloc.gpu);
	return skinAlloc.gpu;
}

void Renderer::IssueDraw(ID3D12GraphicsCommandList* list, const Mesh& mesh,
						 const Mat4& world, const MaterialParams& material,
						 D3D12_GPU_VIRTUAL_ADDRESS paletteVa) {
	UploadAllocator& allocator = *m_frameAllocators[m_frameIndex];

	ObjectConstants object{};
	object.world = world;
	object.baseColor = material.baseColor;
	object.useTexture = material.albedo != nullptr ? 1u : 0u;
	object.skinned = paletteVa != 0 ? 1u : 0u;
	object.useNormalMap = material.normalMap != nullptr ? 1u : 0u;
	object.heightScale = material.heightScale;
	object.metallic = material.metallic;
	object.roughness = material.roughness;
	object.useMRMap = material.metalRough != nullptr ? 1u : 0u;
	object.alphaCutoff = material.alphaCutoff;
	object.emissive = {material.emissive.x, material.emissive.y, material.emissive.z,
					   material.emissiveGroove};
	object.transparent = !material.transparent ? 0u : material.liquid ? 2u : 1u;
	if (material.liquid) {
		// The level is y = liquidLevel in the mesh's space; carried into world space
		// by the world matrix's Y row (the mesh's up, so it tilts with the bottle)
		// and the point on that axis at the level.
		const float ux = world._21, uy = world._22, uz = world._23;
		const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
		const float inv = ul > 1e-12f ? 1.0f / ul : 0.0f;
		const float px = world._41 + ux * material.liquidLevel;
		const float py = world._42 + uy * material.liquidLevel;
		const float pz = world._43 + uz * material.liquidLevel;
		const Vec3 n{ux * inv, uy * inv, uz * inv};
		object.liquidPlane = {n.x, n.y, n.z, -(n.x * px + n.y * py + n.z * pz)};
	}
	UploadAllocation objAlloc = allocator.Allocate(sizeof(ObjectConstants));
	std::memcpy(objAlloc.cpu, &object, sizeof(object));
	list->SetGraphicsRootConstantBufferView(1, objAlloc.gpu);

	// The palette CBV must always be bound; reuse the object CB when unskinned.
	list->SetGraphicsRootConstantBufferView(2, paletteVa != 0 ? paletteVa : objAlloc.gpu);

	// The shadow pass reads no material textures (BeginShadowFace already bound
	// safe defaults to every table), so skip the per-draw texture binds there.
	if (!m_shadowPass) {
		const Texture* bound = material.albedo ? material.albedo : m_whiteTexture.get();
		list->SetGraphicsRootDescriptorTable(3, bound->GpuHandle());
		const Texture* boundNormal =
			material.normalMap ? material.normalMap : m_flatNormalMap.get();
		list->SetGraphicsRootDescriptorTable(4, boundNormal->GpuHandle());
		const Texture* boundMR =
			material.metalRough ? material.metalRough : m_defaultMRTexture.get();
		list->SetGraphicsRootDescriptorTable(7, boundMR->GpuHandle());
	}

	mesh.Bind(list);
	list->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}

} // namespace dungeon::gfx
