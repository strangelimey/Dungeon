#pragma once

#include "Animation/Animator.h"
#include "Audio/AudioEngine.h"
#include "Game/DungeonMap.h"
#include "Game/Party.h"
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"
#include "UI/Controls.h"
#include "UI/UIContext.h"

#include <memory>

namespace dungeon::game {

// The dungeon-crawler itself: owns the map, party, lights, HUD, sounds, and
// drives the per-frame update/render flow on top of the engine modules.
class Game {
public:
    Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
         gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio);

    void Update(float dt);
    void Render(ID3D12GraphicsCommandList* list);

private:
    void BuildHud();
    void UpdateLights(float time);
    void ApplyTorchPalette(int index);

    Window& m_window;
    gfx::GraphicsDevice& m_device;
    gfx::Renderer& m_renderer;
    gfx::SpriteBatch& m_spriteBatch;
    audio::AudioEngine& m_audio;

    DungeonMap m_map;
    Party m_party;
    gfx::Camera m_camera;
    gfx::LightSet m_lights;

    // Dungeon geometry + textures.
    std::unique_ptr<gfx::Mesh> m_wallMesh;
    std::unique_ptr<gfx::Mesh> m_floorMesh;
    std::unique_ptr<gfx::Mesh> m_ceilingMesh;
    std::unique_ptr<gfx::Texture> m_wallTexture;
    std::unique_ptr<gfx::Texture> m_floorTexture;
    std::unique_ptr<gfx::Texture> m_ceilingTexture;

    // Animated showpiece (skinned + animated through the Animation module).
    assets::ModelData m_pillarModel;
    std::unique_ptr<gfx::Mesh> m_pillarMesh;
    anim::Animator m_pillarAnimator;
    Vec3 m_pillarPos{};

    // Sounds (synthesized at startup).
    assets::SoundData m_sfxFootstep;
    assets::SoundData m_sfxBump;
    assets::SoundData m_sfxTurn;
    assets::SoundData m_sfxClick;

    // HUD.
    ui::UIContext m_ui;
    ui::TextOutput* m_log = nullptr;
    ui::Label* m_compass = nullptr;
    ui::Label* m_position = nullptr;

    // Torch palette selected via the drop-down.
    Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
    float m_time = 0.0f;
};

} // namespace dungeon::game
