#include "Graphics/SpriteBatch.h"

#include "Core/Paths.h"
#include "Graphics/ShaderCompiler.h"

#include <cstring>

namespace dungeon::gfx {

SpriteBatch::SpriteBatch(GraphicsDevice& device) : m_device(device) {
    // Root signature: 0 = screen size root constants, 1 = texture table.
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 2;
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

    for (u32 i = 0; i < kFrameCount; ++i)
        m_frameAllocators[i] =
            std::make_unique<UploadAllocator>(m_device.Device(), 4 * 1024 * 1024);

    assets::ImageData white;
    white.width = white.height = 1;
    white.pixels = {255, 255, 255, 255};
    m_white = std::make_unique<Texture>(m_device, white);
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

    list->SetGraphicsRootSignature(m_rootSignature.Get());
    list->SetPipelineState(m_pso.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float screen[2] = {static_cast<float>(screenWidth),
                             static_cast<float>(screenHeight)};
    list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
}

void SpriteBatch::DrawRect(const Rect& dst, const Vec4& color) {
    DrawSprite(dst, {0, 0, 1, 1}, *m_white, color);
}

void SpriteBatch::DrawSprite(const Rect& dst, const Rect& uv, const Texture& texture,
                             const Vec4& color) {
    if (!m_list) return;
    if (m_pendingTexture.ptr != texture.GpuHandle().ptr && !m_pending.empty()) Flush();
    m_pendingTexture = texture.GpuHandle();

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
    if (m_pending.empty() || !m_list) return;

    const u64 size = m_pending.size() * sizeof(SpriteVertex);
    UploadAllocation alloc = m_frameAllocators[m_frameIndex]->Allocate(size, 16);
    std::memcpy(alloc.cpu, m_pending.data(), size);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = alloc.gpu;
    vbv.SizeInBytes = static_cast<UINT>(size);
    vbv.StrideInBytes = sizeof(SpriteVertex);
    m_list->IASetVertexBuffers(0, 1, &vbv);

    D3D12_RECT scissor;
    if (m_scissorActive) {
        scissor = {static_cast<LONG>(m_scissor.x), static_cast<LONG>(m_scissor.y),
                   static_cast<LONG>(m_scissor.x + m_scissor.w),
                   static_cast<LONG>(m_scissor.y + m_scissor.h)};
    } else {
        scissor = {0, 0, static_cast<LONG>(m_screenWidth),
                   static_cast<LONG>(m_screenHeight)};
    }
    m_list->RSSetScissorRects(1, &scissor);

    m_list->SetGraphicsRootDescriptorTable(1, m_pendingTexture);
    m_list->DrawInstanced(static_cast<UINT>(m_pending.size()), 1, 0, 0);
    m_pending.clear();
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
