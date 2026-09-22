// ============================================================================
// Game/Game_World.cpp — the Game side of the world tier (docs/world-map.md).
//
// Two jobs, both small on purpose: resolve terrain.cat into the rules WorldMap
// parses against, and report what was loaded for the `world` dev command.
//
// The resolution lives HERE rather than in WorldMap because WorldMap is free of
// Project by design — it parses and answers questions and can be tested without
// a project, exactly as DungeonMap takes a resolved FixtureTypes rather than
// reaching for the catalogs itself. This file is the seam where catalog data
// becomes map rules, and it is the only place that knows both.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/Resource.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <format>
#include <random>

namespace dungeon::game {

namespace {

// A terrain kind's glyph, as authored. Exactly one character: a glyph IS one
// grid cell, so "" or "MM" is an authoring error rather than something to
// interpret generously.
char GlyphOf(const CatalogEntry& e) {
	const std::string g = e.Get("glyph", "");
	if (g.size() == 1) return g[0];
	log::Warn("terrain '{}' has {} glyph — one character required, using '?'",
			  e.id, g.empty() ? "no" : "a multi-character");
	return '?';
}

// How many space-separated stems a dungeon's `levels` field names.
size_t WordCount(const std::string& s) {
	size_t n = 0;
	bool inWord = false;
	for (const char c : s) {
		const bool space = c == ' ' || c == '	';
		if (!space && !inWord) ++n;
		inWord = !space;
	}
	return n;
}

} // namespace

void Game::LoadWorldMap() {
	WorldMap::TerrainRules rules;
	rules.reserve(m_project.terrain.Entries().size());
	for (const CatalogEntry& e : m_project.terrain.Entries()) {
		WorldMap::Terrain t;
		t.id = e.id;
		t.glyph = GlyphOf(e);
		t.passable = e.GetBool("passable", true);
		t.travel = e.GetFloat("travel", 1.0f);
		t.difficulty = e.GetFloat("difficulty", 0.0f);
		t.tags = CatalogTags(&e);
		CatalogColor(&e, "color", t.color); // absent leaves the neutral default
		rules.push_back(std::move(t));
	}

	if (rules.empty()) {
		// No terrain authored means no world can be read, and that is a normal
		// state for a project that has not grown one yet — not a failure.
		log::Info("No terrain.cat entries: project '{}' has no world map",
				  m_project.name);
		return;
	}

	m_worldMap = WorldMap::Load(m_project.WorldMapPath(), std::move(rules));
	if (!m_worldMap) {
		log::Info("Project '{}' has no world/world.map", m_project.name);
		return;
	}
	log::Info("Loaded world map: {}x{}, {} terrain kinds, {} areas, {} locations",
			  m_worldMap->Width(), m_worldMap->Height(),
			  m_worldMap->Terrains().size(), m_worldMap->Areas().size(),
			  m_worldMap->Locations().size());
}

void Game::ResetWorldState() {
	m_worldState = {};
	if (!m_worldMap) return;
	m_worldState.x = m_worldMap->StartX();
	m_worldState.z = m_worldMap->StartZ();
	// The party can see where it is standing. One cell, not a radius: what a
	// step reveals is P3's business, and guessing it here would be a rule in two
	// places before either is written.
	m_worldState.MarkSeen(m_worldState.x, m_worldState.z);
	// onWorldMap stays FALSE: a new game still begins inside a dungeon until P4
	// moves the opening. The party has a world position regardless — it is where
	// it came in from.
}

bool Game::SaveWorld() {
	if (!m_worldMap) return false;
	// NORMALIZED like every other file the editor writes: the project is CRLF
	// on disk and the serializer emits bare newlines, so one call keeps a
	// savemap from showing up in git as a whole-file line-ending rewrite.
	const std::string text = serialize::NormalizeEol(m_worldMap->Serialize());
	if (!assets::WriteBinaryFile(m_project.WorldMapPath(), text.data(),
								 text.size())) {
		log::Warn("savemap: could not write {}", m_project.WorldMapPath());
		return false;
	}
	// RE-READ WHAT WAS WRITTEN, and keep it. Anything that can be written can
	// be written wrong, and the failure mode here is the worst kind — the
	// in-memory world stays right for the rest of the session and the damage
	// only shows on the next launch. Parsing it back costs microseconds and
	// turns that into a warning now.
	WorldMap::TerrainRules rules;
	for (const WorldMap::Terrain& t : m_worldMap->Terrains()) rules.push_back(t);
	if (std::optional<WorldMap> reread =
			WorldMap::Load(m_project.WorldMapPath(), std::move(rules))) {
		if (reread->Serialize() != m_worldMap->Serialize())
			log::Warn("savemap: the world was written but did not read back "
					  "identically — something in it does not round-trip");
	}
	log::Info("Wrote {}", m_project.WorldMapPath());
	return true;
}

std::vector<validate::Issue> Game::ValidateProject() {
	// The world tier's half of the snapshot. Gathered HERE because Game is the
	// only thing that holds both the world map and the catalogs; the checker
	// stays free of Project and DungeonWorld stays free of the world.
	validate::WorldView view;
	view.map = m_worldMap ? &*m_worldMap : nullptr;
	for (const CatalogEntry& e : m_project.dungeons.Entries()) {
		validate::DungeonView d;
		d.id = e.id;
		d.levels = ParseTags(e.Get("levels", "")); // space-split + lowercased
		view.dungeons.push_back(std::move(d));
	}
	for (const CatalogEntry& e : m_project.quests.Entries()) {
		validate::QuestView q;
		q.id = e.id;
		q.stages = ParseTags(e.Get("stages", ""));
		view.quests.push_back(std::move(q));
	}
	for (const CatalogEntry* e : m_project.AllItems()) {
		if (!e) continue;
		validate::ItemHookView h;
		h.item = e->id;
		const std::string q = e->Get("quest", "");
		if (const size_t colon = q.find(':'); colon != std::string::npos) {
			h.quest = q.substr(0, colon);
			h.stage = q.substr(colon + 1);
		}
		h.reveals = e->Get("reveals", "");
		if (!h.quest.empty() || !h.reveals.empty())
			view.itemHooks.push_back(std::move(h));
	}
	return m_world.Validate(view);
}

void Game::SetOnWorldMap(bool on) {
	if (on) {
		if (!m_worldMap) {
			log::Warn("world map: the project has none to travel");
			return;
		}
		// NOT BEFORE THERE IS A GAME. `worldmap on` answers from the MENU too —
		// dev commands reach the world tier as soon as it is loaded, which is
		// the trap StateName() warns about — and the way back out sets Playing,
		// where the first frame dereferences a HUD that was never built. Two
		// console commands from the title screen crashed the process.
		if (!m_gameLoaded) {
			log::Warn("world map: no game in progress to travel in");
			return;
		}
		// Reveal where the party is standing before the first frame draws, or
		// it appears in the middle of unexplored ground it has plainly reached.
		RevealAround(m_worldState.x, m_worldState.z);
		m_worldState.onWorldMap = true;
		m_worldMapView.Reset(); // fit the whole world, like opening any map
		m_state = AppState::WorldMap;
		return;
	}
	m_worldState.onWorldMap = false;
	if (m_state == AppState::WorldMap) m_state = AppState::Playing;
}

bool Game::EnterLocation(const std::string& id) {
	if (!m_worldMap) return false;
	const WorldMap::Location* loc = nullptr;
	for (const WorldMap::Location& l : m_worldMap->Locations())
		if (l.id == id) loc = &l;
	if (!loc) {
		log::Warn("enter: no location '{}' on the world map", id);
		return false;
	}
	// Only what the party KNOWS is there can be entered. Discovery is not
	// decoration: an undiscovered location is one the party has no idea exists,
	// and being able to walk into it would make finding it meaningless.
	if (!m_worldState.Discovered(id)) {
		if (m_world.onMessage) m_world.onMessage(loc::View("world.undiscovered"));
		return false;
	}
	if (loc->kind != "dungeon") {
		// Towns are a location KIND with no content behind them yet (they need
		// money and trade — see docs/world-map.md). Saying so is better than
		// silently doing nothing at a place the map draws.
		if (m_world.onMessage) m_world.onMessage(loc::View("world.nothing_here"));
		return false;
	}

	const CatalogEntry* d = m_project.dungeons.Find(loc->Dungeon());
	if (!d) {
		log::Warn("enter: location '{}' names no dungeon ('{}')", id,
				  loc->Dungeon());
		return false;
	}
	const std::vector<std::string> levels = ParseTags(d->Get("levels", ""));
	if (levels.empty()) {
		log::Warn("enter: dungeon '{}' has no levels", loc->Dungeon());
		return false;
	}
	// WHERE THIS DOORWAY LANDS — the LOCATION says, and nothing else does. A
	// dungeon has no start of its own (Michael, 2026-09-09): it is a named group
	// of levels, and every way in carries its own destination. An unauthored
	// `level` falls back to the first only so a half-written location still
	// opens something rather than aborting; the checker calls it out.
	std::string entry = loc->level;
	if (entry.empty() ||
		std::find(levels.begin(), levels.end(), entry) == levels.end()) {
		log::Warn("enter: location '{}' names no level of dungeon '{}' — using {}",
				  id, loc->Dungeon(), levels.front());
		entry = levels.front();
	}

	m_worldState.onWorldMap = false;
	m_worldState.atLocation = id; // the fallback for an exit that names none
	BeginLevelTransition(entry, loc->entryX, loc->entryZ, Direction::South,
						 /*stashCurrent=*/false);
	if (m_world.onMessage)
		m_world.onMessage(loc::FormatLine("world.entered", d->Display()));
	return true;
}

bool Game::LeaveDungeon(const std::string& viaLocation) {
	if (!m_worldMap) return false;
	// WHICH DOORWAY THIS IS. An exit stair names the location it surfaces at,
	// because a dungeon may have several ways out and coming out of the front
	// door after climbing the back stairs would be a teleport. An exit that
	// names none — a single-entrance dungeon, or the `leave` dev command —
	// falls back to the way the party came IN.
	std::string where = viaLocation.empty() ? m_worldState.atLocation : viaLocation;
	if (!where.empty()) {
		bool found = false;
		for (const WorldMap::Location& l : m_worldMap->Locations())
			if (l.id == where) {
				m_worldState.x = l.x;
				m_worldState.z = l.z;
				found = true;
			}
		if (!found)
			// Say so rather than silently surfacing wherever the party last
			// stood: a stair pointing at a location that is not there is
			// exactly the drift the stair-pair check exists to name.
			log::Warn("leave: exit names location '{}', which is not on the "
					  "world map — surfacing where the party stood",
					  where);
		// COMING OUT IS FINDING IT. You now know where this door is, even if
		// you have never approached it from the outside — which is the whole
		// point of a back way.
		else if (m_worldState.Discover(where) && m_world.onMessage)
			m_world.onMessage(loc::FormatLine("world.discovered", where));
	}
	m_worldState.atLocation.clear();
	SetOnWorldMap(true);
	return true;
}

bool Game::TravelStep(int dx, int dz) {
	if (!m_worldMap) return false;
	const int nx = m_worldState.x + dx, nz = m_worldState.z + dz;
	if (!m_worldMap->InBounds(nx, nz) || !m_worldMap->Passable(nx, nz)) return false;

	// THE COST OF A STEP IS THE COST OF THE SQUARE YOU ENTER, not an average of
	// the two. It is the rule a player can read off the map before moving — the
	// caption quotes exactly this number for the square under the cursor — and a
	// rule you can see is worth more here than a smoother one you cannot.
	const float hours = m_worldMap->TravelHours(nx, nz);
	m_worldState.x = nx;
	m_worldState.z = nz;
	m_worldState.time += hours;
	SettleJourney(hours);
	RevealAround(nx, nz);

	// AND SOMETHING MAY BE WAITING. The roll is against the square ENTERED, and
	// scales with BOTH its danger and how long it took to cross — an hour in the
	// hills is more chances to be found than half an hour on the road, and the
	// road is safer per hour as well. So terrain gets to matter twice, which is
	// the whole reason it carries two numbers.
	//
	// Rolled AFTER the step is complete: the party is somewhere, its supplies
	// are paid for and the ground is revealed, so an encounter is something that
	// happens to a party that has arrived rather than one caught mid-stride.
	const float danger = m_worldMap->Difficulty(nx, nz);
	if (danger > 0.0f && !m_encountersOff) {
		const float chance =
			std::clamp(danger * hours * m_encounterRate, 0.0f, 0.9f);
		std::uniform_real_distribution<float> d(0.0f, 1.0f);
		if (d(m_world.Rng()) < chance) {
			const WorldMap::Terrain& t = m_worldMap->TerrainAt(nx, nz);
			StartEncounter(danger, t.tags, m_world.Rng()());
		}
	}
	return true;
}

void Game::SettleJourney(float hours) {
	// A JOURNEY SETTLES A BILL, and since 2026-09-09 that bill includes the
	// things that were never rates: DoTs bite on the road, wounds close, and an
	// unconscious member comes round somewhere along the way (Michael — "DoT
	// should still affect the party members. Travelling on the world map COULD
	// easily kill affected members").
	//
	// IN SLICES, because those are state machines and not rates. A poison that
	// would finish someone three hours into a six-hour march has to finish them
	// THERE — settling the whole span in one call would apply six hours of burn
	// to a member who should not have survived the third, and then heal what was
	// left of them.
	//
	// It calls the SAME TickParty the dungeon's own update calls, so the two
	// cost models cannot disagree about what an hour costs. This is why the
	// party tick had to come out of UpdateMonsters: travel has no monsters to
	// update and no level worth updating them in.
	const float seconds = hours * 3600.0f;
	if (seconds <= 0.0f) return;

	// A minute at a time. Fine enough that a death lands within a minute of
	// when it should, coarse enough that a day's march is a few hundred
	// iterations rather than a hundred thousand. Nothing in here is per-frame
	// work — the rates are all dt-scaled, so a slice is arithmetic, not a
	// simulation step.
	constexpr float kSlice = 60.0f;
	for (float t = 0.0f; t < seconds; t += kSlice) {
		const float dt = std::min(kSlice, seconds - t);
		// WALKING IS EXERTION, and this one line is what makes travel dangerous
		// rather than restorative. Health regen is gated on the exertion signal
		// the resources model already has (`staminaHoldoff`, docs/health-and-
		// healing.md), and without it half an hour of road regenerated ~400
		// health — enough to out-heal any DoT that was not lethal within the
		// minute, so a poisoned party arrived FULLER than it set out.
		//
		// That made "travelling could easily kill affected members" true only
		// of doses that kill instantly, and made camp pointless. You heal in
		// CAMP, not on the road.
		for (Character& member : m_characters)
			if (member.IsAlive()) member.staminaHoldoff = dt;
		// No monsters can be near a party that is out on the world map, so the
		// stabilize clock runs: the road is where you come round.
		m_world.TickParty(dt, /*danger=*/false);
		// A wipe on the road ends the journey — and the game. CheckPartyWipe
		// has already fired onPartyWipe by now; carrying on would go on
		// charging supplies to four corpses.
		if (m_world.PartyWiped()) break;
	}
}

void Game::OnItemFound(const std::string& itemId) {
	// THE TWO HOOKS THE DUMP NAMED, and no more: an item that moves a quest on,
	// and a map or clue that reveals a place (docs/world-map.md "Quests").
	// Applied where the item is actually LIFTED, so nothing has to be told
	// twice and an item still on the floor has changed nothing.
	const CatalogEntry* e = m_project.FindItem(itemId);
	if (!e) return;

	// "quest = <id>:<stage>" — the quest reaches that stage. Named, not
	// numbered, so inserting a stage cannot silently move everyone along.
	const std::string q = e->Get("quest", "");
	if (const size_t colon = q.find(':'); colon != std::string::npos) {
		const std::string id = q.substr(0, colon), stage = q.substr(colon + 1);
		if (m_worldState.SetQuestStage(id, stage)) {
			const CatalogEntry* def = m_project.quests.Find(id);
			if (m_world.onMessage)
				m_world.onMessage(loc::FormatLine(
					"world.quest_stage", def ? def->Display() : id,
					def ? def->Get("text_" + stage, stage) : stage));
		}
	} else if (!q.empty()) {
		log::Warn("item '{}' has quest = '{}' — expected <id>:<stage>", itemId, q);
	}

	// "flag = <key>=<value>" — global state that is not a quest's progress.
	const std::string f = e->Get("flag", "");
	if (const size_t eq = f.find('='); eq != std::string::npos)
		m_worldState.SetFlag(f.substr(0, eq), f.substr(eq + 1));
	else if (!f.empty())
		m_worldState.SetFlag(f, "1"); // a bare name is a flag that is simply set

	// "reveals = <location>" — the map-or-clue path, which writes the SAME list
	// exploring writes. That is the whole reason discovery and `seen` are
	// separate fields: this reveals a place without revealing the ground.
	const std::string r = e->Get("reveals", "");
	if (!r.empty() && m_worldMap) {
		bool exists = false;
		for (const WorldMap::Location& l : m_worldMap->Locations())
			if (l.id == r) exists = true;
		if (!exists)
			log::Warn("item '{}' reveals '{}', which is not on the world map",
					  itemId, r);
		else if (m_worldState.Discover(r) && m_world.onMessage)
			m_world.onMessage(loc::FormatLine("world.revealed", r));
	}
}

float Game::Camp() {
	// THE COUNTERWEIGHT to travel that can kill (docs/world-map.md). Camping is
	// REST, reached from the world map — not a second recovery model. It turns
	// the same state on and settles time until the same rules turn it off:
	// deprivation stops it, being fully recovered stops it, and the supplies it
	// burns are the ones TickParty was always going to charge.
	//
	// It does NOT use rest's 60x time multiplier. That exists to make waiting
	// bearable in real time inside a dungeon; out here world time is advanced
	// directly, so an hour camped IS an hour, and there is nothing to speed up.
	if (!m_worldState.onWorldMap) return 0.0f;
	if (m_world.Resting()) return 0.0f;

	m_world.SetResting(true);
	if (!m_world.Resting()) {
		// Refused before it began — starving or parched, and rest would only
		// spend health to pass time you are already losing health for. The
		// world has said why.
		return 0.0f;
	}

	constexpr float kSlice = 60.0f;
	constexpr float kMaxHours = 24.0f; // a day is long enough to be a decision
	float seconds = 0.0f;
	while (m_world.Resting() && seconds < kMaxHours * 3600.0f) {
		m_world.TickParty(kSlice, /*danger=*/false);
		seconds += kSlice;
		m_worldState.time += kSlice / 3600.0f;
		if (m_world.PartyWiped()) break;
	}
	m_world.SetResting(false);

	const float hours = seconds / 3600.0f;
	if (m_world.onMessage)
		m_world.onMessage(loc::FormatLine("world.camped",
										  std::format("{:.1f}", hours)));
	return hours;
}

void Game::RevealAround(int x, int z) {
	if (!m_worldMap) return;
	// A cell and its eight neighbours, the same reach a step reveals underground
	// (DungeonWorld::MarkSeen) — one rule for "what walking shows you".
	for (int oz = -1; oz <= 1; ++oz)
		for (int ox = -1; ox <= 1; ++ox) {
			const int cx = x + ox, cz = z + oz;
			if (!m_worldMap->InBounds(cx, cz)) continue;
			m_worldState.MarkSeen(cx, cz);
			// Discovery falls out of seeing the ground it stands on. The other
			// way in — a map or a clue naming a location outright — writes the
			// same list without touching `seen`, which is why the two are
			// separate fields.
			if (const WorldMap::Location* l = m_worldMap->LocationAt(cx, cz))
				if (m_worldState.Discover(l->id) && m_world.onMessage)
					m_world.onMessage(loc::FormatLine("world.discovered", l->id));
		}
}

std::vector<std::string> Game::WorldReport() const {
	std::vector<std::string> out;
	if (!m_worldMap) {
		out.push_back("no world map loaded (project has no world/world.map)");
		return out;
	}
	const WorldMap& w = *m_worldMap;

	// Count the cells of each kind while walking the grid once. A terrain kind
	// authored but never placed is worth seeing — it is the shape a typo in a
	// glyph takes.
	std::vector<int> cells(w.Terrains().size(), 0);
	int passable = 0;
	for (int z = 0; z < w.Height(); ++z)
		for (int x = 0; x < w.Width(); ++x) {
			const WorldMap::Terrain& t = w.TerrainAt(x, z);
			for (size_t i = 0; i < w.Terrains().size(); ++i)
				if (w.Terrains()[i].id == t.id) {
					++cells[i];
					break;
				}
			if (t.passable) ++passable;
		}

	out.push_back(std::format("{}x{} world, start {},{}, {} of {} cells passable",
							  w.Width(), w.Height(), w.StartX(), w.StartZ(),
							  passable, w.Width() * w.Height()));
	// The DYNAMIC half, which is what a save round-trip has to reproduce. Named
	// "party/state" rather than folded into the line above so the two halves
	// read apart: everything above is authored, everything here is play.
	out.push_back(std::format(
		"  party   {},{} ({})  time {:.2f}h  seen {}  discovered {}  flags {}",
		m_worldState.x, m_worldState.z,
		m_worldState.onWorldMap ? std::string("on the world map")
		: m_worldState.atLocation.empty()
			? std::string("in a dungeon")
			: std::format("inside {}", m_worldState.atLocation),
		m_worldState.time, m_worldState.seen.size(),
		m_worldState.discovered.size(), m_worldState.flags.size()));
	for (size_t i = 0; i < w.Terrains().size(); ++i) {
		const WorldMap::Terrain& t = w.Terrains()[i];
		out.push_back(std::format(
			"  terrain {:<10} '{}' {:>5} cells  travel {:.2f}h  difficulty {:.2f}{}",
			t.id, t.glyph, cells[i], t.travel, t.difficulty,
			t.passable ? "" : "  (impassable)"));
	}
	for (const WorldMap::Area& a : w.Areas())
		out.push_back(std::format("  area    {:<10} {},{} {}x{}  difficulty {}",
								  a.id, a.x, a.z, a.w, a.h,
								  a.difficulty >= 0.0f
									  ? std::format("{:.2f}", a.difficulty)
									  : std::string("(terrain's own)")));
	for (const WorldMap::Location& l : w.Locations()) {
		// Report the location against the DUNGEON it names, since a location
		// pointing at nothing is the fault most worth seeing here.
		const CatalogEntry* d = m_project.dungeons.Find(l.Dungeon());
		out.push_back(std::format(
			"  {:<7} {:<12} {},{}  on {}  difficulty {:.2f}  -> {}", l.kind, l.id,
			l.x, l.z, w.TerrainAt(l.x, l.z).id, w.Difficulty(l.x, l.z),
			d ? std::format("{} ({} level(s)) at {} {},{}", l.Dungeon(),
							WordCount(d->Get("levels", "")),
							l.level.empty() ? std::string("(unset!)") : l.level,
							l.entryX, l.entryZ)
			  : std::string("NO SUCH DUNGEON")));
	}
	return out;
}

// ---------------------------------------------------------------------------
// The world settings dialog (W4, docs/world-editor-plan.md).
//
// The DIALOG PROPOSES AND THIS DISPOSES: every callback here brackets its own
// undo step and hands the decision to WorldMap, whose refusals are the ones the
// LOADER and the checker make. That is what stops the dialog and the `worldloc`
// / `worldarea` / `worldprops` commands being two editors with one name.
// ---------------------------------------------------------------------------
void Game::OpenWorldSettings(const std::string& selectLocation) {
	if (!m_worldMap) {
		log::Warn("world settings: the project has no world");
		return;
	}
	// The dungeons and their levels, so the dialog's dungeon/level fields can
	// be DROPDOWNS of what exists. A location naming a level its dungeon does
	// not have is a checker error; offering only real pairs refuses it by
	// construction rather than reporting it afterwards.
	std::vector<WorldSettingsDialog::DungeonInfo> dungeons;
	for (const CatalogEntry& e : m_project.dungeons.Entries())
		dungeons.push_back({e.id, ParseTags(e.Get("levels", ""))});

	WorldSettingsDialog::Manifest m;
	m.startDungeon = m_project.startDungeon;
	m.startLevel = m_project.startLevel;
	m.startX = m_project.startX;
	m.startZ = m_project.startZ;
	m.evalLevel = m_project.evalLevel;
	m_worldSettingsDialog.Open(&*m_worldMap, std::move(dungeons), m_project.levels,
							   std::move(m), selectLocation);
}

void Game::WireWorldSettingsDialog() {
	WorldSettingsDialog& d = m_worldSettingsDialog;
	// One shape for every world edit: open a step, let the map decide, close
	// the step with whether anything actually changed. CommitUndoStep(false)
	// is what keeps a refused edit from spending a Ctrl+Z on nothing.
	auto step = [this](auto&& apply) {
		m_world.BeginUndoStep();
		const bool ok = apply();
		m_world.CommitUndoStep(ok);
		return ok;
	};
	d.onSetStart = [this, step](int x, int z) {
		return step([&] { return m_worldMap->SetStart(x, z); });
	};
	d.onAddArea = [this, step](const WorldMap::Area& a) {
		return step([&] { return m_worldMap->AddArea(a); });
	};
	d.onDeleteArea = [this, step](const std::string& id) {
		return step([&] { return m_worldMap->RemoveArea(id); });
	};
	d.onOrderArea = [this, step](const std::string& id, int index) {
		return step([&] { return m_worldMap->MoveArea(id, index); });
	};
	d.onEditArea = [this, step](const std::string& id, const WorldMap::Area& next) {
		return step([&] {
			WorldMap::Area* live = m_worldMap->MutableArea(id);
			if (!live) return false;
			// A RENAME IS REFUSED WHEN IT COLLIDES, for the reason AddArea
			// refuses one: this dialog addresses an area by its id, and two
			// rows answering to one name is not something it can represent.
			if (next.id != id && m_worldMap->MutableArea(next.id)) return false;
			if (next.w <= 0 || next.h <= 0) return false; // Load asserts on it
			*live = next;
			return true;
		});
	};
	d.onAddLocation = [this, step](WorldMap::Location l) {
		return step([&] { return m_worldMap->AddLocation(std::move(l)); });
	};
	d.onDeleteLocation = [this, step](const std::string& id) {
		return step([&] { return m_worldMap->RemoveLocation(id); });
	};
	d.onMoveLocation = [this, step](const std::string& id, int x, int z) {
		return step([&] { return m_worldMap->MoveLocation(id, x, z); });
	};
	d.onEditLocation = [this, step](const std::string& id,
									const WorldMap::Location& next) {
		return step([&] {
			WorldMap::Location* live = m_worldMap->MutableLocation(id);
			if (!live) return false;
			// Everything but WHERE IT STANDS and WHAT IT IS CALLED: the cell
			// has an occupancy rule of its own (onMoveLocation), and the id is
			// named by exit stairs and by saves, so renaming needs the sweep.
			live->kind = next.kind;
			live->dungeon = next.dungeon;
			live->level = next.level;
			live->entryX = next.entryX;
			live->entryZ = next.entryZ;
			return true;
		});
	};
	// THE MANIFEST IS NOT UNDOABLE and the dialog says so in its own words:
	// the editor's history snapshots the world and the levels, not project.ini.
	d.onManifest = [this](const WorldSettingsDialog::Manifest& m) {
		m_project.startDungeon = m.startDungeon;
		m_project.startLevel = m.startLevel;
		m_project.startX = m.startX;
		m_project.startZ = m.startZ;
		m_project.evalLevel = m.evalLevel;
	};
	// Save means WRITE WHAT I CHANGED, both halves of it — the world to
	// world.map and the manifest to project.ini. A dialog that wrote one of
	// the two would lose the other on the next launch, silently, which is the
	// failure the world writer's read-back check exists to avoid elsewhere.
	d.onSave = [this] {
		const bool world = SaveWorld();
		const bool proj = m_project.Save();
		m_ui.AddLogLine(loc::View(world && proj ? "map.world.saved"
												: "map.world.savefailed"));
	};
}

} // namespace dungeon::game
