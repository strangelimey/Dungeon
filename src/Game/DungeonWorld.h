// ============================================================================
// Game/DungeonWorld.h — the 3D world and everything living in it.
//
// Owns the dungeon's two map layers (static .map structure, dynamic .ent
// spawns), the party, the monsters, the fires (sconces + braziers with
// particle effects), the serpent pillar, the batched surface geometry with
// its texture variants, the lights, and the camera — plus the per-frame
// simulation and both render passes (cube shadow maps, then the main scene).
//
// The world knows nothing about menus or the app state machine: Game decides
// when to load it (AppendLoadTasks feeds the staged loader), when to step it
// (Update only runs while playing), and when to draw it. Feedback flows out
// through onMessage (the HUD log) and the shared SoundBank.
// ============================================================================
#pragma once

#include "Animation/Animator.h"
#include "Assets/Model.h"
#include "Audio/AudioEngine.h"
#include "Game/DungeonEntities.h"
#include "Game/DungeonMap.h"
#include "Game/FireEffect.h"
#include "Game/GameSettings.h"
#include "Game/LoadQueue.h"
#include "Game/Party.h"
#include "Game/SaveGame.h"
#include "Game/SoundBank.h"
#include "Graphics/Camera.h"
#include "Graphics/ParticleBatch.h"
#include "Graphics/Renderer.h"

#include <array>
#include <flat_map>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class DungeonWorld {
public:
	DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
				 audio::AudioEngine& audio, const SoundBank& sounds,
				 const GameSettings& settings);

	// Appends the dungeon's load tasks (blocks, textures, batched meshes,
	// pillar, monsters, fires, dust) to the staged loader. Order matters:
	// textures register their variant counts before the geometry task buckets
	// cells by variant. The texture work is split one task per material — the
	// scanned sets are the bulk of the load (~300 MB at Ultra), and
	// per-material tasks keep the progress bar moving through them.
	void AppendLoadTasks(LoadQueue& queue);

	// Hot-swaps the dungeon meshes (and textures, when crossing a resolution
	// boundary) for the settings' new quality tier, if they are already
	// built. Drains the GPU first — it may still be reading the old data.
	void ApplyQuality(bool textureResChanged);

	// "Start New Game": snaps the party home, re-arms the monster
	// announcements, and resets the torch palette (which speaks via
	// onMessage — the caller clears the log right after, as before).
	void ResetForNewGame();

	// One simulation step: party input/movement, animators, monster
	// announcements, lights (with shadow-slot assignment), camera, and the
	// fire particles (gathered back-to-front for the smoke blend).
	// acceptInput=false simulates the world but ignores party movement keys —
	// used while the dev console is open (the world keeps running, the party
	// stays put). Everything else (physics, monsters, lights, particles)
	// updates regardless.
	void Update(const Input& input, float dt, float time, bool acceptInput = true);

	// Per-frame arena rotation for the world-owned batches (safe pre-load).
	void NewFrame(u32 frameIndex);

	void RenderShadowMaps(ID3D12GraphicsCommandList* list);
	void RenderScene(ID3D12GraphicsCommandList* list);

	// Torchlight palette (the HUD dropdown): 0 warm, 1 cold blue, 2 eerie
	// green. Announces the change through onMessage.
	void SetTorchPalette(int index);

	Party& GetParty() { return m_party; }
	const DungeonMap& Map() const { return m_map; }
	const DungeonEntities& Entities() const { return m_entities; }
	size_t MonsterCount() const { return m_monsters.size(); }

	// --- fog of war (dynamic/save-side state, not in DungeonMap) -------------
	// Whether a cell has been revealed (the party has stood on it or an
	// adjacent cell). The map overlay draws only seen cells. This belongs to
	// the dynamic layer — a future save serializes it, never the .map file.
	bool IsSeen(int x, int z) const;

	// --- save / load --------------------------------------------------------
	// Gathers the world's dynamic state into `out`: party pose, torch palette,
	// fog of war (whole), and per-monster grid overrides (only monsters that
	// have moved from their .ent spawn — empty today, monsters don't roam yet).
	// The character roster is the Game's half, filled separately.
	void CaptureState(SaveData& out) const;
	// Applies a loaded save onto a freshly-built level (call after
	// ResetForNewGame): snaps the party, restores fog + palette, and moves any
	// overridden entities by id. Out-of-range/unknown ids are ignored.
	void ApplyState(const SaveData& in);

	// --- map editor seam (driven by MapView) --------------------------------
	// Paints a cell to a new type, revealing it and rebuilding the affected
	// surface geometry. Drains the GPU, so it is an interactive edit, not a
	// per-frame call. Structural only: fixtures, turbidity, and decorations
	// are not recomputed (matches DungeonMap::SetCell).
	void EditCell(int x, int z, Cell cell);

	// --- dev console hooks ---------------------------------------------------
	// "kind @ x,z" for each live monster.
	std::vector<std::string> MonsterList() const;
	// Point lights submitted this frame (after UpdateLights).
	size_t ActiveLightCount() const { return m_lights.points.size(); }
	// Camera vertical FOV in degrees (clamped); UpdateCamera applies it.
	void SetFov(float degrees);
	float Fov() const { return m_fovDegrees; }
	// Toggle the shadow pass (off = lights still lit, just unshadowed).
	void SetShadowsEnabled(bool on) { m_shadowsEnabled = on; }
	bool ShadowsEnabled() const { return m_shadowsEnabled; }
	// Toggle volumetric dust (off feeds the renderer clear air).
	void SetDustEnabled(bool on) { m_dustEnabled = on; }
	bool DustEnabled() const { return m_dustEnabled; }

	// HUD log feedback (bump lines, monster announcements, palette flavor).
	// Set before play starts; the party/monster callbacks route through it.
	std::function<void(const std::string&)> onMessage;

private:
	// A texture variant set: parallel albedo / normal+height pairs plus the
	// batched mesh bucket per variant.
	struct Surface {
		std::vector<std::unique_ptr<gfx::Texture>> albedo;
		std::vector<std::unique_ptr<gfx::Texture>> normal;
		std::vector<std::unique_ptr<gfx::Mesh>> meshes; // null where bucket empty
		float heightScale = 0.0f;
	};

	// Per-kind monster assets (shared) and per-instance state. Kinds are
	// entity type names from the .ent file ("skeleton" loads skeleton.gltf).
	struct MonsterKind {
		assets::ModelData model; // must outlive the Animators pointing into it
		std::unique_ptr<gfx::Mesh> mesh;
		std::string name;
	};
	struct Monster {
		const MonsterKind* kind = nullptr; // points into m_monsterKinds (stable)
		int id = -1; // source Entity::id, for save overrides
		int x, z;
		int spawnX = 0, spawnZ = 0; // .ent baseline, for the save diff
		float yaw = 0.0f;
		bool announced = false;
		anim::Animator animator;
	};

	// Fires: wall sconces (at 'T' cells, mounted on the adjacent wall) and
	// floor braziers ('F' cells). Each carries a flickering point light at
	// its flame origin and a FireEffect particle simulation.
	struct Fire {
		bool brazier = false;
		Mat4 world;        // prop transform
		Vec3 flamePos;     // particle + light origin
		float phase = 0;   // flicker phase
		FireEffect effect;
	};

	// --- loading ---------------------------------------------------------------
	// One surface's texture set + the height scale its parallax uses. The three
	// SurfaceDefs (walls/floors/ceilings) are the single source of those scales,
	// shared by the staged loader and the quality hot-swap.
	struct SurfaceDef {
		Surface& surface;
		std::span<const std::string> names;
		float heightScale;
	};
	std::array<SurfaceDef, 3> SurfaceDefs();

	void LoadDungeonBlocks();      // loads the worn block set for the quality tier
	void LoadSurfaceMaterial(Surface& surface, const std::string& name);
	void LoadTextureSet(const SurfaceDef& def); // resets, then loads the set
	void LoadAllSurfaceTextures(); // reloads every set (quality hot-swap)
	void BuildDungeonMeshes();
	void LoadMonsters();
	void BuildFires();
	void BuildTurbidityMap();

	// --- per-frame --------------------------------------------------------------
	void UpdateCamera();
	void UpdateLights(float time);
	void UpdateMonsters(float dt);
	void AssignShadowSlots();

	// Reveals a cell and its eight neighbors in the fog-of-war set.
	void MarkSeen(int x, int z);
	// Re-bakes the surface meshes after a map edit (WaitIdle + rebuild).
	void RebuildGeometry();

	// --- rendering --------------------------------------------------------------
	// All 3D draw calls, shared verbatim by the shadow and main passes.
	void SubmitSceneGeometry(ID3D12GraphicsCommandList* list);
	void DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface);

	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	audio::AudioEngine& m_audio;
	const SoundBank& m_sounds;
	const GameSettings& m_settings;

	DungeonMap m_map;           // static layer (.map): structure, fixtures
	DungeonEntities m_entities; // dynamic layer (.ent): monsters, items, buttons
	Party m_party;
	std::vector<u8> m_seen;     // fog of war, parallel to map cells (1 = revealed)
	gfx::Camera m_camera;
	gfx::LightSet m_lights;
	// Scratch reused by AssignShadowSlots: (distance², light index), sorted
	// nearest-first each frame into retained capacity.
	std::vector<std::pair<float, size_t>> m_shadowCandidates;
	gfx::Atmosphere m_atmosphere; // per-cell air turbidity (dust)
	std::unique_ptr<gfx::Texture> m_turbidityMap;

	Surface m_walls;
	Surface m_floors;
	Surface m_ceilings;
	// Worn block geometry, one mesh per texture variant (same order as the
	// surface texture sets), held between the load and mesh-build tasks.
	std::vector<assets::MeshData> m_wallBlocks, m_floorBlocks, m_ceilingBlocks;

	assets::ModelData m_pillarModel;
	std::unique_ptr<gfx::Mesh> m_pillarMesh;
	anim::Animator m_pillarAnimator;
	Vec3 m_pillarPos{};

	std::flat_map<std::string, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	std::vector<Fire> m_fires;
	std::unique_ptr<gfx::Mesh> m_sconceMesh;
	std::unique_ptr<gfx::Mesh> m_brazierMesh;
	Vec4 m_sconceColor{1, 1, 1, 1};
	Vec4 m_brazierColor{1, 1, 1, 1};
	std::unique_ptr<gfx::ParticleBatch> m_particleBatch;
	std::vector<gfx::ParticleInstance> m_particleScratch;

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	int m_torchPalette = 0; // index behind m_torchColor (saved/restored)

	// Dev console toggles (see the hooks above).
	float m_fovDegrees = 70.0f;
	bool m_shadowsEnabled = true;
	bool m_dustEnabled = true;
};

} // namespace dungeon::game
