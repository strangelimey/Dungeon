#pragma once

#include "Core/MathTypes.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Texture.h"
#include "Graphics/UploadAllocator.h"

#include <memory>
#include <vector>

namespace dungeon::gfx {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Batched 2D rendering in pixel coordinates (origin top-left). Used by the UI
// module for panels, controls, and text. Draw order is submission order.
class SpriteBatch {
public:
    explicit SpriteBatch(GraphicsDevice& device);

    void NewFrame(u32 frameIndex);
    void Begin(ID3D12GraphicsCommandList* list, u32 screenWidth, u32 screenHeight);

    // Solid-color rectangle.
    void DrawRect(const Rect& dst, const Vec4& color);
    // Textured quad; uv in normalized texture coordinates.
    void DrawSprite(const Rect& dst, const Rect& uv, const Texture& texture,
                    const Vec4& color);

    // Pixel-space clipping for scrolling panels. Pass nullptr to reset.
    void SetScissor(const Rect* rect);

    void End();

    const Texture& WhiteTexture() const { return *m_white; }

private:
    struct SpriteVertex {
        Vec2 position;
        Vec2 uv;
        Vec4 color;
    };

    void Flush();

    GraphicsDevice& m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
    std::unique_ptr<Texture> m_white;

    ID3D12GraphicsCommandList* m_list = nullptr;
    u32 m_frameIndex = 0;
    u32 m_screenWidth = 1;
    u32 m_screenHeight = 1;
    std::vector<SpriteVertex> m_pending;
    D3D12_GPU_DESCRIPTOR_HANDLE m_pendingTexture{};
    Rect m_scissor{};
    bool m_scissorActive = false;
};

} // namespace dungeon::gfx
