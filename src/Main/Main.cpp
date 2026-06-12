// Composition root: constructs the engine modules, wires them together, and
// runs the frame loop. No game logic lives here.

#include "Audio/AudioEngine.h"
#include "Core/Log.h"
#include "Core/Time.h"
#include "Game/Game.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	using namespace dungeon;

#ifdef _DEBUG
	// Show a console for logs in debug builds.
	AllocConsole();
	FILE* unused = nullptr;
	freopen_s(&unused, "CONOUT$", "w", stdout);
	freopen_s(&unused, "CONOUT$", "w", stderr);
#endif

	log::Info("Dungeon starting...");

	WindowDesc desc;
	desc.title = "Dungeon";
	Window window(desc);

	gfx::GraphicsDevice device(window.Handle(), window.Width(), window.Height());
	gfx::Renderer renderer(device);
	gfx::SpriteBatch spriteBatch(device);
	audio::AudioEngine audioEngine;

	window.onResize = [&device](u32 w, u32 h) { device.Resize(w, h); };

	game::Game game(window, device, renderer, spriteBatch, audioEngine);

	Timer timer;
	const float clearColor[4] = {0.01f, 0.01f, 0.015f, 1.0f};

	while (window.PumpMessages()) {
		const float dt = timer.Tick();
		game.Update(dt);

		ID3D12GraphicsCommandList* list = device.BeginFrame(clearColor);
		game.Render(list);
		device.EndFrame();

		window.GetInput().EndFrame();

		// Esc is state-dependent (pause in-game, back out of menus); the
		// game raises this when it actually means quit.
		if (game.QuitRequested()) break;
	}

	device.WaitIdle();
	log::Info("Dungeon shutting down.");
	return 0;
}
