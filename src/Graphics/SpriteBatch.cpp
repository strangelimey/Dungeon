#include "Graphics/SpriteBatch.h"

#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <cmath>
#include <cstring>

namespace dungeon::gfx {

SpriteBatch::SpriteBatch(GraphicsDevice& device) : m_device(device) {
	// Root signature: 0 = root constants (screen size + the bar fills' clock,
	// visible to the pixel stage for bar.hlsl), 1 = texture table.
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;

	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 8; // ScreenConstants: two float4s (see Begin)
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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

	const std::string shaderPath = paths::Asset("shaders\\sprite.hlsl");
	ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};
	static_assert(sizeof(SpriteVertex) == 56, "sprite.hlsl's VSInput mirrors this layout");

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

	// Standard alpha blending; UI renders over the 3D scene without depth.
	auto& blend = pso.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	pso.DepthStencilState.DepthEnable = FALSE;

	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));

	// The bar-fill pipeline: same root signature, its own shader and vertex,
	// and PREMULTIPLIED blending, so a fill can be both a solid body (alpha 1)
	// and light added on top (alpha 0) - the emissive look, in a pass that runs
	// after bloom and tonemapping and so gets no glow from the scene's.
	const std::string barPath = paths::Asset("shaders\\bar.hlsl");
	ComPtr<ID3DBlob> barVs = CompileShader(barPath, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> barPs = CompileShader(barPath, "PSMain", "ps_5_1");
	const D3D12_INPUT_ELEMENT_DESC barLayout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,
		 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};
	pso.VS = {barVs->GetBufferPointer(), barVs->GetBufferSize()};
	pso.PS = {barPs->GetBufferPointer(), barPs->GetBufferSize()};
	pso.InputLayout = {barLayout, _countof(barLayout)};
	blend.SrcBlend = D3D12_BLEND_ONE;
	DN_HR(m_device.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_barPso)));

	for (u32 i = 0; i < kFrameCount; ++i)
		m_frameAllocators[i] =
			std::make_unique<UploadAllocator>(m_device.Device(), 4 * 1024 * 1024);

	assets::ImageData white;
	white.width = white.height = 1;
	white.pixels = {255, 255, 255, 255};
	m_white = std::make_unique<Texture>(m_device, white);

	// The pending list empties on every flush but keeps its capacity; reserve
	// enough for a busy HUD so steady-state frames never allocate.
	m_pending.reserve(8 * 1024);
	// Bar fills: four members x three bars on the party bar, five on the sheet
	// - six vertices each. Generous, for the same reason.
	m_pendingBars.reserve(64 * 6);
}

void SpriteBatch::NewFrame(u32 frameIndex) {
	m_frameIndex = frameIndex;
	m_frameAllocators[m_frameIndex]->Reset();
}

void SpriteBatch::Begin(ID3D12GraphicsCommandList* list, u32 screenWidth,
						u32 screenHeight) {
	m_list = list;
	m_screenWidth = screenWidth;
	m_screenHeight = screenHeight;
	m_scissorActive = false;
	m_mode = Mode::Sprite;
	m_textOutline = {0, 0, 0, 0}; // a context that set one and threw cannot leak it

	list->SetGraphicsRootSignature(m_rootSignature.Get());
	list->SetPipelineState(m_pso.Get());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// The second float4 is bar.hlsl's look (sprite.hlsl declares only the first).
	const float constants[8] = {static_cast<float>(screenWidth),
								static_cast<float>(screenHeight),
								m_time,
								0.0f,
								m_barBrightness,
								m_barSaturation,
								0.0f,
								0.0f};
	list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
}

void SpriteBatch::UseMode(Mode mode) {
	if (m_mode == mode) return;
	Flush();
	m_mode = mode;
	if (m_list) m_list->SetPipelineState(mode == Mode::Bar ? m_barPso.Get() : m_pso.Get());
}

void SpriteBatch::DrawRect(const Rect& dst, const Vec4& color) {
	DrawSprite(dst, {0, 0, 1, 1}, *m_white, color);
}

void SpriteBatch::DrawBarFill(const Rect& tube, const BarFill& fill) {
	if (!m_list || tube.w <= 0.0f || tube.h <= 0.0f) return;
	UseMode(Mode::Bar);
	const Vec4 params{static_cast<float>(fill.kind), fill.fraction, fill.beat, fill.seed};
	const Vec4 extra{tube.w / tube.h, tube.h, fill.pulse, 0.0f};
	const BarVertex v0{{tube.x, tube.y}, {0, 0}, fill.tint, params, extra};
	const BarVertex v1{{tube.x + tube.w, tube.y}, {1, 0}, fill.tint, params, extra};
	const BarVertex v2{{tube.x + tube.w, tube.y + tube.h}, {1, 1}, fill.tint, params, extra};
	const BarVertex v3{{tube.x, tube.y + tube.h}, {0, 1}, fill.tint, params, extra};
	m_pendingBars.push_back(v0);
	m_pendingBars.push_back(v1);
	m_pendingBars.push_back(v2);
	m_pendingBars.push_back(v0);
	m_pendingBars.push_back(v2);
	m_pendingBars.push_back(v3);
}

void SpriteBatch::DrawSprite(const Rect& dst, const Rect& uv, const Texture& texture,
							 const Vec4& color) {
	DrawSprite(dst, uv, texture.GpuHandle(), color);
}

void SpriteBatch::DrawSprite(const Rect& dst, const Rect& uv,
							 D3D12_GPU_DESCRIPTOR_HANDLE srv, const Vec4& color) {
	if (!m_list) return;
	UseMode(Mode::Sprite);
	if (m_pendingTexture.ptr != srv.ptr && !m_pending.empty()) Flush();
	m_pendingTexture = srv;

	const SpriteVertex v0{{dst.x, dst.y}, {uv.x, uv.y}, color};
	const SpriteVertex v1{{dst.x + dst.w, dst.y}, {uv.x + uv.w, uv.y}, color};
	const SpriteVertex v2{{dst.x + dst.w, dst.y + dst.h}, {uv.x + uv.w, uv.y + uv.h}, color};
	const SpriteVertex v3{{dst.x, dst.y + dst.h}, {uv.x, uv.y + uv.h}, color};
	m_pending.push_back(v0);
	m_pending.push_back(v1);
	m_pending.push_back(v2);
	m_pending.push_back(v0);
	m_pending.push_back(v2);
	m_pending.push_back(v3);
}

void SpriteBatch::DrawGlyph(const Rect& dst, const Rect& uv, const Texture& atlas,
							const Vec4& color, float radius) {
	if (!m_list) return;
	UseMode(Mode::Sprite);
	const D3D12_GPU_DESCRIPTOR_HANDLE srv = atlas.GpuHandle();
	if (m_pendingTexture.ptr != srv.ptr && !m_pending.empty()) Flush();
	m_pendingTexture = srv;

	const Vec2 glyph{radius, 0.0f};
	const SpriteVertex v0{{dst.x, dst.y}, {uv.x, uv.y}, color, m_textOutline, glyph};
	const SpriteVertex v1{{dst.x + dst.w, dst.y}, {uv.x + uv.w, uv.y}, color, m_textOutline,
						  glyph};
	const SpriteVertex v2{{dst.x + dst.w, dst.y + dst.h}, {uv.x + uv.w, uv.y + uv.h}, color,
						  m_textOutline, glyph};
	const SpriteVertex v3{{dst.x, dst.y + dst.h}, {uv.x, uv.y + uv.h}, color, m_textOutline,
						  glyph};
	m_pending.push_back(v0);
	m_pending.push_back(v1);
	m_pending.push_back(v2);
	m_pending.push_back(v0);
	m_pending.push_back(v2);
	m_pending.push_back(v3);
}

void SpriteBatch::DrawRectRotated(const Vec2& center, const Vec2& size,
								  float radians, const Vec4& color) {
	DrawSpriteRotated(center, size, radians, {0, 0, 1, 1}, *m_white, color);
}

void SpriteBatch::DrawSpriteRotated(const Vec2& center, const Vec2& size,
									float radians, const Rect& uv,
									const Texture& texture, const Vec4& color) {
	if (!m_list) return;
	UseMode(Mode::Sprite);
	if (m_pendingTexture.ptr != texture.GpuHandle().ptr && !m_pending.empty()) Flush();
	m_pendingTexture = texture.GpuHandle();

	const float c = std::cos(radians), s = std::sin(radians);
	const float hx = size.x * 0.5f, hy = size.y * 0.5f;
	// Corner offsets from the center, in unrotated local space (CW from top-left).
	const Vec2 local[4] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
	const Rect uvs{uv.x, uv.y, uv.w, uv.h};
	SpriteVertex v[4];
	for (int i = 0; i < 4; ++i) {
		v[i].position = {center.x + local[i].x * c - local[i].y * s,
						 center.y + local[i].x * s + local[i].y * c};
		v[i].color = color;
	}
	v[0].uv = {uvs.x, uvs.y};
	v[1].uv = {uvs.x + uvs.w, uvs.y};
	v[2].uv = {uvs.x + uvs.w, uvs.y + uvs.h};
	v[3].uv = {uvs.x, uvs.y + uvs.h};
	m_pending.push_back(v[0]);
	m_pending.push_back(v[1]);
	m_pending.push_back(v[2]);
	m_pending.push_back(v[0]);
	m_pending.push_back(v[2]);
	m_pending.push_back(v[3]);
}

void SpriteBatch::DrawTriangle(const Vec2& a, const Vec2& b, const Vec2& c,
							   const Vec4& color) {
	if (!m_list) return;
	UseMode(Mode::Sprite);
	const Texture& white = *m_white;
	if (m_pendingTexture.ptr != white.GpuHandle().ptr && !m_pending.empty()) Flush();
	m_pendingTexture = white.GpuHandle();
	m_pending.push_back({{a.x, a.y}, {0, 0}, color});
	m_pending.push_back({{b.x, b.y}, {1, 0}, color});
	m_pending.push_back({{c.x, c.y}, {1, 1}, color});
}

void SpriteBatch::SetScissor(const Rect* rect) {
	Flush();
	if (rect) {
		m_scissor = *rect;
		m_scissorActive = true;
	} else {
		m_scissorActive = false;
	}
}

void SpriteBatch::Flush() {
	if (!m_list) return;
	if (m_mode == Mode::Bar) {
		if (m_pendingBars.empty()) return;
		// bar.hlsl samples nothing, but the table is bound regardless so the
		// root signature is never left half-set.
		m_pendingTexture = m_white->GpuHandle();
		Submit(m_pendingBars.data(), m_pendingBars.size() * sizeof(BarVertex),
			   sizeof(BarVertex), static_cast<u32>(m_pendingBars.size()));
		m_pendingBars.clear();
		return;
	}
	if (m_pending.empty()) return;
	Submit(m_pending.data(), m_pending.size() * sizeof(SpriteVertex), sizeof(SpriteVertex),
		   static_cast<u32>(m_pending.size()));
	m_pending.clear();
}

void SpriteBatch::Submit(const void* data, u64 bytes, u32 stride, u32 count) {
	D3D12_RECT scissor;
	if (m_scissorActive) {
		scissor = {static_cast<LONG>(m_scissor.x), static_cast<LONG>(m_scissor.y),
				   static_cast<LONG>(m_scissor.x + m_scissor.w),
				   static_cast<LONG>(m_scissor.y + m_scissor.h)};
	} else {
		scissor = {0, 0, static_cast<LONG>(m_screenWidth),
				   static_cast<LONG>(m_screenHeight)};
	}
	// A BATCH THAT IS ENTIRELY SCISSORED OUT IS DROPPED, not submitted. It can
	// write nothing by definition, so this is a no-op for the image — but D3D12
	// warns on every such DrawInstanced, and a widget just below a scrolling
	// page's view produces one PER FRAME: a dialog left open wrote ten thousand
	// identical validation warnings into dungeon.log, which is the file you open
	// after a crash. The clipping itself is correct; submitting the work was not.
	if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;

	UploadAllocation alloc = m_frameAllocators[m_frameIndex]->Allocate(bytes, 16);
	std::memcpy(alloc.cpu, data, bytes);

	D3D12_VERTEX_BUFFER_VIEW vbv{};
	vbv.BufferLocation = alloc.gpu;
	vbv.SizeInBytes = static_cast<UINT>(bytes);
	vbv.StrideInBytes = stride;
	m_list->IASetVertexBuffers(0, 1, &vbv);

	m_list->RSSetScissorRects(1, &scissor);

	m_list->SetGraphicsRootDescriptorTable(1, m_pendingTexture);
	m_list->DrawInstanced(count, 1, 0, 0);
}

void SpriteBatch::End() {
	Flush();
	// Restore the full-screen scissor for whoever draws next.
	const D3D12_RECT full{0, 0, static_cast<LONG>(m_screenWidth),
						  static_cast<LONG>(m_screenHeight)};
	if (m_list) m_list->RSSetScissorRects(1, &full);
	m_list = nullptr;
}

} // namespace dungeon::gfx
