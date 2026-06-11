#include "Game/Game.h"

#include "Core/Log.h"
#include "Game/AnimatedProp.h"
#include "Game/DungeonMeshBuilder.h"
#include "Game/ProceduralSounds.h"
#include "Game/ProceduralTextures.h"

#include <cmath>
#include <format>

using namespace DirectX;

namespace dungeon::game {

Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
           gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
    : m_window(window), m_device(device), m_renderer(renderer),
      m_spriteBatch(spriteBatch), m_audio(audio),
      m_party(m_map, m_map.StartX(), m_map.StartZ()),
      m_ui(device, "", 17.0f) {
    // Dungeon geometry + procedural stone textures.
    const DungeonGeometry geo = BuildDungeonGeometry(m_map);
    m_wallMesh = std::make_unique<gfx::Mesh>(device, geo.walls);
    m_floorMesh = std::make_unique<gfx::Mesh>(device, geo.floors);
    m_ceilingMesh = std::make_unique<gfx::Mesh>(device, geo.ceilings);
    m_wallTexture = std::make_unique<gfx::Texture>(device, MakeBrickWallTexture());
    m_floorTexture = std::make_unique<gfx::Texture>(device, MakeFloorSlabTexture());
    m_ceilingTexture = std::make_unique<gfx::Texture>(device, MakeCeilingTexture());

    // Animated pillar two cells south of the start.
    m_pillarModel = BuildSerpentPillar();
    m_pillarMesh = std::make_unique<gfx::Mesh>(device, m_pillarModel.meshes[0]);
    m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
    m_pillarAnimator.Play("sway");
    m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);

    // Sound effects.
    m_sfxFootstep = MakeFootstepSound();
    m_sfxBump = MakeBumpSound();
    m_sfxTurn = MakeTurnSound();
    m_sfxClick = MakeUiClickSound();

    // Party event hooks.
    m_party.onStep = [this] { m_audio.Play(m_sfxFootstep, 0.8f); };
    m_party.onBlocked = [this] {
        m_audio.Play(m_sfxBump, 0.9f);
        m_log->AddLine("You bump into a wall.");
    };
    m_party.onTurn = [this] { m_audio.Play(m_sfxTurn, 0.6f); };

    // Camera + lights.
    m_camera.SetLens(70.0f * kPi / 180.0f,
                     static_cast<float>(window.Width()) / window.Height(), 0.05f,
                     100.0f);
    m_lights.ambient = {0.035f, 0.032f, 0.045f};
    m_lights.directional.color = {0, 0, 0}; // no sun underground

    BuildHud();

    m_log->AddLine("You descend into the dungeon...");
    m_log->AddLine("W/S move, A/D strafe, Q/E turn.");
    log::Info("Game initialized: {}x{} dungeon, {} torches", m_map.Width(),
              m_map.Height(), m_map.TorchCells().size());
}

void Game::BuildHud() {
    const float w = static_cast<float>(m_window.Width());
    const float h = static_cast<float>(m_window.Height());

    // Message log, bottom-left.
    m_log = m_ui.Add<ui::TextOutput>(gfx::Rect{16, h - 200, 520, 184});

    // Status labels, top-left.
    m_ui.Add<ui::Panel>(gfx::Rect{16, 16, 240, 64});
    m_compass = m_ui.Add<ui::Label>(gfx::Rect{28, 26, 220, 20}, "Facing: South");
    m_position = m_ui.Add<ui::Label>(gfx::Rect{28, 50, 220, 20}, "Position: -");
    m_position->dim = true;

    // Options panel, top-right.
    const float panelW = 250;
    const float px = w - panelW - 16;
    m_ui.Add<ui::Panel>(gfx::Rect{px, 16, panelW, 240});
    m_ui.Add<ui::Label>(gfx::Rect{px + 14, 26, panelW - 28, 20}, "Options");

    m_ui.Add<ui::Slider>(
        gfx::Rect{px + 14, 84, panelW - 28, 18}, "Volume", 0.0f, 1.0f, 1.0f,
        [this](float v) { m_audio.SetMasterVolume(v); });

    m_ui.Add<ui::Label>(gfx::Rect{px + 14, 116, panelW - 28, 20}, "Torchlight")->dim =
        true;
    m_ui.Add<ui::DropDown>(gfx::Rect{px + 14, 140, panelW - 28, 26},
                           std::vector<std::string>{"Warm flame", "Cold moonfire",
                                                    "Eerie emberlight"},
                           0, [this](int index) {
                               m_audio.Play(m_sfxClick, 0.5f);
                               ApplyTorchPalette(index);
                           });

    m_ui.Add<ui::Button>(gfx::Rect{px + 14, 180, (panelW - 38) / 2, 28}, "Wait",
                         [this] {
                             m_audio.Play(m_sfxClick, 0.5f);
                             m_log->AddLine("You wait. The torches gutter.");
                         });
    m_ui.Add<ui::Button>(gfx::Rect{px + 24 + (panelW - 38) / 2, 180,
                                   (panelW - 38) / 2, 28},
                         "Help", [this] {
                             m_audio.Play(m_sfxClick, 0.5f);
                             m_log->AddLine("W/S move, A/D strafe, Q/E turn.");
                             m_log->AddLine("Mouse wheel scrolls this log.");
                         });
}

void Game::ApplyTorchPalette(int index) {
    switch (index) {
    case 1:  m_torchColor = {0.45f, 0.65f, 1.0f}; m_log->AddLine("The flames turn cold and blue."); break;
    case 2:  m_torchColor = {0.55f, 1.0f, 0.45f}; m_log->AddLine("An eerie green light spreads."); break;
    default: m_torchColor = {1.0f, 0.62f, 0.28f}; m_log->AddLine("Warm firelight returns."); break;
    }
}

void Game::UpdateLights(float time) {
    m_lights.points.clear();

    // Party torch: carried slightly ahead and above the eye.
    const Vec3 eye = m_party.EyePosition();
    const float flicker =
        0.92f + 0.08f * std::sin(time * 9.0f) * std::sin(time * 13.7f + 1.3f);
    gfx::PointLight torch;
    torch.position = {eye.x, eye.y + 0.25f, eye.z};
    torch.radius = 9.0f;
    torch.color = m_torchColor;
    torch.intensity = 2.6f * flicker;
    m_lights.points.push_back(torch);

    // Static wall torches with independent flicker phases.
    int phase = 0;
    for (const auto& [tx, tz] : m_map.TorchCells()) {
        const float p = static_cast<float>(phase++) * 1.7f;
        gfx::PointLight wall;
        wall.position = m_map.CellCenter(tx, tz, kWallHeight - 0.6f);
        wall.radius = 7.0f;
        wall.color = m_torchColor;
        wall.intensity =
            1.8f * (0.9f + 0.1f * std::sin(time * 11.0f + p) * std::sin(time * 7.3f + p));
        m_lights.points.push_back(wall);
    }

    // The serpent pillar glows jade.
    gfx::PointLight glow;
    glow.position = {m_pillarPos.x, 1.3f, m_pillarPos.z};
    glow.radius = 5.0f;
    glow.color = {0.3f, 0.9f, 0.6f};
    glow.intensity = 1.2f + 0.2f * std::sin(time * 2.2f);
    m_lights.points.push_back(glow);
}

void Game::Update(float dt) {
    m_time += dt;

    // UI first so it can consume the mouse; keyboard always reaches the party.
    m_ui.Update(m_window.GetInput());
    m_party.HandleInput(m_window.GetInput());
    m_party.Update(dt);
    m_pillarAnimator.Update(dt);
    UpdateLights(m_time);
    m_audio.Update();

    // Camera follows the party.
    m_camera.SetPosition(m_party.EyePosition());
    m_camera.SetYawPitch(m_party.Yaw(), 0.0f);
    m_camera.SetLens(70.0f * kPi / 180.0f,
                     static_cast<float>(m_device.Width()) /
                         static_cast<float>(m_device.Height()),
                     0.05f, 100.0f);

    // Status readouts.
    m_compass->text = std::format("Facing: {}", Party::FacingName(m_party.Facing()));
    m_position->text =
        std::format("Position: {}, {}", m_party.GridX(), m_party.GridZ());
}

void Game::Render(ID3D12GraphicsCommandList* list) {
    m_renderer.NewFrame(m_device.FrameIndex());
    m_spriteBatch.NewFrame(m_device.FrameIndex());

    // --- 3D scene ---------------------------------------------------------
    m_renderer.BeginScene(list, m_camera, m_lights);
    const Mat4 identity = Mat4Identity();
    const Vec4 white{1, 1, 1, 1};
    m_renderer.DrawMesh(list, *m_wallMesh, identity, m_wallTexture.get(), white);
    m_renderer.DrawMesh(list, *m_floorMesh, identity, m_floorTexture.get(), white);
    m_renderer.DrawMesh(list, *m_ceilingMesh, identity, m_ceilingTexture.get(), white);

    // Animated pillar (GPU-skinned).
    Mat4 pillarWorld = Mat4Identity();
    pillarWorld._41 = m_pillarPos.x;
    pillarWorld._43 = m_pillarPos.z;
    const Vec4 jade = m_pillarModel.materials[0].baseColorFactor;
    m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, nullptr, jade,
                        m_pillarAnimator.Palette());

    // --- HUD ----------------------------------------------------------------
    m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
    m_ui.Render(m_spriteBatch);
    m_spriteBatch.End();
}

} // namespace dungeon::game
