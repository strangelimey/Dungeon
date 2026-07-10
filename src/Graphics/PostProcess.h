// ============================================================================
// Graphics/PostProcess.h — the HDR scene target + bloom + tonemap composite.
//
// The forward pass renders linear HDR into a full-window kSceneColorFormat
// target (BeginScene binds it with the device depth buffer in place of the
// back buffer), then Resolve() runs the post chain: a bright-pass into a
// half-res target, a separable gaussian blur (two ping-pong iterations), and
// a full-screen composite onto the back buffer that adds the bloom and
// applies the ACES tonemap + gamma. The scene shader's own inline tonemap is
// disabled for this path (Renderer::BeginScene hdrTarget flag); the LDR
// preview/icon passes keep it and never come through here.
//
// Pass shaders live in assets/shaders/post.hlsl (edit + relaunch, no C++
// rebuild). Targets follow the window: BeginScene detects a size change and
// rebuilds them (WaitIdle — resizes are rare, and the recycled SRV slots must
// not be rewritten under in-flight frames).
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"

namespace dungeon::gfx {

class PostProcess {
public:
	explicit PostProcess(GraphicsDevice& device);
	~PostProcess();

	PostProcess(const PostProcess&) = delete;
	PostProcess& operator=(const PostProcess&) = delete;

	// Binds the HDR scene target + the device depth buffer (clearing both) for
	// the forward pass. Call in place of BindBackBuffer, before the scene.
	void BeginScene(ID3D12GraphicsCommandList* list);

	// Bright-pass + blur + composite onto the back buffer. Leaves the back
	// buffer bound with the full viewport — the 2D pass draws next.
	void Resolve(ID3D12GraphicsCommandList* list);

private:
	void CreateTargets(); // (re)build the size-dependent targets + views

	GraphicsDevice& m_device;
	u32 m_width = 0;
	u32 m_height = 0;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_psoBright;    // scene -> half-res bright
	ComPtr<ID3D12PipelineState> m_psoBlur;      // half-res gaussian (dir in constants)
	ComPtr<ID3D12PipelineState> m_psoComposite; // scene + bloom -> back buffer

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap; // 0 = scene, 1..2 = bloom ping-pong
	ComPtr<ID3D12Resource> m_scene;
	ComPtr<ID3D12Resource> m_bloom[2]; // half-res ping-pong pair
	SrvHandle m_sceneSrv;
	SrvHandle m_bloomSrv[2];
};

} // namespace dungeon::gfx
