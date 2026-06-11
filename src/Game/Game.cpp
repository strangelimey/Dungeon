#include "Game/Game.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/DungeonMeshBuilder.h"

#include <cmath>
#include <format>

using namespace DirectX;

namespace dungeon::game {

namespace {

assets::ModelData LoadModelOrDie(const std::string& name) {
    auto model = assets::LoadModel(paths::Asset("models\\" + name));
    DN_ASSERT(model.has_value(), model.error() + " — run AssetBaker over assets/");
    return std::move(*model);
}

assets::SoundData LoadSound(const std::string& name) {
    auto sound = assets::LoadWavFile(paths::Asset("sounds\\" + name));
    if (!sound) log::Warn("{} (running silent)", sound.error());
    return std::move(sound).value_or(assets::SoundData{});
}

} // namespace

Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
           gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
    : m_window(window), m_device(device), m_renderer(renderer),
      m_spriteBatch(spriteBatch), m_audio(audio),
      m_party(m_map, m_map.StartX(), m_map.StartZ()),
      m_ui(device, "", 17.0f) {
    LoadSurfaces();
    LoadMonsters();

    // Animated pillar two cells south of the start.
    m_pillarModel = LoadModelOrDie("pillar.gltf");
    m_pillarMesh = std::make_unique<gfx::Mesh>(device, m_pillarModel.meshes[0]);
    m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
    m_pillarAnimator.Play("sway");
    m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);

    m_sfxFootstep = LoadSound("footstep.wav");
    m_sfxBump = LoadSound("bump.wav");
    m_sfxTurn = LoadSound("turn.wav");
    m_sfxClick = LoadSound("click.wav");
    m_sfxMonster = LoadSound("monster.wav");

    m_party.onStep = [this] { m_audio.Play(m_sfxFootstep, 0.8f); };
    m_party.onBlocked = [this] {
        m_audio.Play(m_sfxBump, 0.9f);
        m_log->AddLine("You bump into a wall.");
    };
    m_party.onTurn = [this] { m_audio.Play(m_sfxTurn, 0.6f); };
    m_party.isOccupied = [this](int x, int z) {
        for (const Monster& monster : m_monsters) {
            if (monster.x == x && monster.z == z) {
                m_audio.Play(m_sfxMonster, 0.8f);
                m_log->AddLine(std::format("The {} blocks your way!",
                                           m_monsterKinds.at(monster.kind)->name));
                return true;
            }
        }
        return false;
    };

    m_camera.SetLens(70.0f * kPi / 180.0f,
                     static_cast<float>(window.Width()) / window.Height(), 0.05f,
                     100.0f);
    m_lights.ambient = {0.035f, 0.032f, 0.045f};
    m_lights.directional.color = {0, 0, 0}; // no sun underground
    // Rebuilt every frame into retained capacity — no steady-state allocation.
    m_lights.points.reserve(gfx::kMaxPointLights);

    BuildHud();

    m_log->AddLine("You descend into the dungeon...");
    m_log->AddLine("Something shuffles in the dark.");
    m_log->AddLine("W/S move, A/D strafe, Q/E turn.");
    log::Info("Game initialized: {}x{} dungeon, {} torches, {} monsters",
              m_map.Width(), m_map.Height(), m_map.TorchCells().size(),
              m_monsters.size());
}

void Game::LoadSurfaces() {
    const assets::MeshData wallBlock = LoadModelOrDie("wall_block.gltf").meshes[0];
    const assets::MeshData floorBlock = LoadModelOrDie("floor_block.gltf").meshes[0];
    const assets::MeshData ceilBlock = LoadModelOrDie("ceiling_block.gltf").meshes[0];

    auto loadSet = [&](Surface& surface, std::initializer_list<const char*> names,
                       float heightScale) {
        surface.heightScale = heightScale;
        for (const char* name : names) {
            auto albedo = assets::LoadImageFile(
                paths::Asset(std::format("textures\\{}.png", name)));
            auto normal = assets::LoadImageFile(
                paths::Asset(std::format("textures\\{}_n.png", name)));
            DN_ASSERT(albedo && normal,
                      (albedo ? normal : albedo).error() +
                          " — run AssetBaker over assets/");
            surface.albedo.push_back(std::make_unique<gfx::Texture>(m_device, *albedo));
            surface.normal.push_back(std::make_unique<gfx::Texture>(m_device, *normal));
        }
    };
    loadSet(m_walls, {"wall_brick", "wall_stone", "wall_moss"}, 0.055f);
    loadSet(m_floors, {"floor_slabs", "floor_cobble"}, 0.045f);
    loadSet(m_ceilings, {"ceiling_rough", "ceiling_cracked"}, 0.035f);

    const DungeonGeometry geo = BuildDungeonGeometry(
        m_map, wallBlock, floorBlock, ceilBlock,
        static_cast<u32>(m_walls.albedo.size()),
        static_cast<u32>(m_floors.albedo.size()),
        static_cast<u32>(m_ceilings.albedo.size()));

    auto upload = [&](Surface& surface, const std::vector<assets::MeshData>& buckets) {
        for (const assets::MeshData& bucket : buckets)
            surface.meshes.push_back(
                bucket.vertices.empty()
                    ? nullptr
                    : std::make_unique<gfx::Mesh>(m_device, bucket));
    };
    upload(m_walls, geo.walls);
    upload(m_floors, geo.floors);
    upload(m_ceilings, geo.ceilings);
}

void Game::LoadMonsters() {
    auto kindOf = [this](char kind) -> MonsterKind& {
        auto it = m_monsterKinds.find(kind);
        if (it == m_monsterKinds.end()) {
            auto assets = std::make_unique<MonsterKind>();
            switch (kind) {
            case 'S': assets->model = LoadModelOrDie("skeleton.gltf"); assets->name = "skeleton"; break;
            case 'M': assets->model = LoadModelOrDie("mummy.gltf"); assets->name = "mummy"; break;
            default:  assets->model = LoadModelOrDie("blob.gltf"); assets->name = "blob"; break;
            }
            assets->mesh = std::make_unique<gfx::Mesh>(m_device, assets->model.meshes[0]);
            it = m_monsterKinds.emplace(kind, std::move(assets)).first;
        }
        return *it->second;
    };

    int phase = 0;
    for (const DungeonMap::MonsterSpawn& spawn : m_map.MonsterSpawns()) {
        MonsterKind& kind = kindOf(spawn.kind);
        Monster monster;
        monster.kind = spawn.kind;
        monster.x = spawn.x;
        monster.z = spawn.z;
        monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
        monster.animator.Play("idle");
        monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
        m_monsters.push_back(std::move(monster));
    }
}

bool Game::MonsterAt(int x, int z) const {
    for (const Monster& monster : m_monsters)
        if (monster.x == x && monster.z == z) return true;
    return false;
}

void Game::BuildHud() {
    const float w = static_cast<float>(m_window.Width());
    const float h = static_cast<float>(m_window.Height());

    m_log = m_ui.Add<ui::TextOutput>(gfx::Rect{16, h - 200, 520, 184});

    m_ui.Add<ui::Panel>(gfx::Rect{16, 16, 240, 64});
    m_compass = m_ui.Add<ui::Label>(gfx::Rect{28, 26, 220, 20}, "Facing: South");
    m_position = m_ui.Add<ui::Label>(gfx::Rect{28, 50, 220, 20}, "Position: -");
    m_position->dim = true;

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

    const Vec3 eye = m_party.EyePosition();
    const float flicker =
        0.92f + 0.08f * std::sin(time * 9.0f) * std::sin(time * 13.7f + 1.3f);
    gfx::PointLight torch;
    torch.position = {eye.x, eye.y + 0.25f, eye.z};
    torch.radius = 9.0f;
    torch.color = m_torchColor;
    torch.intensity = 2.6f * flicker;
    m_lights.points.push_back(torch);

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

    gfx::PointLight glow;
    glow.position = {m_pillarPos.x, 1.3f, m_pillarPos.z};
    glow.radius = 5.0f;
    glow.color = {0.3f, 0.9f, 0.6f};
    glow.intensity = 1.2f + 0.2f * std::sin(time * 2.2f);
    m_lights.points.push_back(glow);
}

void Game::UpdateMonsters(float dt) {
    const Vec3 partyPos = m_party.EyePosition();
    for (Monster& monster : m_monsters) {
        monster.animator.Update(dt);

        // Face the party (blobs don't care).
        const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
        if (monster.kind != 'B')
            monster.yaw = std::atan2(partyPos.x - pos.x, partyPos.z - pos.z);

        // Announce once when the party first comes within one cell.
        const int dx = std::abs(monster.x - m_party.GridX());
        const int dz = std::abs(monster.z - m_party.GridZ());
        if (!monster.announced && std::max(dx, dz) <= 1) {
            monster.announced = true;
            const char* name = m_monsterKinds.at(monster.kind)->name;
            m_log->AddLine(std::format("A {} stirs before you!", name));
            m_audio.Play(m_sfxMonster, 0.7f);
        }
    }
}

void Game::Update(float dt) {
    m_time += dt;

    m_ui.Update(m_window.GetInput());
    m_party.HandleInput(m_window.GetInput());
    m_party.Update(dt);
    m_pillarAnimator.Update(dt);
    UpdateMonsters(dt);
    UpdateLights(m_time);

    m_camera.SetPosition(m_party.EyePosition());
    m_camera.SetYawPitch(m_party.Yaw(), 0.0f);
    m_camera.SetLens(70.0f * kPi / 180.0f,
                     static_cast<float>(m_device.Width()) /
                         static_cast<float>(m_device.Height()),
                     0.05f, 100.0f);

    // Reformat the status labels only when they actually change; per-frame
    // string formatting is needless heap churn.
    if (m_party.Facing() != m_lastFacing) {
        m_lastFacing = m_party.Facing();
        m_compass->text = std::format("Facing: {}", Party::FacingName(m_lastFacing));
    }
    if (m_party.GridX() != m_lastGridX || m_party.GridZ() != m_lastGridZ) {
        m_lastGridX = m_party.GridX();
        m_lastGridZ = m_party.GridZ();
        m_position->text = std::format("Position: {}, {}", m_lastGridX, m_lastGridZ);
    }
}

void Game::DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface) {
    const Mat4 identity = Mat4Identity();
    const Vec4 white{1, 1, 1, 1};
    for (size_t i = 0; i < surface.meshes.size(); ++i) {
        if (!surface.meshes[i]) continue;
        m_renderer.DrawMesh(list, *surface.meshes[i], identity,
                            surface.albedo[i].get(), white, {},
                            surface.normal[i].get(), surface.heightScale);
    }
}

void Game::Render(ID3D12GraphicsCommandList* list) {
    m_renderer.NewFrame(m_device.FrameIndex());
    m_spriteBatch.NewFrame(m_device.FrameIndex());

    // --- 3D scene ---------------------------------------------------------
    m_renderer.BeginScene(list, m_camera, m_lights);
    DrawSurface(list, m_walls);
    DrawSurface(list, m_floors);
    DrawSurface(list, m_ceilings);

    // Pillar.
    Mat4 pillarWorld = Mat4Identity();
    pillarWorld._41 = m_pillarPos.x;
    pillarWorld._43 = m_pillarPos.z;
    m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, nullptr,
                        m_pillarModel.materials[0].baseColorFactor,
                        m_pillarAnimator.Palette());

    // Monsters.
    for (const Monster& monster : m_monsters) {
        const MonsterKind& kind = *m_monsterKinds.at(monster.kind);
        const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
        Mat4 world;
        XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
                                    XMMatrixTranslation(pos.x, 0, pos.z));
        m_renderer.DrawMesh(list, *kind.mesh, world, nullptr,
                            kind.model.materials[0].baseColorFactor,
                            monster.animator.Palette());
    }

    // --- HUD ----------------------------------------------------------------
    m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
    m_ui.Render(m_spriteBatch);
    m_spriteBatch.End();
}

} // namespace dungeon::game
