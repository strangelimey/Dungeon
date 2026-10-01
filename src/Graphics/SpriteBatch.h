// ============================================================================
// Graphics/SpriteBatch.h — batched 2D pass for the UI.
//
// Pixel-space quads (origin top-left), alpha-blended, no depth — drawn AFTER
// the 3D scene so the HUD overlays it. Quads accumulate into one vertex list
// and flush as a single draw; a flush is forced whenever the texture or the
// scissor rect changes, so submission order == draw order. Vertices live in
// the per-frame UploadAllocator arena (nothing persists between frames).
// A second pipeline draws the procedural resource-bar fills (DrawBarFill,
// assets/shaders/bar.hlsl); moving between the two is a flush like any other.
// Text rendering sits one level up: ui::Font turns glyphs into DrawSprite
// calls against its atlas texture.
// ============================================================================
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

// A PROCEDURAL bar fill (assets/shaders/bar.hlsl): the animated fluid inside a
// resource bar's glass tube. Each kind has its own look and motion; Solid is a
// flat tint (the food/water placeholder). The shader paints the whole tube -
// the filled part AND the empty glass past it - so the caller passes the
// tube's rect, not the filled width.
enum class BarKind : u32 { Solid = 0, Health = 1, Stamina = 2, Mana = 3 };
struct BarFill {
	BarKind kind = BarKind::Solid;
	float fraction = 1.0f; // 0..1, how full the bar is
	float beat = 0.0f;     // heartbeat phase within the current beat, 0..1 (Health)
	float seed = 0.0f;     // per-bar offset, so two bars never move in lockstep
	Vec4 tint{1, 1, 1, 1}; // Solid's colour (the animated kinds carry their own)
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
	// As above, but from a raw shader-visible SRV handle — for offscreen render
	// targets (e.g. the editor's 3D model preview) that aren't a Texture.
	void DrawSprite(const Rect& dst, const Rect& uv, D3D12_GPU_DESCRIPTOR_HANDLE srv,
					const Vec4& color);

	// Quad rotated `radians` clockwise about `center` (screen Y is down, so
	// positive angles turn toward +X→+Y). `size` is the unrotated width/height.
	// Used for direction markers the axis-aligned path can't express (the map's
	// party/entity facing arrows).
	void DrawSpriteRotated(const Vec2& center, const Vec2& size, float radians,
						   const Rect& uv, const Texture& texture, const Vec4& color);
	void DrawRectRotated(const Vec2& center, const Vec2& size, float radians,
						 const Vec4& color);

	// Solid-color triangle (the map's party/entity facing markers). Winding is
	// irrelevant — the UI PSO culls nothing.
	void DrawTriangle(const Vec2& a, const Vec2& b, const Vec2& c,
					  const Vec4& color);

	// One procedural bar fill over `tube` (see BarFill). Batched like sprites:
	// consecutive fills share one draw, and switching between fills and sprites
	// flushes, so submission order is still draw order - a frame drawn after
	// its fill lands on top of it.
	void DrawBarFill(const Rect& tube, const BarFill& fill);

	// The clock the bar fills animate by, in seconds. REAL time, set once a
	// frame before Begin: not the world's clock, which runs 60x while resting
	// and stops in the pause menu.
	void SetTime(float seconds) { m_time = seconds; }

	// One glyph of OUTLINED text (ui::Font::Draw is the only caller). `dst` and
	// `uv` are the glyph's box GROWN by `radius` px on every side, so the ring
	// has room; sprite.hlsl dilates the atlas coverage by `radius` and puts the
	// glyph over a ring in TextOutline()'s colour. Same pipeline as sprites, so
	// text and faces still batch together.
	void DrawGlyph(const Rect& dst, const Rect& uv, const Texture& atlas,
				   const Vec4& color, float radius);

	// The outline every glyph drawn from here on carries (alpha 0 = none). A
	// skinned UIContext sets it for its own draw pass and puts the old one back
	// (UIContext::Render), so text on stone is outlined at all ~110 draw sites
	// with no per-site code, and flat mode, the editor and the console are not.
	void SetTextOutline(const Vec4& color) { m_textOutline = color; }
	const Vec4& TextOutline() const { return m_textOutline; }

	// Pixel-space clipping for scrolling panels. Pass nullptr to reset.
	void SetScissor(const Rect* rect);

	void End();

	const Texture& WhiteTexture() const { return *m_white; }

private:
	// `outline` is zero for every sprite but an outlined glyph (DrawGlyph);
	// glyph.x is that glyph's outline radius in px.
	struct SpriteVertex {
		Vec2 position;
		Vec2 uv;
		Vec4 color;
		Vec4 outline{0, 0, 0, 0};
		Vec2 glyph{0, 0};
	};
	// bar.hlsl's vertex: uv runs 0..1 across the TUBE; params = (kind,
	// fraction, beat, seed); extra = (tube aspect w/h, tube height in px).
	struct BarVertex {
		Vec2 position;
		Vec2 uv;
		Vec4 tint;
		Vec4 params;
		Vec4 extra;
	};
	enum class Mode { Sprite, Bar };

	void Flush();
	void UseMode(Mode mode);
	// Uploads `bytes` of vertices and draws them under the current scissor
	// (nothing is submitted when the scissor clips everything away).
	void Submit(const void* data, u64 bytes, u32 stride, u32 count);

	GraphicsDevice& m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	ComPtr<ID3D12PipelineState> m_barPso;
	std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
	std::unique_ptr<Texture> m_white;

	ID3D12GraphicsCommandList* m_list = nullptr;
	u32 m_frameIndex = 0;
	u32 m_screenWidth = 1;
	u32 m_screenHeight = 1;
	float m_time = 0.0f;
	Vec4 m_textOutline{0, 0, 0, 0};
	Mode m_mode = Mode::Sprite;
	std::vector<SpriteVertex> m_pending;
	std::vector<BarVertex> m_pendingBars;
	D3D12_GPU_DESCRIPTOR_HANDLE m_pendingTexture{};
	Rect m_scissor{};
	bool m_scissorActive = false;
};

} // namespace dungeon::gfx
