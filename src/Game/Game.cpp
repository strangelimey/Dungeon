// ============================================================================
// Game/Game.cpp — see Game.h. The module classes do the real work; this file
// is construction wiring, the staged-load task lists, and the state machine.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/AllocTrack.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Graphics/DisplayEnum.h"
#include "Graphics/Texture.h"
#include "Platform/PerfMonitor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace dungeon::game {

namespace {

// Builds the font library with assets/fonts/fonts.cat already applied.
//
// It is a function rather than two statements in the constructor body because
// the roles must be set BEFORE the UI contexts first resolve one: m_ui is a
// member, so its constructor runs before any constructor body could configure
// the library, and each context would otherwise build an atlas for the fallback
// face and abandon it a frame later.
//
// The library cannot read its own config: Catalog/Serialize live in this lib,
// which sits ABOVE UI in the layer order. Same split as DungeonMap taking
// FixtureTypes because the map has no catalog access.
ui::FontLibrary MakeFontLibrary(gfx::GraphicsDevice& device) {
	ui::FontLibrary fonts(device);
	Catalog cat;
	cat.Load(paths::Asset("fonts\\fonts.cat"));
	for (int i = 0; i < ui::kFontRoleCount; ++i) {
		const auto role = static_cast<ui::FontRole>(i);
		const CatalogEntry* entry = cat.Find(ui::FontRoleName(role));
		if (!entry) continue;
		ui::FaceSpec spec;
		// An absent or empty `file` leaves the path empty, which the library
		// reads as "system fallback" — how every role ships until the audition
		// (docs/fonts.md Phase 4) picks a face.
		if (const std::string file = entry->Get("file", ""); !file.empty())
			spec.path = paths::Asset(file);
		spec.scale = entry->GetFloat("scale", 1.0f);
		fonts.SetFace(role, std::move(spec));
	}
	return fonts;
}

} // namespace

// ============================================================================
// Construction — cheap setup only; the heavy asset work is queued as load
// tasks that run one per frame behind the loading screen (see Update).
// ============================================================================
Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
	: m_window(window), m_device(device), m_renderer(renderer),
	  m_spriteBatch(spriteBatch), m_audio(audio), m_postProcess(device),
	  m_project(Project::Load(paths::Asset("projects\\dungeon-demo"))),
	  m_world(device, renderer, audio, m_sounds, m_settings, m_project, m_threads),
	  m_fonts(MakeFontLibrary(device)),
	  m_ui(window, device, spriteBatch, audio, m_sounds, m_settings,
		   m_characters, m_fonts),
	  m_mapView(device, m_world, m_settings, m_fonts),
	  m_mapEditor(m_mapView, m_world, m_settings),
	  m_console(m_fonts, m_threads),
	  m_modelPreview(device, 512),
	  m_assetDialog(device, window),
	  m_monsterDialog(device, m_fonts), m_balanceDialog(device, m_fonts),
	  m_levelSettingsDialog(device, m_fonts), m_typeDialog(device, m_fonts),
	  m_assetPicker(device, m_fonts),
	  m_entityInspector(device, m_fonts), m_fixtureInspector(device, m_fonts),
	  m_propInspector(device, m_fonts), m_doorInspector(device, m_fonts),
	  m_buttonInspector(device, m_fonts), m_nicheInspector(device, m_fonts),
	  m_projectileInspector(device, m_fonts), m_inspectPicker(device, m_fonts),
	  m_previewParticles(device) {
	m_mapView.SetEditor(&m_mapEditor); // the view drives the editor in Editor mode
	// The editor's header save buttons: Save = write every edited level (what the
	// savemap console command does); To source = also copy the project into the
	// repo tree. Feedback goes through the world's message channel.
	m_mapView.onSave = [this](bool toSource) {
		if (!m_gameLoaded) return; // no world to save yet
		const std::vector<std::string> saved = m_world.SaveAllLevels();
		bool ok = !saved.empty();
		if (ok && toSource) ok = SyncProjectToSource();
		if (!m_world.onMessage) return;
		if (!ok) {
			m_world.onMessage(loc::Tr("map.save.failed"));
			return;
		}
		std::string list;
		for (const std::string& s : saved) list += (list.empty() ? "" : ", ") + s;
		m_world.onMessage(
			loc::Format(toSource ? "map.save.synced" : "map.save.done", list));
	};
	// The editor's Balance header button → the combat-tuning dialog. Edits
	// apply LIVE (the world's Balance is the one every formula reads, and the
	// derived resource maxima follow); Save also writes the two catalogs back
	// to the project (the asset copy — To source syncs them to the repo).
	m_mapView.onBalance = [this] { m_balanceDialog.Open(m_world.GetBalance()); };
	m_balanceDialog.onApply = [this](const Balance& b) {
		m_world.GetBalance() = b;
		m_world.RecomputePartyMaxima();
	};
	m_balanceDialog.onSave = [this](const Balance& b) {
		m_world.GetBalance() = b;
		m_world.RecomputePartyMaxima();
		b.Save(m_project.balance, m_project.attacks);
		const bool ok =
			m_project.balance.Save(m_project.CatalogPath("balance.cat"),
								   "Balance: the attack-formula knob sheet "
								   "([formula] block; docs/combat.md).") &&
			m_project.attacks.Save(m_project.CatalogPath("attacks.cat"),
								   "Attacks: per-melee-verb numbers "
								   "(damage/accuracy/speed multipliers); "
								   "identity (damage type) is C++ (Balance.h).");
		if (m_world.onMessage)
			m_world.onMessage(loc::Tr(ok ? "map.balance.saved" : "map.save.failed"));
	};
	// The editor toolbar's Level button → the per-level atmosphere dialog,
	// opened on the VIEWED level's effective values (a browsed level's come
	// from its stash-backed snapshot). Edits preview live only while that
	// level is the active one — a browsed level isn't on screen to preview.
	// Save commits to the level's map/stash; the values persist as the .map
	// `atmosphere` record on the next savemap (the toolbar Save).
	m_mapView.onLevelSettings = [this] {
		float dust, haze, ambient;
		DungeonWorld::EffectiveAtmosphere(m_mapView.ViewedMap(), dust, haze, ambient);
		m_levelSettingsDialog.Open(m_mapView.ViewedLevel(), dust, haze, ambient);
	};
	m_levelSettingsDialog.onApply = [this](float dust, float haze, float ambient) {
		if (m_levelSettingsDialog.Level() != m_world.CurrentLevel()) return;
		m_world.SetDustDensity(dust);
		m_world.SetHazeAmbient(haze);
		m_world.SetAmbientScale(ambient);
	};
	m_levelSettingsDialog.onSave = [this](float dust, float haze, float ambient) {
		m_world.SetLevelAtmosphere(m_levelSettingsDialog.Level(), dust, haze, ambient);
		if (m_world.onMessage)
			m_world.onMessage(loc::Format("map.level.applied",
										  m_levelSettingsDialog.Level()));
	};
	// The editor toolbar's [+] button: mint a fresh level (files + manifest)
	// and hand the stem back so the view jumps onto the new canvas.
	m_mapView.onNewLevel = [this] { return CreateNewLevel(); };
	// The Level dialog's inline name edit → the full rename flow.
	m_levelSettingsDialog.onRename = [this](const std::string& oldStem,
											const std::string& newStem) {
		return RenameLevel(oldStem, newStem);
	};
	m_settings.Load();
	ApplyLanguage(false); // strings must exist before any UI builds
	m_audio.SetMasterVolume(m_settings.volume);
	m_device.SetPresentInterval(m_settings.presentInterval);
	m_world.GetParty().SetKeys(m_settings.moveKeys);
	m_world.GetParty().SetLook(m_settings.look);
	m_world.GetParty().SetHeadBob(m_settings.headBob);

	m_characters = CreateDefaultParty();
	ApplyMemberColors(); // the settings palette wins over the authored defaults
	ApplyPartySpeed();
	m_world.SetRoster(&m_characters); // combat drains these; reset in place
	m_ui.SetHitSplats(&m_hitSplats);  // stable address; LoadHitSplats fills it in
	m_ui.SetItemIcons(&m_itemIcons);    // stable; LoadItemIcons fills it in
	m_ui.SetItemWeights(&m_itemWeights); // stable; LoadItemIcons fills it in
	m_ui.SetItemCategories(&m_itemCategories); // stable; LoadItemIcons fills it in
	m_ui.SetSlotIcons(&m_slotIcons);     // stable; LoadItemIcons fills it in
	m_ui.SetHeldItem(&m_heldItem);    // cursor icon reads the held catalog id

	WireModuleCallbacks();
	RegisterDevCommands();

	m_ui.BuildStaticUi();
	BuildBootLoadTasks();

	// Honor a saved borderless/exclusive display mode now that the window and
	// device exist (windowed at the default size needs nothing).
	ApplyDisplaySettings();
}

Game::~Game() {
	// The AudioEngine outlives Game (constructed before it in Main), but its
	// playback references SoundBank sample memory owned HERE — silence every
	// voice before that memory goes away, or the mixer thread reads freed
	// buffers on a quit mid-sound.
	m_audio.StopAll();
}

// ============================================================================
// Module wiring — the world↔UI/editor callback graph and the dev-console
// command table, split out of the constructor (which is otherwise just member
// init) so each is browsable on its own.
// ============================================================================
void Game::BuildBootLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_loadQueue.Add(loc::Tr("load.echoes"), [this] { m_sounds.Load(); }, "sounds");
	m_loadQueue.Add(loc::Tr("load.title_art"), [this] { m_ui.LoadTitleArt(); }, "title art");
}

void Game::BuildGameLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_world.AppendLoadTasks(m_loadQueue);
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadPortraits(); }, "portraits");
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadHitSplats(); }, "hit splats");
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadItemIcons(); }, "item icons");
	m_loadQueue.Add(
		loc::Tr("load.hud"),
		[this] {
			m_ui.BuildHud();
			log::Info("Game loaded: {}x{} dungeon, {} torches, {} monsters",
					  m_world.Map().Width(), m_world.Map().Height(),
					  m_world.Map().Sconces().size(), m_world.MonsterCount());
		},
		"hud");
}

void Game::BeginLevelTransition(const std::string& stem, int x, int z,
								Direction facing, bool stashCurrent) {
	m_world.BeginLevelLoad(stem, stashCurrent); // swap + reset per-level state now
	m_loadQueue.Clear();          // re-stage only the world rebuild (portraits /
	m_loadQueue.SetDoneLabel(loc::Tr("load.done")); // HUD persist across levels)
	m_world.AppendLoadTasks(m_loadQueue);
	m_pendingLevelX = x;
	m_pendingLevelZ = z;
	m_pendingLevelFacing = facing;
	// Ordinary transitions arrive square-on; a save load overrides this right
	// after the call (the only path that carries a free-look offset across levels).
	m_pendingLookYaw = m_pendingLookPitch = 0.0f;
	m_pendingLooking = false;
	m_state = AppState::LoadingLevel;
	m_stateFrameMark = m_framesRendered;
}

// Launches the AssetBaker command for the current bake step (P4c). Models are a
// single import-model; texture sets import the maps (step 0) then rebake the
// worn block meshes that sample them (step 1).
bool Game::RunLoadTasks() {
	const bool wasDone = m_loadQueue.Done();
	if (m_framesRendered > m_stateFrameMark) m_loadQueue.RunOne();
	const bool done = m_loadQueue.Done();
	if (done && !wasDone) LogLoadStats(); // the frame the last task landed
	return done;
}

// The load-time counterpart to the steady-state rule: staged loading is ALLOWED
// to allocate, but until now nobody had measured how much. Sorted by time, since
// that is what a player feels; the allocation columns say where the time went.
void Game::LogLoadStats(bool echoToConsole) {
	const std::vector<LoadQueue::TaskStat>& stats = m_loadQueue.Stats();
	if (stats.empty()) {
		if (echoToConsole) m_console.Print("no load has run yet");
		return;
	}
	auto say = [this, echoToConsole](std::string line) {
		if (echoToConsole) m_console.Print(line);
		log::Info("{}", line);
	};

	double totalMs = 0.0;
	u64 totalAllocs = 0, totalBytes = 0;
	for (const LoadQueue::TaskStat& s : stats) {
		totalMs += s.ms;
		totalAllocs += s.allocs;
		totalBytes += s.bytes;
	}

	// Sorted by cost, not run order — the table is read top-down for suspects.
	std::vector<const LoadQueue::TaskStat*> byCost;
	byCost.reserve(stats.size());
	for (const LoadQueue::TaskStat& s : stats) byCost.push_back(&s);
	std::ranges::sort(byCost, [](const LoadQueue::TaskStat* a, const LoadQueue::TaskStat* b) {
		return a->ms > b->ms;
	});

	const ProcessMemory mem = QueryProcessMemory();
	say(std::format("--- load: {} tasks, {:.0f} ms, {} allocs, {:.1f} MB requested "
					"(working set {:.0f} MB, peak {:.0f} MB) ---",
					stats.size(), totalMs, totalAllocs,
					static_cast<double>(totalBytes) / (1024.0 * 1024.0),
					mem.workingSetMB, mem.peakWorkingSetMB));
	for (const LoadQueue::TaskStat* s : byCost)
		say(std::format("  {:>8.1f} ms  {:>8} allocs  {:>9.2f} MB  {}", s->ms, s->allocs,
						static_cast<double>(s->bytes) / (1024.0 * 1024.0), s->name));
}

void Game::LoadPortraits() {
	m_portraitTextures.clear();
	for (Character& member : m_characters) {
		std::string stem = "portrait_" + member.name;
		std::ranges::transform(stem, stem.begin(), [](char c) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		});
		auto texture =
			TryLoadTextureFile(m_device, paths::Asset("ui\\" + stem));
		if (!texture)
			log::Warn("missing {}.png — falling back to the initial tile", stem);
		member.portrait = texture.get();
		m_portraitTextures.push_back(std::move(texture));
	}
}

void Game::LoadHitSplats() {
	// Three severity icons, drawn over a struck member's portrait. Committed
	// source PNGs under assets/ui/. A missing icon just leaves that severity null.
	static const char* kStems[3] = {"hit_splat_small", "hit_splat_med",
									"hit_splat_hard"};
	for (int i = 0; i < 3; ++i) {
		m_hitSplatTextures[i] =
			TryLoadTextureFile(m_device, paths::Asset(std::string("ui\\") + kStems[i]));
		if (!m_hitSplatTextures[i])
			log::Warn("missing {}.png — no hit splat for that severity", kStems[i]);
		m_hitSplats.icon[i] = m_hitSplatTextures[i].get();
	}
}

// Builds a tiny solid-colour RGBA texture (a flat placeholder icon). The mip
// chain is generated on the spot (fine for a small runtime texture); sRGB so the
// tint matches the linear CategoryTint colour seen on the floor mesh.
static std::unique_ptr<gfx::Texture> MakeSolidIcon(gfx::GraphicsDevice& device,
												   const Vec4& color) {
	constexpr u32 kSize = 16;
	assets::ImageData img;
	img.width = kSize;
	img.height = kSize;
	img.pixels.resize(static_cast<size_t>(kSize) * kSize * 4);
	const auto enc = [](float c) {
		return static_cast<u8>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
	};
	const u8 rgba[4] = {enc(color.x), enc(color.y), enc(color.z), enc(color.w)};
	for (size_t p = 0; p < img.pixels.size(); p += 4) {
		img.pixels[p + 0] = rgba[0];
		img.pixels[p + 1] = rgba[1];
		img.pixels[p + 2] = rgba[2];
		img.pixels[p + 3] = rgba[3];
	}
	return std::make_unique<gfx::Texture>(device, img, /*srgb=*/true);
}

void Game::LoadItemIcons() {
	// One element-tinted icon per symbol, keyed by the rune's catalog id
	// (rune_fire → rune_icon_fire). PNG only (like the splats). Drawn on the
	// cursor when a tablet is held, and in the hand slots / inventory.
	for (u32 i = 0; i < kSymbolCount; ++i) {
		const auto sym = static_cast<SpellSymbol>(i);
		const std::string id = RuneItemId(sym);
		m_runeIconTextures[i] = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\rune_icon_{}", SymbolId(sym))));
		if (!m_runeIconTextures[i])
			log::Warn("missing rune_icon_{}.png — no cursor icon", SymbolId(sym));
		m_itemIcons.byType[id] = m_runeIconTextures[i].get();
	}
	// Non-rune items: a model item uses its baked 3D thumbnail (rendered once by
	// DungeonWorld; the same texture feeds every slot/grid/cursor instance);
	// model-less items keep a generated solid category-tint placeholder.
	for (const CatalogEntry* defp : m_project.AllItems()) {
		const CatalogEntry& def = *defp;
		const std::string category = def.Get("category", "misc");
		if (category == "rune") continue; // runes use their element PNG above
		if (const gfx::Texture* model = m_world.ItemIconFor(def.id)) {
			m_itemIcons.byType[def.id] = model;
			continue;
		}
		m_itemIconPlaceholders.push_back(MakeSolidIcon(m_device, CategoryTint(category)));
		m_itemIcons.byType[def.id] = m_itemIconPlaceholders.back().get();
	}
	// Carry weights + categories for every catalog item (load sum; pack check).
	m_itemWeights.byType.clear();
	m_itemCategories.byType.clear();
	m_itemCategories.capacityByType.clear();
	m_itemCategories.acceptsByType.clear();
	m_itemCategories.holdableTypes.clear();
	// Splits a whitespace/comma list (the catalog `accepts` field) into tokens.
	const auto splitList = [](const std::string& s) {
		std::vector<std::string> out;
		std::string tok;
		for (char c : s) {
			if (c == ' ' || c == '\t' || c == ',') {
				if (!tok.empty()) { out.push_back(tok); tok.clear(); }
			} else {
				tok += c;
			}
		}
		if (!tok.empty()) out.push_back(tok);
		return out;
	};
	for (const CatalogEntry* defp : m_project.AllItems()) {
		const CatalogEntry& def = *defp;
		m_itemWeights.byType[def.id] = def.GetFloat("weight", 0.0f);
		m_itemCategories.byType[def.id] = def.Get("category", "misc");
		m_itemCategories.capacityByType[def.id] =
			static_cast<int>(def.GetFloat("capacity", 0.0f));
		m_itemCategories.acceptsByType[def.id] = splitList(def.Get("accepts", ""));
		if (def.GetBool("holdable", false))
			m_itemCategories.holdableTypes.insert(def.id);
	}

	// Equipment-slot outline silhouettes (slot_<type>.png), the ghost behind an
	// empty doll slot. PNG only, like the rune icons; a missing one just draws no
	// ghost for that slot.
	for (const char* type : {"head", "body", "legs", "feet", "cloak", "amulet",
							 "hand", "ring"}) {
		auto tex = TryLoadTextureFile(
			m_device, paths::Asset(std::format("ui\\slot_{}", type)));
		if (!tex) {
			log::Warn("missing slot_{}.png — no empty-slot outline", type);
			continue;
		}
		m_slotIconTextures.push_back(std::move(tex));
		m_slotIcons.byType[type] = m_slotIconTextures.back().get();
	}
}

// ============================================================================
// State transitions
// ============================================================================

void Game::ResetRoster() {
	// Element-wise so the addresses the party-bar panels and the sheet point at
	// stay valid, keeping each slot's loaded portrait (the defaults carry null).
	const std::vector<Character> fresh = CreateDefaultParty();
	for (size_t i = 0; i < m_characters.size() && i < fresh.size(); ++i) {
		const gfx::Texture* portrait = m_characters[i].portrait;
		m_characters[i] = fresh[i];
		m_characters[i].portrait = portrait;
	}
	// CreateDefaultParty seeds the derived maxima at k=1; re-derive under the
	// project's live balance knobs (fresh members are at full, so top them up).
	m_world.RecomputePartyMaxima();
	for (Character& member : m_characters) {
		member.health = member.maxHealth;
		member.stamina = member.maxStamina;
		member.mana = member.maxMana;
	}
	ApplyMemberColors(); // the settings palette wins over the authored defaults
}

void Game::ApplyMemberColors() {
	for (size_t i = 0; i < m_characters.size() && i < kMemberColorCount; ++i)
		m_characters[i].portraitColor = m_settings.memberColors[i];
}

void Game::StartNewGame() {
	m_world.ResetForNewGame();
	ResetRoster(); // fresh members carry empty inventories + no known symbols
	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// A new game always begins on the first level; if a prior game left the world
	// on a deeper level, load the first one fresh (the loading screen handles it).
	const std::string first =
		m_project.levels.empty() ? std::string("level1") : m_project.levels.front();
	if (m_world.CurrentLevel() != first) {
		BeginLevelTransition(first, -1, -1, Direction::South, /*stashCurrent=*/false);
		log::Info("New game started (loading {})", first);
		return; // the LoadingLevel done-handler resumes play
	}

	m_ui.ClearLog();
	m_ui.AddLogLine(loc::Tr("log.descend"));
	m_ui.AddLogLine(loc::Tr("log.shuffle"));
	m_ui.AddLogLine(m_settings.MoveKeysHelp());

	m_ui.ResetHudStatus();
	m_state = AppState::Playing;
	log::Info("New game started");
}

void Game::SaveGame(const std::string& name) {
	if (!m_gameLoaded) {
		log::Warn("SaveGame: no game loaded");
		return;
	}
	SaveData data;
	data.name = name;
	data.timestamp = std::format("{:%Y-%m-%d %H:%M:%S}",
								 std::chrono::floor<std::chrono::seconds>(
									 std::chrono::system_clock::now()));
	// An item on the cursor is party-level state — save it as such, leaving the
	// live session's held item untouched (restored to the cursor on load).
	if (m_heldItem) data.heldItem = *m_heldItem;

	m_world.CaptureState(data);
	for (const Character& member : m_characters) {
		SaveData::CharState c{member.health, member.maxHealth, member.stamina,
							  member.maxStamina, member.mana, member.maxMana,
							  member.knownSymbols};
		// equipment[] now includes the weapon hands (EquipSlot::LeftHand/RightHand).
		for (const ItemSlot& s : member.inventory.equipment) c.equipment.push_back(s.typeId);
		// Pack row: the container ids + each pack's contents + the selected pack.
		c.selectedPack = member.inventory.selectedPack;
		for (const Pack& p : member.inventory.packs) {
			c.packTypes.push_back(p.typeId);
			std::vector<std::string> items;
			for (const ItemSlot& s : p.contents) items.push_back(s.typeId);
			c.packContents.push_back(std::move(items));
		}
		// The member's remembered per-hand, per-item default uses (hand
		// left-click action) and each hand's cast-recency list.
		for (size_t hand = 0; hand < 2; ++hand) {
			for (const auto& [item, cmd] : member.useDefaults[hand])
				c.useDefaults[hand].emplace_back(item, cmd);
			c.mruSpells[hand] = member.spellMru[hand];
		}
		// Spells learned by first successful cast.
		for (const std::string& id : member.learnedSpells)
			c.learnedSpells.push_back(id);
		// Active status effects — the EFFECT ID names the kind now (older saves
		// stored the category token; EffectBook::FindLegacy maps those forward
		// on load). The name key still rides along for readability only: an
		// effect names itself through its kind.
		for (const fx::Inst& e : member.effects)
			c.effects.push_back({std::string(e.Id()), SymbolId(e.school),
								 e.timeLeft, e.duration, e.magnitude, e.source,
								 std::string(e.NameKey())});
		// Skills, stat-creep pools, and the five attributes (they grow now).
		for (const auto& [id, xp] : member.skillXp) c.skills.emplace_back(id, xp);
		for (const auto& [stat, progress] : member.statProgress)
			c.statProgress.emplace_back(stat, progress);
		c.hasAttrs = true;
		c.strength = member.strength;
		c.dexterity = member.dexterity;
		c.vitality = member.vitality;
		c.willpower = member.willpower;
		c.intelligence = member.intelligence;
		// The resource bases (v17) — the authored half of the derived maxima.
		c.hasBases = true;
		c.baseHealth = member.baseHealth;
		c.baseStamina = member.baseStamina;
		c.baseMana = member.baseMana;
		c.dead = member.dead; // the overkill flag (v18)
		data.characters.push_back(std::move(c));
	}
	WriteSave(data, SaveSlotPath(name));
}

bool Game::LoadGame(const std::string& path) {
	if (!m_gameLoaded) {
		log::Warn("LoadGame: dungeon not loaded yet");
		return false;
	}
	auto data = ReadSave(path);
	if (!data) {
		log::Warn("LoadGame: could not read {}", path);
		return false;
	}

	// Rebuild the baseline (party home, fog cleared, monsters at spawn, palette
	// reset), then lay the save on top.
	m_world.ResetForNewGame();
	ResetRoster();
	// Restore the cursor-held tablet (empty = nothing carried).
	if (!data->heldItem.empty()) m_heldItem = data->heldItem;
	else m_heldItem.reset();
	for (size_t i = 0; i < m_characters.size() && i < data->characters.size(); ++i) {
		const SaveData::CharState& c = data->characters[i];
		m_characters[i].health = c.health;     m_characters[i].maxHealth = c.maxHealth;
		m_characters[i].stamina = c.stamina;   m_characters[i].maxStamina = c.maxStamina;
		m_characters[i].mana = c.mana;         m_characters[i].maxMana = c.maxMana;
		m_characters[i].knownSymbols = c.knownSymbols;
		// Inventory (ResetRoster gave a fresh one; lay the save's items back in).
		Inventory& inv = m_characters[i].inventory;
		for (size_t e = 0; e < c.equipment.size() && e < static_cast<size_t>(kEquipCount); ++e)
			inv.equipment[e].typeId = c.equipment[e];
		// Restore the pack row (container ids + each pack's contents + selection).
		for (size_t p = 0; p < c.packTypes.size() && p < static_cast<size_t>(kPackRowSlots); ++p) {
			inv.packs[p].typeId = c.packTypes[p];
			const std::vector<std::string> items =
				p < c.packContents.size() ? c.packContents[p] : std::vector<std::string>{};
			inv.packs[p].contents.assign(items.size(), {});
			for (size_t s = 0; s < items.size(); ++s)
				inv.packs[p].contents[s].typeId = items[s];
		}
		if (c.selectedPack >= 0 && c.selectedPack < kPackRowSlots)
			inv.selectedPack = c.selectedPack;
		// Restore the remembered per-hand default uses (ResetRoster left the
		// maps empty) and each hand's cast-recency list.
		for (size_t hand = 0; hand < 2; ++hand) {
			for (const auto& [item, cmd] : c.useDefaults[hand])
				m_characters[i].useDefaults[hand][item] = cmd;
			m_characters[i].spellMru[hand] = c.mruSpells[hand];
		}
		// And the spells learned by casting (likewise reset to empty).
		for (const std::string& id : c.learnedSpells)
			m_characters[i].learnedSpells.insert(id);
		// Restore active status effects (pre-v13 saves carry none). The token
		// is an effect id; FindLegacy also accepts the pre-effects-system
		// category tokens ("ward" + school, "poison", ...). An unresolved one
		// — a newer save, or an effect this project's classes don't have — is
		// skipped, not misread.
		for (const SaveData::EffectState& e : c.effects) {
			SpellSymbol school = SpellSymbol::Fire;
			ParseSymbol(e.school, school);
			const fx::EffectKind* kind = m_world.Effects().FindLegacy(e.id, school);
			if (!kind || e.time <= 0.0f) continue;
			m_characters[i].effects.push_back({kind, school, e.magnitude, e.time,
											   std::max(e.duration, e.time),
											   e.source});
		}
		// Skills, stat-creep pools, and the grown attributes (pre-v15 saves
		// carry none — skills fresh, archetype attributes stand).
		for (const auto& [id, xp] : c.skills) m_characters[i].skillXp[id] = xp;
		for (const auto& [stat, progress] : c.statProgress)
			m_characters[i].statProgress[stat] = progress;
		if (c.hasAttrs) {
			m_characters[i].strength = c.strength;
			m_characters[i].dexterity = c.dexterity;
			m_characters[i].vitality = c.vitality;
			m_characters[i].willpower = c.willpower;
			m_characters[i].intelligence = c.intelligence;
		}
		// Resource bases (v17): the maxima DERIVE from bases + stats, so with
		// the attributes settled, either restore the saved bases or back-solve
		// them from the saved maxima (a pre-v17 save reproduces its maxima
		// exactly under unchanged knobs). RecomputeMaxima then re-derives —
		// current values arrived above and clamp/carry as usual.
		const Balance& bal = m_world.GetBalance();
		Character& member = m_characters[i];
		if (c.hasBases) {
			member.baseHealth = c.baseHealth;
			member.baseStamina = c.baseStamina;
			member.baseMana = c.baseMana;
		} else {
			member.baseHealth =
				c.maxHealth - bal.kHealth * static_cast<float>(member.vitality);
			member.baseStamina =
				c.maxStamina - bal.kStamina * 0.5f *
								   static_cast<float>(member.strength +
													  member.vitality);
			member.baseMana =
				c.maxMana - bal.kMana * 0.5f *
								static_cast<float>(member.intelligence +
												   member.willpower);
		}
		member.RecomputeMaxima(bal.kHealth, bal.kStamina, bal.kMana);
		// The exhausted latch is a live transient (not saved) — re-derive it
		// from the restored bar so a save made mid-exhaustion resumes winded.
		member.exhausted = member.stamina <= 0.0f;
		// The overkill flag rides the save; the stabilize clock is transient
		// (an unconscious member starts their safe count fresh on load).
		member.dead = c.dead;
		member.stabilize = 0.0f;
	}
	m_world.ApplyState(*data); // fills the per-level store + party pose/torch

	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// Route to the saved level. If it is the one already active, restore its
	// live state inline; otherwise load it (arriving at the saved pose, without
	// stashing the throwaway baseline) and let the loader finish the restore.
	if (m_world.CurrentLevel() != data->currentLevel) {
		m_ui.ClearLog();
		BeginLevelTransition(data->currentLevel, data->partyX, data->partyZ,
							 static_cast<Direction>(data->partyFacing),
							 /*stashCurrent=*/false);
		// Carry the free-look offset across the level rebuild (PlacePartyAt would
		// otherwise leave the party square-on) — applied once the load finishes.
		m_pendingLookYaw = data->lookYaw;
		m_pendingLookPitch = data->lookPitch;
		m_pendingLooking = data->looking;
		log::Info("Loaded game from {} (loading {})", path, data->currentLevel);
		return true;
	}

	// Same level: ApplyState already re-layered the look offset (parked at the
	// saved angle); mirror its looking flag into the RMB tracker so it clears
	// cleanly if the button isn't actually held.
	m_looking = m_world.GetParty().IsLooking();
	m_world.ApplyActiveSnapshot(); // restore the active level's fog + entity diff
	m_ui.ClearLog(); // SetTorchPalette logged a line during ApplyState
	m_ui.AddLogLine(loc::Tr("log.descend"));
	const Party& party = m_world.GetParty();
	m_ui.ResetHudStatus();
	m_ui.SetHudStatus(party);
	m_state = AppState::Playing;
	log::Info("Loaded game from {}", path);
	return true;
}

void Game::OpenCharacterSheet(size_t index) {
	m_audio.Play(m_sounds.click, 0.5f);
	m_ui.ShowSheet(index);
	m_state = AppState::CharacterSheet;
}

// The party moves as fast as its slowest member: take the roster minimum and
// hand it to the Party, which scales its step and turn rates by it.
void Game::ApplyPartySpeed() {
	float slowest = m_characters.empty() ? 1.0f : m_characters[0].moveSpeed;
	for (const Character& member : m_characters)
		slowest = std::min(slowest, member.moveSpeed);
	m_world.GetParty().SetSpeed(slowest);
}

void Game::ApplyLanguage(bool rebuild) {
	if (!m_pendingLanguage.empty()) {
		m_settings.language = m_pendingLanguage;
		m_pendingLanguage.clear();
		m_settings.Save();
	}
	if (!loc::LoadFile(paths::Asset("lang\\" + m_settings.language + ".lang"))) {
		if (m_settings.language != "en")
			loc::LoadFile(paths::Asset("lang\\en.lang"));
	} else if (m_settings.language != "en") {
		// Surface translation drift: any en.lang key this language lacks
		// renders as the raw key in the UI, so name them in the log.
		loc::LogMissingKeys(paths::Asset("lang\\en.lang"));
	}
	if (rebuild) m_ui.RebuildForLanguage();
}

void Game::DrawBusyNotice(const std::string& text, float dw, float dh) {
	const ui::Font& font = m_mapView.Font();
	const float w = font.MeasureWidth(text);
	const gfx::Rect back{(dw - w) * 0.5f - 14.0f, (dh - font.Height()) * 0.5f - 10.0f,
						 w + 28.0f, font.Height() + 20.0f};
	m_spriteBatch.DrawRect(back, {0.0f, 0.0f, 0.0f, 0.75f});
	font.Draw(m_spriteBatch, text, (dw - w) * 0.5f, (dh - font.Height()) * 0.5f,
			  m_settings.theme.accent);
}

void Game::SetQuality(Quality quality) {
	if (quality == m_settings.quality) return;
	const std::string oldTextureSuffix = m_settings.TextureSuffix();
	m_settings.quality = quality;
	const bool textureResChanged = oldTextureSuffix != m_settings.TextureSuffix();
	// The light budget follows the tier; re-point the Video tab's dropdown at it.
	m_settings.maxPointLights = GameSettings::QualityLightBudget(quality);
	m_ui.SyncMaxLights();
	m_settings.Save();
	m_world.ApplyQuality(textureResChanged);
}

void Game::ApplyDisplaySettings() {
	// Resolve the active adapter's outputs so we can position a borderless window
	// or target a monitor for exclusive full-screen.
	const std::vector<gfx::AdapterInfo> adapters = gfx::EnumerateAdapters();
	const gfx::AdapterInfo* active = nullptr;
	for (const gfx::AdapterInfo& a : adapters)
		if (a.luid == m_device.AdapterLuid()) {
			active = &a;
			break;
		}
	const int out = m_settings.displayOutput;
	const gfx::OutputInfo* output =
		(active && out >= 0 && out < static_cast<int>(active->outputs.size()))
			? &active->outputs[static_cast<size_t>(out)]
			: nullptr;

	switch (m_settings.fullscreen) {
	case gfx::FullscreenMode::Windowed: {
		const u32 w = m_settings.displayWidth > 0 ? static_cast<u32>(m_settings.displayWidth)
												  : m_window.Width();
		const u32 h = m_settings.displayHeight > 0
						  ? static_cast<u32>(m_settings.displayHeight)
						  : m_window.Height();
		m_device.SetFullscreen(false, 0, 0, 0); // drop any exclusive state first
		m_window.SetWindowed(w, h);
		break;
	}
	case gfx::FullscreenMode::Borderless: {
		m_device.SetFullscreen(false, 0, 0, 0);
		if (output)
			m_window.SetBorderless(output->x, output->y,
								   static_cast<u32>(output->width),
								   static_cast<u32>(output->height));
		break;
	}
	case gfx::FullscreenMode::Exclusive: {
		u32 w = static_cast<u32>(m_settings.displayWidth);
		u32 h = static_cast<u32>(m_settings.displayHeight);
		if ((w == 0 || h == 0) && output) { // default to the monitor's native size
			w = static_cast<u32>(output->width);
			h = static_cast<u32>(output->height);
		}
		m_device.SetFullscreen(true, static_cast<u32>(out > 0 ? out : 0), w, h);
		break;
	}
	}
}

void Game::RestartApp() {
	// Leave any exclusive full-screen so the new process can claim the display.
	m_device.SetFullscreen(false, 0, 0, 0);
	if (!m_restart.Start("\"" + paths::ExecutableDir() + "\\Dungeon.exe\""))
		log::Warn("Could not relaunch the game for the adapter change");
	m_quitRequested = true;
}

// ============================================================================
// The state machine
// ============================================================================

// Adaptive thread governor: when the frame runs over its time budget, ease every
// background worker's cadence down (freeing CPU); otherwise drift it back toward
// full speed. Off unless enabled via `governor auto`. Hysteresis comes from
// ASYMMETRIC easing — shed load fast, recover slowly — rather than a deadband
// that holds the current scale (which could pin it throttled forever once a
// transient spike dropped it). The scale change is pushed with wakeNow=false so
// the per-frame updates don't wake every worker (which would itself cause a tick
// burst). NOTE: keys off whole-frame time, which can be GPU-bound — a coarse
// heuristic, hence opt-in.
void Game::UpdateGovernor(float dt) {
	if (!m_governorAuto) return;
	const float fps = m_console.Fps();
	const float frameMs = fps > 1.0f ? 1000.0f / fps : 1000.0f;
	const float ratio = frameMs / m_governorTargetMs;
	const float desired = ratio > 1.15f ? 0.25f : 1.0f; // over budget vs recover
	// Throttle down fast (~0.17s), recover slowly (~2s): sustained load stays
	// throttled, but it always trends back to full when the pressure clears.
	const float rate = desired < m_governorScale ? dt * 6.0f : dt * 0.5f;
	m_governorScale += (desired - m_governorScale) * std::min(1.0f, rate);
	m_governorScale = std::clamp(m_governorScale, 0.25f, 1.0f);
	if (std::abs(m_governorScale - m_threads.GlobalThrottle()) > 0.005f)
		m_threads.SetGlobalThrottle(m_governorScale, /*wakeNow=*/false);
}

// Is the frame now starting one the "steady-state frames allocate nothing" rule
// actually covers? Playing, with nothing that legitimately builds or rebuilds in
// flight — and it must have been that way for a WARM-UP, because the first
// frames after a load or after an overlay closes are still settling (first-time
// icon bakes, a shadow cube filling in, a widget tree laying out).
bool Game::SteadyStateFrame() {
	constexpr u32 kWarmupFrames = 120;
	const bool quiet = m_state == AppState::Playing && !m_console.IsOpen() &&
					   !m_mapView.IsOpen() && !m_baking && m_pendingLanguage.empty() &&
					   !m_pendingQuality;
	m_steadyFrames = quiet ? m_steadyFrames + 1 : 0;
	return m_steadyFrames > kWarmupFrames;
}

// One `alloctest` window: spend the budget only on frames that actually armed,
// so the load, the warm-up and the console being open cost the test nothing. The
// verdict is the guard's own stats, differenced across the window.
void Game::UpdateAllocTest(float dt, bool steady) {
	m_allocTestDeadline -= dt;
	if (steady) {
		m_allocTestRemaining -= dt;
		++m_allocTestFrames;
	}
	if (m_allocTestRemaining > 0.0f && m_allocTestDeadline > 0.0f) return;

	const alloc::GuardStats now = alloc::Stats();
	const u64 violations = now.violations - m_allocTestStart.violations;
	const u64 badFrames = now.framesViolating - m_allocTestStart.framesViolating;
	const bool timedOut = m_allocTestRemaining > 0.0f;
	// One machine-readable line: tools\AllocTest.ps1 greps for it and nothing
	// else, so the format is part of the contract.
	const std::string line =
		std::format("alloctest RESULT={} frames={} violations={} violating_frames={}{}",
					timedOut ? "SKIP" : (violations == 0 ? "PASS" : "FAIL"),
					m_allocTestFrames, violations, badFrames,
					timedOut ? " reason=never_reached_a_steady_frame" : "");
	log::Info("{}", line);
	m_console.Print(line);
	if (violations > 0)
		m_console.Print("call sites are in dungeon.log (each reported once per session)");
	m_allocTestRemaining = 0.0f;
}

void Game::Update(float dt) {
	const bool steady = SteadyStateFrame();
	alloc::ArmFrame(steady);
	if (m_allocTestRemaining > 0.0f) UpdateAllocTest(dt, steady);
	// `allocpoke`: a deliberate violation, so the guard can be seen to catch one.
	if (m_allocPokeRemaining > 0.0f) {
		m_allocPokeRemaining -= dt;
		m_pokeScratch = std::make_unique<u32>(m_framesRendered);
	}

	const float wdt = dt * m_timeScale; // world dt (dev console `timescale`)
	m_time += wdt;

	// A language picked last frame applies now, before any widget updates —
	// the rebuild destroys every widget, so none may be mid-callback.
	if (!m_pendingLanguage.empty()) ApplyLanguage(true);
	// A quality tier picked last frame applies now — the frame that showed the
	// "applying" notice has presented, so the multi-second texture reload stalls
	// with that notice on screen instead of on a frozen settings page.
	if (m_pendingQuality) {
		const Quality q = *m_pendingQuality;
		m_pendingQuality.reset();
		SetQuality(q);
	}
	// A Video-tab adapter/monitor change last frame repopulates the settings page
	// now, for the same reason: the rebuild destroys the dropdown that triggered it.
	m_ui.ApplyPendingVideoRebuild();

	m_ui.UpdateFonts(dt);
	if (m_previewMesh) m_previewOrbit += dt * 0.6f; // spin the editor 3D preview

	// Poll the asset bake (P4c): non-blocking, so the "baking…" dialog stays
	// responsive. A texture import runs a second step (worn meshes) before it's
	// done; on success FinishBake writes the catalog entry.
	if (m_baking && !m_bake.Running()) {
		if (m_bake.ExitCode() != 0) {
			// Surface the failure where the user is looking: the dialog stays up
			// with the exit code so the form can be fixed and retried (the full
			// baker output is in dungeon.log next to the exe).
			log::Warn("AssetBaker failed (exit {})", m_bake.ExitCode());
			m_baking = false;
			if (m_restyleBake) { m_restyleBake = false; m_typeDialog.Close(); }
			else m_assetDialog.SetError(loc::Format("newasset.err.bake",
													m_bake.ExitCode()));
		} else if (m_bakeReq.textureSet && m_bakeStep == 0) {
			m_bakeStep = 1; // textures imported — now rebake worn block meshes
			if (!StartBakeStep()) {
				m_baking = false;
				m_assetDialog.SetBusy(false);
			}
		} else if (m_restyleBake) {
			// Surface restyle rebake done: swap the new worn geometry in live.
			m_world.ReloadDungeonBlocks();
			m_restyleBake = false;
			m_baking = false;
			m_typeDialog.Close();
			if (m_world.onMessage) m_world.onMessage(loc::Tr("map.wallstyle.applied"));
		} else {
			FinishBake();
			m_baking = false;
		}
		// Reset to defaults (relief < 0 = the baker's own per-kind amplitude).
		if (!m_baking) { m_bakeWear = 1.0f; m_bakeColumns = true; m_bakeRelief = -1.0f; }
	}

	const Input& input = m_window.GetInput();

	// The dev console toggles with `~` and overlays any state. While it is
	// open it captures input (so the party can't move) but the world keeps
	// simulating — it does NOT pause the game. The FPS sampler ticks every
	// frame regardless. While a staged load is mid-flight the world is only
	// partially built (the HUD log, meshes, and monsters arrive task by task),
	// so command EXECUTION is gated off — a `cast`/`save`/`quality` then would
	// reach into objects a later task creates (this crashed: 0xc0000005 in the
	// HUD log line a cast raises before BuildHud has run). The console itself
	// stays usable for the perf/thread panels.
	const bool loading = m_state == AppState::Loading ||
						 m_state == AppState::LoadingGame ||
						 m_state == AppState::LoadingLevel;
	const bool consoleWasOpen = m_console.IsOpen();
	if (input.WasKeyPressed(VK_OEM_3)) m_console.Toggle();
	m_console.SetCommandsEnabled(!loading);
	m_console.Update(input, dt, static_cast<float>(m_window.Width()),
					 static_cast<float>(m_window.Height()));
	UpdateGovernor(dt); // adaptive thread throttle (no-op unless `governor auto`)
	// The console owns the whole frame's input if it was open at the start (or
	// just opened) — so the very keystroke that closes it (Esc or `~`) never
	// also reaches the pause menu / HUD this frame. Owning input is NOT a
	// pause: a playing world keeps simulating here, and a loading state falls
	// through to its case below so the task queue keeps pumping (an open
	// console used to stall the load, holding the world half-built) — only
	// its Esc-to-quit is console-gated.
	const bool consoleOwnsInput = m_console.IsOpen() || consoleWasOpen;
	if (consoleOwnsInput && !loading) {
		if (m_state == AppState::Playing) {
			m_world.Update(input, wdt, m_time, /*acceptInput=*/false);
			Party& party = m_world.GetParty();
			m_ui.SetHudStatus(party);
		}
		return;
	}

	switch (m_state) {
	case AppState::Loading:
		if (!consoleOwnsInput && input.WasKeyPressed(VK_ESCAPE)) m_quitRequested = true;
		if (RunLoadTasks()) m_state = AppState::Menu;
		return;

	case AppState::Menu:
		// The menu sits on baked title art; nothing in the world simulates.
		// Esc backs out of settings, or quits from the landing list — unless
		// a key-bind box is armed, where Esc just cancels the capture.
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive()) {
			if (!m_ui.CloseSettingsPage()) m_quitRequested = true;
		}
		m_ui.UpdateMenu(input);
		return;

	case AppState::LoadingGame:
		if (!consoleOwnsInput && input.WasKeyPressed(VK_ESCAPE)) m_quitRequested = true;
		if (RunLoadTasks()) {
			m_gameLoaded = true;
			if (!m_pendingLoadPath.empty()) {
				const std::string path = std::exchange(m_pendingLoadPath, {});
				if (!LoadGame(path)) { // bad/corrupt file: fall back to the menu
					m_state = AppState::Menu;
					m_ui.ResetToMainPage();
				}
			} else {
				StartNewGame(); // sets AppState::Playing
			}
		}
		return;

	case AppState::LoadingLevel:
		if (RunLoadTasks()) {
			// Restore this level's saved fog/progress (if visited before — the
			// monsters now exist for the entity diff), then place the party.
			m_world.ApplyActiveSnapshot();
			int px = m_pendingLevelX, pz = m_pendingLevelZ;
			if (px < 0) { // sentinel: arrive at the new level's start cell
				px = m_world.Map().StartX();
				pz = m_world.Map().StartZ();
			}
			m_world.PlacePartyAt(px, pz, m_pendingLevelFacing);
			// Re-layer the saved free-look offset on the placed party (a save load
			// onto a different level; orthogonal for ordinary transitions). The
			// offset parks at the saved angle; mirror the looking flag into the RMB
			// tracker so it clears cleanly if the button isn't actually held.
			m_world.GetParty().SetLookState(m_pendingLookYaw, m_pendingLookPitch,
											m_pendingLooking);
			m_looking = m_pendingLooking;
			m_ui.ClearLog();
			m_state = AppState::Playing;
		}
		return;

	case AppState::Paused:
		// The world is frozen — only the pause menu (or the shared settings
		// page) updates. Esc backs out of settings (but an armed key-bind box
		// gets it first, as its cancel), or resumes play.
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive()) {
			m_audio.Play(m_sounds.click, 0.5f);
			if (!m_ui.CloseSettingsPage()) m_state = AppState::Playing;
			return;
		}
		m_ui.UpdatePause(input);
		return;

	case AppState::CharacterSheet:
		// Frozen like Paused; only the sheet page updates. Esc resumes.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_audio.Play(m_sounds.click, 0.5f);
			m_state = AppState::Playing;
			return;
		}
		m_ui.UpdateSheet(input);
		return;

	case AppState::Playing:
		break;
	}

	// --- Playing -------------------------------------------------------------
	// The asset-creation dialog is modal over the editor: while it is up it owns
	// input and the world/overlay are frozen.
	// The asset picker sits ABOVE every dialog that opens it (the type editor and
	// the create dialog), so it comes first: while it is up it owns the mouse and
	// the keyboard (its search box types).
	if (m_assetPicker.IsOpen()) {
		m_assetPicker.Update(input, static_cast<float>(m_window.Width()),
							 static_cast<float>(m_window.Height()), dt);
		return;
	}
	if (m_assetDialog.IsOpen()) {
		m_assetDialog.Update(input, static_cast<float>(m_window.Width()),
							 static_cast<float>(m_window.Height()), dt);
		return;
	}
	// The combat-tuning dialog is likewise modal over the editor.
	if (m_balanceDialog.IsOpen()) {
		m_balanceDialog.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		return;
	}
	// The per-level settings dialog is likewise modal over the editor.
	if (m_levelSettingsDialog.IsOpen()) {
		m_levelSettingsDialog.Update(input, static_cast<float>(m_window.Width()),
									 static_cast<float>(m_window.Height()));
		return;
	}
	// The per-type catalog editor is likewise modal over the editor.
	if (m_typeDialog.IsOpen()) {
		m_typeDialog.Update(input, static_cast<float>(m_window.Width()),
							static_cast<float>(m_window.Height()));
		return;
	}
	// The monster-config dialog is likewise modal over the editor.
	if (m_monsterDialog.IsOpen()) {
		m_monsterDialog.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		// Drive the live preview: (re)build the Animator when the selected type/clip
		// changes, then advance it looping so Render can blit the current pose.
		const std::string& type = m_monsterDialog.SelectedType();
		const std::string& clip = m_monsterDialog.PreviewClip();
		if (clip.empty()) {
			m_previewMonMesh = nullptr;
		m_previewMonSubs.clear();
			m_previewClip.clear();
		} else if (type != m_previewType) {
			// New type: (re)build the Animator over its skeleton/clips + cache the
			// mesh/material/scale/yaw. A same-type clip switch is just a Play (below).
			const auto d = m_world.MonsterPreviewFor(type);
			m_previewMonMesh = d.mesh;
			m_previewMonMat = d.material;
			m_previewMonSubs = d.subs; // multi-material rigs preview every piece
			m_previewMonScale = d.modelScale;
			m_previewMonYaw = d.modelYaw;
			m_previewAnim = anim::Animator(d.skeleton, d.clips);
			m_previewAnim.Play(clip, /*loop*/ true);
			m_previewType = type;
			m_previewClip = clip;
		} else if (clip != m_previewClip) {
			m_previewAnim.Play(clip, /*loop*/ true); // same rig, just switch clips
			m_previewClip = clip;
		}
		if (m_previewMonMesh) m_previewAnim.Update(dt);
		return;
	}
	// The multi-object inspect chooser is modal over the editor (it precedes the
	// inspector it opens).
	if (m_inspectPicker.IsOpen()) {
		m_inspectPicker.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		return;
	}
	// The per-instance entity inspector is likewise modal over the editor.
	if (m_entityInspector.IsOpen()) {
		m_entityInspector.Update(input, static_cast<float>(m_window.Width()),
								 static_cast<float>(m_window.Height()));
		if (m_entityInspector.Preview().skeleton) m_previewAnim.Update(dt); // idle loop
		return;
	}
	// The per-instance fixture (torch) inspector is likewise modal over the editor.
	if (m_fixtureInspector.IsOpen()) {
		m_fixtureInspector.Update(input, static_cast<float>(m_window.Width()),
								  static_cast<float>(m_window.Height()));
		const PreviewSpec& sp = m_fixtureInspector.Preview();
		if (sp.fire && sp.showFire) m_previewFire.Update(dt); // preview flame
		return;
	}
	// The per-instance item/decoration inspector is likewise modal over the editor.
	if (m_propInspector.IsOpen()) {
		m_propInspector.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		if (m_propInspector.Preview().spin) m_previewSpin += dt * 0.9f; // turntable
		return;
	}
	// The per-instance door inspector too (open/closed + required key).
	if (m_doorInspector.IsOpen()) {
		m_doorInspector.Update(input, static_cast<float>(m_window.Width()),
							   static_cast<float>(m_window.Height()));
		return;
	}
	// And the button inspector (target-door wiring).
	if (m_buttonInspector.IsOpen()) {
		m_buttonInspector.Update(input, static_cast<float>(m_window.Width()),
								 static_cast<float>(m_window.Height()));
		return;
	}
	// And the wall-niche inspector (shape / secret / name).
	if (m_nicheInspector.IsOpen()) {
		m_nicheInspector.Update(input, static_cast<float>(m_window.Width()),
								static_cast<float>(m_window.Height()));
		return;
	}
	// And the in-flight projectile inspector (read-only details + dismiss).
	if (m_projectileInspector.IsOpen()) {
		m_projectileInspector.Update(input, static_cast<float>(m_window.Width()),
									 static_cast<float>(m_window.Height()));
		return;
	}

	// Map overlay: a toggle that never pauses the world. While it is open the
	// party still walks (keyboard) — the overlay only claims the mouse for
	// panning/zooming/editing, and Esc/M closes it instead of pausing.
	// EXCEPTION: while the editor's palette filter box holds focus, typed
	// keys are ITS (an 'm' must not toggle the map, Esc only unfocuses, and
	// the party must not walk on WASD) — capture is checked before the
	// overlay update runs so a releasing Esc doesn't also act here.
	const bool typingFilter = m_mapView.IsOpen() &&
							  m_mapView.CurrentMode() == MapView::Mode::Editor &&
							  m_mapEditor.KeyboardCaptured();
	if (!typingFilter && input.WasKeyPressed('M')) m_mapView.Toggle();

	// The editor's pause/play button freezes the world so the level can be
	// edited against a still scene: no sim time step and no party input. Never
	// set outside Editor mode (MapView::EditorPaused gates on it), and the
	// overlay clears it on close/mode-flip, so a closed editor always runs.
	const bool worldFrozen = m_mapView.EditorPaused();

	// Deferred editor-geometry rebake: undo/redo skips the expensive surface
	// rebuild while the full-screen editor hides the scene. The debt comes due
	// the moment the scene can show again (editor closed OR flipped to the
	// player map, which draws over the live scene): latch one frame so the
	// "rebuilding geometry" notice renders, then flush — the blocking rebake
	// freezes on the notice frame.
	const bool editorMapActive = m_mapView.IsOpen() &&
								 m_mapView.CurrentMode() == MapView::Mode::Editor;
	if (editorMapActive) {
		m_geomNoticeLatched = false;
	} else if (m_world.GeometryDirty()) {
		if (m_geomNoticeLatched) {
			m_world.FlushGeometry();
			m_geomNoticeLatched = false;
		} else {
			m_geomNoticeLatched = true;
		}
	}
	if (m_mapView.IsOpen()) {
		// While laying a patrol route (grid clicks lay waypoints), keys finish/undo
		// it — ahead of the overlay's own Esc-to-close.
		if (!typingFilter && m_mapEditor.LayingRoute()) {
			if (input.WasKeyPressed(VK_BACK))
				m_world.RemoveLastPatrolWaypoint(m_mapEditor.RouteId());
			if (input.WasKeyPressed(VK_RETURN) || input.WasKeyPressed(VK_ESCAPE)) {
				const u32 id = m_mapEditor.RouteId();
				m_mapEditor.EndRoute();
				if (const auto* r = m_world.MonsterPatrol(id))
					m_inspectCfg.patrolCount = static_cast<int>(r->size());
				m_entityInspector.Open(m_inspectCfg, m_world.SpellIds(),
									   m_inspectPreview); // back to the inspector (with preview)
				return;
			}
		}
		if (!typingFilter && input.WasKeyPressed(VK_ESCAPE)) {
			m_mapView.Close();
			return;
		}
		m_mapView.Update(input, MapPanel(static_cast<float>(m_window.Width()),
										 static_cast<float>(m_window.Height())));
		// The world keeps simulating while the map is open (the party still
		// walks on the keyboard) — EXCEPT while the editor is PAUSED, where the
		// whole world update is skipped so every persistent bit freezes:
		// monster AI decisions (they act off cooldowns, not dt, so dt=0 alone
		// wouldn't stop a ready monster), party tweens, particles, door slides,
		// animators. Editing routes through MapEditor→DungeonWorld directly, not
		// through Update, so it works while frozen; the full-screen editor
		// renders no 3D scene, so the skipped camera/light refresh is unseen.
		// The filter box eats the keyboard when it holds focus (blank Input).
		if (!worldFrozen) {
			static const Input kNoInput;
			m_world.Update(typingFilter ? kNoInput : input, wdt, m_time);
			if (auto t = m_world.ConsumeLevelTransition()) {
				m_mapView.Close(); // a stair step starts a new level load
				BeginLevelTransition(t->level, t->x, t->z, t->facing);
				return;
			}
			Party& party = m_world.GetParty();
			m_ui.SetHudStatus(party);
		}
		return;
	}

	// Esc closes the inventory window first (if open), before it would pause.
	if (m_ui.InventoryOpen() && input.WasKeyPressed(VK_ESCAPE)) {
		m_ui.CloseInventory();
		return;
	}

	// Esc freezes the world and opens the pause menu.
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_audio.Play(m_sounds.click, 0.5f);
		m_ui.ResetToMainPage();
		m_ui.RebuildPauseMenu(); // Load entry tracks whether a save now exists
		m_state = AppState::Paused;
		return;
	}

	// UI first so it can consume the mouse; keyboard always reaches the party.
	m_ui.UpdateHud(input, dt);
	// A portrait click may have opened the character sheet — freeze now
	// rather than simulating one more frame.
	if (m_state != AppState::Playing) return;

	// Held-tablet mouse interaction in the 3D world (only when the HUD didn't
	// already claim the click). Empty-handed: a click on a floor tablet picks it
	// up onto the cursor. Holding: a click drops it on the floor. Placement onto
	// portraits / hands / inventory is handled by those widgets (P4+), which
	// consume the mouse first.
	if (!m_ui.HudMouseConsumed()) {
		const float mx = input.MouseX(), my = input.MouseY();
		const float w = static_cast<float>(m_window.Width());
		const float h = static_cast<float>(m_window.Height());
		if (input.WasMousePressed(MouseButton::Left)) {
			if (m_heldItem) {
				m_world.DropItemAt(*m_heldItem, mx, my, w, h);
				m_heldItem.reset();
			} else if (auto picked = m_world.TryPickItem(mx, my, w, h)) {
				m_heldItem = std::move(picked);
			} else if (!m_world.ToggleDoorAhead()) {
				// No tablet, no door ahead: try the button on the wall the
				// party faces (a lever in the party's own cell).
				m_world.PressButtonFacing();
			}
		}
		// Right-mouse free-look: hold RMB and drag to swing the view. Begin on a
		// press over the 3D view (not a HUD widget); the party folds the offset
		// into a grid turn once it passes 45° (Party::AddLook). The handler below
		// runs every frame the button is down so the drag keeps tracking even if
		// the cursor wanders over the HUD.
		if (input.WasMousePressed(MouseButton::Right)) {
			m_looking = true;
			m_lookPrevX = mx;
			m_lookPrevY = my;
			m_world.GetParty().BeginLook();
		}
	}
	// Free-look drag/release is tracked outside the HUD-consumed gate so a drag
	// that strays over the bar (or a release there) still resolves.
	if (m_looking && input.IsMouseDown(MouseButton::Right)) {
		// Base ~0.005 rad/pixel (a quarter turn in ~157px), scaled by the user's
		// Look Sensitivity setting (Settings → Controls).
		const float k = 0.005f * m_settings.look.sensitivity;
		const float dx = input.MouseX() - m_lookPrevX;
		const float dy = input.MouseY() - m_lookPrevY;
		m_lookPrevX = input.MouseX();
		m_lookPrevY = input.MouseY();
		// Drag right -> view swings right (clockwise); drag down -> look down.
		m_world.GetParty().AddLook(-dx * k, -dy * k);
	} else if (m_looking) {
		m_looking = false;
		m_world.GetParty().EndLook(); // RMB up: the offset eases back to orthogonal
	}
	m_world.Update(input, wdt, m_time);
	if (auto t = m_world.ConsumeLevelTransition()) {
		BeginLevelTransition(t->level, t->x, t->z, t->facing);
		return;
	}

	Party& party = m_world.GetParty();
	m_ui.SetHudStatus(party);
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound. Loading, Menu, and LoadingGame are 2D-only
// (title art / progress screens); Playing draws the 3D scene + HUD.
// ============================================================================
void Game::Render(ID3D12GraphicsCommandList* list) {
	m_renderer.NewFrame(m_device.FrameIndex());
	m_spriteBatch.NewFrame(m_device.FrameIndex());
	m_world.NewFrame(m_device.FrameIndex());

	// The full-screen editor map covers everything, so skip the 3D scene (and
	// the HUD below) while it is up — nothing else needs drawing behind it.
	const bool editorMap = m_state == AppState::Playing && m_mapView.IsOpen() &&
						   m_mapView.CurrentMode() == MapView::Mode::Editor;

	// The offscreen 3D preview feeds from the asset dialog's picked model (P4b)
	// or the dev `preview` command (P4a). Render() redirects the OM, so rebind
	// the back buffer for the 2D pass.
	const gfx::Mesh* pvMesh = nullptr;
	gfx::MaterialParams pvMat;
	float pvOrbit = 0.0f;
	float pvScale = 1.0f;
	float pvAspect = 1.0f;           // pane width/height, so a tall pane doesn't distort
	std::span<const Mat4> pvPalette; // skinning palette for the animated monster preview
	gfx::ParticleBatch* pvParticles = nullptr; // torch preview flame/smoke
	std::span<const gfx::ParticleInstance> pvBillboards;
	std::span<const gfx::PreviewSubmesh> pvSubs; // per-instance dialog (multi-material)
	const Vec3* pvFitMin = nullptr;             // auto-fit AABB (small items)
	const Vec3* pvFitMax = nullptr;
	if (m_assetPicker.IsOpen() && m_assetPicker.HasPreview()) {
		// The picker is above the type editor and above the create dialog, so it
		// claims the shared preview RT first.
		pvMesh = &m_assetPicker.PreviewMesh();
		pvMat = m_assetPicker.PreviewMaterial();
		pvOrbit = m_assetPicker.Orbit();
	} else if (m_assetDialog.IsOpen() && m_assetDialog.HasPreview()) {
		pvMesh = &m_assetDialog.PreviewMesh();
		pvMat = m_assetDialog.PreviewMaterial();
		pvOrbit = m_assetDialog.Orbit();
	} else if (m_monsterDialog.IsOpen() && m_previewMonMesh) {
		// The monster-config dialog's live animation: a fixed front-on view (the
		// mesh faces +Z / the camera is at -Z, so ~π turns it toward the camera),
		// rendered at the (tall) preview pane's aspect so it isn't squashed.
		// Every piece draws (a multi-material rig previews bones+armor+weapons).
		const gfx::Rect pv = m_monsterDialog.PreviewRect(
			static_cast<float>(m_device.Width()), static_cast<float>(m_device.Height()));
		pvSubs = m_previewMonSubs;
		pvScale = m_previewMonScale;
		pvOrbit = kPi + m_previewMonYaw; // face the camera + the model's facing fixup
		pvAspect = pv.h > 0.0f ? pv.w / pv.h : 1.0f;
		pvPalette = m_previewAnim.Palette();
	} else if (InstanceInspector* ii = ActiveInstanceInspector(); ii && ii->HasPreview()) {
		// A per-instance edit dialog's live preview, read generically from its spec:
		// mesh(es) (animated for a monster), rendered at the pane's aspect, front-on,
		// with the torch's flame/smoke overlaid when lit.
		const PreviewSpec& sp = ii->Preview();
		const gfx::Rect pv = ii->PreviewRect(static_cast<float>(m_device.Width()),
											 static_cast<float>(m_device.Height()));
		pvSubs = sp.subs;
		pvScale = sp.scale;
		pvOrbit = kPi + sp.yaw + (sp.spin ? m_previewSpin : 0.0f);
		pvAspect = pv.h > 0.0f ? pv.w / pv.h : 1.0f;
		if (sp.skeleton) pvPalette = m_previewAnim.Palette();
		if (sp.autoFit) {
			pvFitMin = &sp.fitMin;
			pvFitMax = &sp.fitMax;
		}
		if (sp.fire && sp.showFire) { // lit torch: overlay flame/smoke
			m_previewFireScratch.clear();
			m_previewFire.AppendParticles(m_previewFireScratch);
			pvParticles = &m_previewParticles;
			pvBillboards = m_previewFireScratch;
		}
	} else if (m_previewMesh) {
		pvMesh = m_previewMesh.get();
		pvMat = m_previewMaterial;
		pvOrbit = m_previewOrbit;
	}
	const bool devPreviewFullscreen = m_previewMesh && !m_assetDialog.IsOpen() &&
									  !m_monsterDialog.IsOpen();

	if (!pvSubs.empty()) { // per-instance dialog preview (one or many submeshes)
		if (pvParticles) pvParticles->NewFrame(m_device.FrameIndex());
		m_modelPreview.Render(list, m_renderer, pvSubs, pvScale, pvOrbit, pvAspect, pvPalette,
							  pvParticles, pvBillboards, pvFitMin, pvFitMax);
		m_device.BindBackBuffer(list);
	} else if (pvMesh) {
		m_modelPreview.Render(list, m_renderer, *pvMesh, pvMat, pvScale, pvOrbit, pvAspect,
							  pvPalette);
		m_device.BindBackBuffer(list);
	}
	// The 3D scene draws during play and under the pause/character-sheet
	// overlays (frozen); Loading and Menu are 2D-only. The full-screen dev
	// preview replaces it; the editor map and dialog skip it too.
	else if ((m_state == AppState::Playing || m_state == AppState::Paused ||
			  m_state == AppState::CharacterSheet) &&
			 !editorMap) {
		m_world.UpdateItemIcons(list, m_spriteBatch); // 3D item icons (static + spin)
		m_world.UpdateMapIcons(list, m_spriteBatch);  // map marker icons (one-shot)
		m_world.RenderShadowMaps(list);
		// The scene renders linear HDR into the post target; Resolve runs the
		// bloom chain + ACES composite and leaves the back buffer bound for
		// the 2D pass below.
		m_postProcess.BeginScene(list);
		m_world.RenderScene(list);
		m_postProcess.Resolve(list);
	} else if (editorMap) {
		// The editor covers the scene, but its map overlay draws the baked
		// marker icons — keep the bakes running (a kind placed from the palette
		// bakes on the next frame; item icons also feed the map's item markers).
		// The bakes rebind the back buffer themselves when they ran.
		m_world.UpdateItemIcons(list, m_spriteBatch);
		m_world.UpdateMapIcons(list, m_spriteBatch);
	}
	// The asset picker's model tiles bake in the same phase (they need the
	// command list). Only the DRAW is recorded here — the picker made the mesh
	// and the target back in Update, because creating a render target drains the
	// GPU and doing that mid-recording corrupted the next bake in the frame.
	if (m_assetPicker.IsOpen()) {
		bool baked = false;
		for (const AssetPicker::PendingBake& bake : m_assetPicker.PendingBakes(2)) {
			m_world.BakeIconFor(list, m_spriteBatch, *bake.mesh, bake.lo, bake.hi,
								*bake.target);
			m_assetPicker.MarkBaked(bake.name);
			baked = true;
		}
		// A bake redirects the output merger at its own 256px target; leaving it
		// there sends the 2D pass into the last icon baked (a squashed copy of
		// the whole screen, which is exactly what the tiles showed). Same
		// epilogue UpdateMapIcons has.
		if (baked) m_device.BindBackBuffer(list);
	}

	// 2D pass.
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	switch (m_state) {
	case AppState::Loading:     m_ui.RenderLoadingScreen(m_loadQueue); break;
	case AppState::Menu:        m_ui.RenderMenuOverlay(); break;
	case AppState::LoadingGame:  m_ui.RenderGameLoadingScreen(m_loadQueue); break;
	case AppState::LoadingLevel: m_ui.RenderGameLoadingScreen(m_loadQueue); break;
	case AppState::Playing: {
		const float dw = static_cast<float>(m_device.Width());
		const float dh = static_cast<float>(m_device.Height());
		if (editorMap) {
			// Full-screen editor — drawn alone, no HUD or scene behind it.
			m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
		} else {
			m_ui.RenderHud();
			if (m_mapView.IsOpen()) {
				// Player map: dim the scene behind the 80% panel, over the HUD.
				m_spriteBatch.DrawRect({0, 0, dw, dh}, {0, 0, 0, 0.45f});
				m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
			}
		}
		// The deferred-rebake notice (see Update): the frame the blocking
		// FlushGeometry freezes on, so the pause reads as work, not a hang.
		if (m_geomNoticeLatched) DrawBusyNotice(loc::Tr("map.rebuilding"), dw, dh);
		break;
	}
	case AppState::Paused:      m_ui.RenderPauseOverlay(); break;
	case AppState::CharacterSheet: m_ui.RenderCharacterSheetOverlay(); break;
	}
	const float dw = static_cast<float>(m_device.Width());
	const float dh = static_cast<float>(m_device.Height());
	if (m_assetDialog.IsOpen()) {
		// The asset dialog overlays the editor; it draws its own frame, then we
		// blit the rendered preview model into its preview pane.
		m_assetDialog.Render(m_spriteBatch, dw, dh);
		if (m_assetDialog.HasPreview())
			m_spriteBatch.DrawSprite(m_assetDialog.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	} else if (devPreviewFullscreen) {
		// Dev `preview` command: dim the frame and blit the model full-screen.
		m_spriteBatch.DrawRect({0, 0, dw, dh}, {0.04f, 0.04f, 0.06f, 1.0f});
		const float s = std::min(dw, dh) * 0.85f;
		m_spriteBatch.DrawSprite({(dw - s) * 0.5f, (dh - s) * 0.5f, s, s},
								 {0, 0, 1, 1}, m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	if (m_monsterDialog.IsOpen()) { // modal over the editor, like the asset dialog
		m_monsterDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
		if (m_previewMonMesh) // blit the live animation into the preview pane
			m_spriteBatch.DrawSprite(m_monsterDialog.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	if (m_balanceDialog.IsOpen()) // modal over the editor, like the others
		m_balanceDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_levelSettingsDialog.IsOpen())
		m_levelSettingsDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_typeDialog.IsOpen())
		m_typeDialog.Render(m_spriteBatch, m_settings.theme, dw, dh);
	// The asset picker draws OVER the type editor it was opened from, and blits
	// the selected asset's preview into its own pane (the asset dialog's seam).
	if (m_assetPicker.IsOpen()) {
		m_assetPicker.Render(m_spriteBatch, dw, dh);
		if (m_assetPicker.HasPreview())
			m_spriteBatch.DrawSprite(m_assetPicker.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	// The per-instance edit dialogs, each drawn (panel + controls) THEN, once all
	// are drawn, the 3D preview blitted into the active one's pane — the blit must
	// come last so a dialog's backing box never covers it.
	if (m_entityInspector.IsOpen())
		m_entityInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_fixtureInspector.IsOpen())
		m_fixtureInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_propInspector.IsOpen())
		m_propInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_doorInspector.IsOpen())
		m_doorInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_buttonInspector.IsOpen())
		m_buttonInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_nicheInspector.IsOpen())
		m_nicheInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_projectileInspector.IsOpen())
		m_projectileInspector.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (InstanceInspector* ii = ActiveInstanceInspector(); ii && ii->HasPreview())
		m_spriteBatch.DrawSprite(ii->PreviewRect(dw, dh), {0, 0, 1, 1}, m_modelPreview.Srv(),
								 {1, 1, 1, 1});
	if (m_inspectPicker.IsOpen()) // multi-object chooser, modal over the editor
		m_inspectPicker.Render(m_spriteBatch, m_settings.theme, dw, dh);
	if (m_console.IsOpen())
		m_console.Render(m_spriteBatch, m_device, static_cast<float>(m_device.Width()),
						 static_cast<float>(m_device.Height()));
	// The pending quality swap's notice. Drawn LAST and outside the per-state
	// switch on purpose: the tier can be picked from the landing page (Menu),
	// the pause menu (Paused) or the dev console over any state, and this is the
	// frame the reload will stall on — nothing may cover it.
	if (m_pendingQuality) DrawBusyNotice(loc::Tr("settings.applying"), dw, dh);
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
