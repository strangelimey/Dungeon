// ============================================================================
// Game/DungeonWorld_Light.cpp - light profiles, and the party's own light: a
// LIT TORCH in a hand.
//
// Every light is pushed through PushLight from a lights.cat PROFILE
// (Game/LightProfile.h, lighting-updates Phase 2): its colour, brightness,
// reach, pulse and shadow come from the source's TYPE, not from code.
//
// There is no light at the eye any more (spell-updates Phase 4, Michael: "the
// held torch is the light"). Each lit torch a member holds - and one on the
// cursor - is a flickering point light at that member's side of the eye; with
// none, the party sees by the level's ambient alone, and a level authored at
// ambient 0 is pitch black.
//
// A lit torch BURNS while it is held: its CHARGE (ItemSlot::charge) counts its
// seconds down from the kind's `burn_time`, it dims over its last tenth, and
// spent it becomes its `spent_as` stub. Stowed in a pack it goes out, keeping
// what is left. On the FLOOR it stays lit (Phase 4): it burns on where it lies
// and lights its square, thrown or set down.
//
// The hand menu's TORCH COMMANDS live here too: Put out (any lit torch) and
// Light (a magical one, which no spell's fire takes - it costs mana).
//
// Everything here runs every frame, so it allocates nothing: the ids it renames
// are assigned into the slots' own buffers (always to a shorter or equal-length
// id in the shipped catalog, and a std::string keeps its capacity anyway), and
// the kinds are all built at load (PreloadItemKinds).
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/Facing.h"
#include "Graphics/LightTiles.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

namespace dungeon::game {

namespace {
// The share of a torch's burn over which it dims, and how dim it gets.
constexpr float kDimShare = 0.1f;
constexpr float kDimFloor = 0.35f;
// The cursor's torch's slot in its light key (member hands use member x 2 + hand).
constexpr u32 kCursorTorchSlot = 255;
} // namespace

// --- light profiles (lighting-updates Phase 2) --------------------------------

void DungeonWorld::ReloadLightProfiles() {
	m_lightProfiles.clear();
	std::vector<std::string> problems;
	for (const CatalogEntry& e : m_project.lights.Entries())
		m_lightProfiles.push_back(light::Parse(
			e.id, [&e](std::string_view key) { return e.Get(key); }, &problems));
	for (const std::string& p : problems) log::Warn("{}", p);

	// A source naming a profile the project lacks still lights (the fallback),
	// but say so once here rather than leave it to be noticed as "too orange".
	const auto known = [this](std::string_view id) {
		for (const light::Profile& p : m_lightProfiles)
			if (p.id == id) return true;
		return false;
	};
	const auto check = [&](const Catalog& cat, const char* file) {
		for (const CatalogEntry& e : cat.Entries())
			if (const std::string id = e.Get("light"); !id.empty() && !known(id))
				log::Warn("{} [{}]: light '{}' is not in lights.cat; it draws as the "
						  "fallback warm light", file, e.id, id);
	};
	check(m_project.fixtures, "fixtures.cat");
	check(m_project.items, "items.cat");
	check(m_project.weapons, "weapons.cat");
	check(m_project.armor, "armor.cat");
	check(m_project.effects, "effects.cat");
	// The ids the code names when a source names none.
	for (const char* id : {"fire_sconce", "fire_brazier", "torch", "burning", "floor_glow",
						   "ember_sight", "hand_puff", "spell_fire", "spell_water", "spell_air",
						   "spell_earth", "spell_flare"})
		if (!known(id))
			log::Warn("lights.cat has no [{}]; those lights use the fallback", id);

	// Item kinds are built once (PreloadItemKinds) and no other reload reaches
	// their `light` or `trail`, so re-read them here: re-pointed live.
	for (auto&& [id, kind] : m_itemKinds) {
		const CatalogEntry* def = m_project.FindItem(id);
		kind->light = CatalogGet(def, "light", kind->Lit() ? "torch" : "");
		kind->trail = CatalogGet(def, "trail", "");
	}

	// trails.cat (Phase 4), the same way: parsed, then every name checked.
	m_trailProfiles.clear();
	problems.clear();
	for (const CatalogEntry& e : m_project.trails.Entries())
		m_trailProfiles.push_back(trail::Parse(
			e.id, [&e](std::string_view key) { return e.Get(key); }, &problems));
	for (const std::string& p : problems) log::Warn("{}", p);
	const auto knownTrail = [this](std::string_view id) {
		for (const trail::Profile& p : m_trailProfiles)
			if (p.id == id) return true;
		return false;
	};
	for (const Catalog* cat : {&m_project.items, &m_project.weapons, &m_project.armor})
		for (const CatalogEntry& e : cat->Entries())
			if (const std::string id = e.Get("trail"); !id.empty() && !knownTrail(id))
				log::Warn("[{}]: trail '{}' is not in trails.cat; it sheds nothing", e.id, id);
	for (const CatalogEntry& e : m_project.spells.Entries()) {
		if (const std::string id = e.Get("light"); !id.empty() && !known(id))
			log::Warn("spells.cat [{}]: light '{}' is not in lights.cat; its bolt draws as "
					  "the fallback warm light", e.id, id);
		if (const std::string id = e.Get("trail"); !id.empty() && !knownTrail(id))
			log::Warn("spells.cat [{}]: trail '{}' is not in trails.cat; its bolt sheds nothing",
					  e.id, id);
	}
	// The ids a flight takes when nothing names its own (DungeonWorld_Flight.cpp).
	for (const char* id : {"bolt_fire", "bolt_earth", "bolt_air", "bolt_water", "bolt_shot"})
		if (!known(id)) log::Warn("lights.cat has no [{}]; those bolts use the fallback", id);
	for (const char* id : {"trail_fire", "trail_earth", "trail_air", "trail_water", "trail_shot"})
		if (!knownTrail(id)) log::Warn("trails.cat has no [{}]; those bolts shed nothing", id);
	log::Info("Light profiles: {} (lights.cat), trails: {} (trails.cat)", m_lightProfiles.size(),
			  m_trailProfiles.size());
}

const light::Profile& DungeonWorld::LightProfileFor(std::string_view id) const {
	for (const light::Profile& p : m_lightProfiles)
		if (p.id == id) return p;
	return light::Fallback();
}

Vec3 DungeonWorld::FixtureLightColor(const std::string& type) {
	return LightProfileFor(FixtureKindFor(type).light).color;
}

gfx::PointLight* DungeonWorld::PushLight(const light::Profile& profile, const char* source,
										 u32 key, const Vec3& pos, float time, float phase,
										 const Vec3& color, float brightness,
										 float radiusMetres) {
	if (brightness <= 0.0f) return nullptr;
	// The candidate ceiling (the lists were reserved to it): past it a light is
	// simply not considered, rather than growing a list inside a frame.
	if (m_lights.points.size() >= kLightCandidates) return nullptr;
	const light::Sample s = light::Evaluate(profile, time, phase);
	gfx::PointLight l;
	l.position = {pos.x + s.offset.x * kCellSize, pos.y + s.offset.y * kCellSize,
				  pos.z + s.offset.z * kCellSize};
	l.radius = radiusMetres > 0.0f ? radiusMetres : profile.radius * kCellSize;
	l.color = profile.sourceColor ? color : profile.color;
	l.intensity = s.intensity * brightness;
	l.castsShadow = profile.shadow;
	l.longShadowFade = profile.longFade;
	// A wandering origin re-renders its shadow cube on the flicker cadence
	// rather than on every sub-pixel move, and on a real move only past what the
	// wander alone could do (ShadowScheduler).
	l.wander = light::WanderSpan(profile.wander) * kCellSize;
	l.id = key;
	m_lights.points.push_back(l);
	// Which lights.cat profile it is, by index (-1 = one the catalog does not
	// hold: the fallback, or a built-in like the stress light). Compared with
	// std::less, the one pointer order defined across unrelated objects.
	int index = -1;
	const light::Profile* first = m_lightProfiles.data();
	if (!m_lightProfiles.empty() && !std::less<>{}(&profile, first) &&
		std::less<>{}(&profile, first + m_lightProfiles.size()))
		index = static_cast<int>(&profile - first);
	m_lightOrigins.push_back({source, index});
	return &m_lights.points.back();
}

std::vector<std::string> DungeonWorld::DescribeLights() const {
	std::vector<std::string> out;
	const Vec3 eye = PartyEye();
	const LightCull& c = m_lightCull;
	out.push_back(std::format(
		"lights: {} drawn of {} candidates (budget {}) - off screen {}, out of reach {}, "
		"over budget {}, fading out {}; {} profiles",
		m_lights.points.size(), c.candidates, m_settings.maxPointLights, c.offscreen,
		c.unreachable, c.budget, c.fadingOut, m_lightProfiles.size()));
	out.push_back(std::format("lights: tiles {} ({} tile-light pairs of {} untiled)",
							  m_renderer.LightTiling() ? "on" : "off",
							  m_renderer.TileLightPairs(),
							  m_lights.points.size() * gfx::kLightTileCount));
	const gfx::LightTiler tiler(m_camera.ViewProj());
	const Vec3 cam = m_camera.Position();
	const Vec3 fwd = m_camera.Forward();
	out.push_back(std::format("lights: camera at {:.2f} {:.2f} {:.2f} looking {:.2f} {:.2f} {:.2f}",
							  cam.x, cam.y, cam.z, fwd.x, fwd.y, fwd.z));
	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& l = m_lights.points[i];
		const LightOrigin origin = i < m_lightOrigins.size() ? m_lightOrigins[i] : LightOrigin{};
		gfx::TileRange t;
		const bool seen = tiler.Range(l.position, l.radius, t);
		out.push_back(seen ? std::format("       tiles cols {}-{} rows {}-{} (light at {:.2f} {:.2f} {:.2f})",
										 t.c0, t.c1, t.r0, t.r1, l.position.x, l.position.y,
										 l.position.z)
						   : std::format("       tiles none (light at {:.2f} {:.2f} {:.2f})",
										 l.position.x, l.position.y, l.position.z));
		const int p = origin.profile;
		const char* profile = p >= 0 && p < static_cast<int>(m_lightProfiles.size())
								  ? m_lightProfiles[static_cast<size_t>(p)].id.c_str()
								  : "(built-in)";
		const Vec3 d = Sub(l.position, eye);
		out.push_back(std::format(
			"  [{:2}] {:<8} {:<13} rgb {:.2f} {:.2f} {:.2f}  i {:.2f}  r {:.1f} m  at {:.1f} m  "
			"fade {:.2f}  {}",
			i, origin.source, profile, l.color.x, l.color.y, l.color.z, l.intensity, l.radius,
			std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), origin.fade,
			!l.castsShadow ? "no shadow"
			: l.shadowSlot >= 0 ? std::format("shadow slot {}", l.shadowSlot)
								: std::string("shadow (no slot)")));
	}
	return out;
}

std::vector<std::string> DungeonWorld::DescribeShadows() const {
	// A light by its kind, as LightKind names it, and which one.
	static constexpr const char* kKinds[] = {"?",     "torch",      "fire",  "burning",  "glow",
											 "sight", "stress",     "bolt",  "floortorch", "flash",
											 "handglow", "worn",    "spell", "stone"};
	static_assert(std::size(kKinds) == static_cast<size_t>(LightKind::Stone) + 1,
				  "a LightKind with no name in the shadow readout");
	const auto name = [](u32 id) -> std::string {
		if (id == 0 || id == 0xFFFFFFFFu) return "none";
		if (id & 0x80000000u) return std::format("index:{}", id & 0x7FFFFFFFu);
		const u32 kind = id >> 24;
		return std::format("{}:{}", kind < std::size(kKinds) ? kKinds[kind] : "?", id & 0xFFFFFFu);
	};
	const ShadowScheduler::Stats& s = m_shadows.GetStats();
	std::vector<std::string> out;
	out.push_back(std::format(
		"shadows: {} passes={} notes={} overflows={} swept={} rate={:.1f}hz budget={} "
		"ignore notes={} moves={}",
		m_shadowsEnabled ? "on" : "off", s.passes, s.notes, s.overflows, s.swept,
		m_shadows.FlickerHz(), m_shadows.FlickerBudget(), m_shadows.IgnoresNotes() ? "on" : "off",
		m_shadows.IgnoresMoves() ? "on" : "off"));
	using Reason = ShadowScheduler::Reason;
	for (size_t slot = 0; slot < s.slots.size(); ++slot) {
		// Who holds it THIS frame, which need not be who it last rendered for.
		u32 holder = 0;
		for (const gfx::PointLight& l : m_lights.points)
			if (l.shadowSlot == static_cast<int>(slot)) holder = l.id;
		const ShadowScheduler::SlotStats& st = s.slots[slot];
		std::string line = std::format("shadows slot {}: light={} last={} renders={}", slot,
									   name(holder), name(st.lightId), st.renders);
		for (size_t r = 0; r < static_cast<size_t>(Reason::Count); ++r)
			line += std::format(" {}={}", ShadowScheduler::ReasonName(static_cast<Reason>(r)),
								st.by[r]);
		// When it last rendered, and how far its light has stood from that cube
		// while it was reused - in SQUARES, so a harness's bound survives kUnit.
		line += std::format(" lastpass={} lag={:.3f}", st.lastPass, st.lag / kCellSize);
		out.push_back(std::move(line));
	}
	return out;
}

std::string DungeonWorld::DescribeDoorShadow(int x, int z) const {
	for (const Door& d : m_doors)
		if (d.x == x && d.z == z)
			return std::format("shadows door {},{}: open={} openT={:.3f} pull={:.3f} posepass={} "
							   "passes={}",
							   x, z, d.open ? 1 : 0, d.openT, d.pullT, d.posePass,
							   m_shadows.GetStats().passes);
	return {};
}

float DungeonWorld::TorchBrightness(const ItemKind& kind, float charge) {
	if (!kind.Lit()) return 0.0f;
	const float left = charge < 0.0f ? kind.burnTime : charge;
	const float share = left / kind.burnTime;
	return share >= kDimShare ? 1.0f
							  : kDimFloor + (1.0f - kDimFloor) * std::max(share, 0.0f) / kDimShare;
}

// --- torch commands (the hand menu's Put out / Light) --------------------------

bool DungeonWorld::MagicalTorch(const ItemKind& kind) {
	if (kind.Lit()) return kind.powerLevel > 0.0f;
	return !kind.litAs.empty() && ItemKindFor(kind.litAs).powerLevel > 0.0f;
}

DungeonWorld::TorchAct DungeonWorld::TorchActFor(const std::string& typeId) {
	if (typeId.empty()) return TorchAct::None;
	const ItemKind& kind = ItemKindFor(typeId);
	if (kind.Lit()) return kind.unlitAs.empty() ? TorchAct::None : TorchAct::PutOut;
	// An ordinary torch is lit by fire (Flame, a sconce); only a magical one by
	// its own word.
	return MagicalTorch(kind) ? TorchAct::Light : TorchAct::None;
}

bool DungeonWorld::PutOutTorch(size_t member, int hand) {
	if (!m_roster || member >= m_roster->size() || hand < 0 || hand > 1) return false;
	Character& c = (*m_roster)[member];
	ItemSlot& slot = c.inventory.Hand(hand);
	if (TorchActFor(slot.typeId) != TorchAct::PutOut) return false;
	// Into the slot's own buffer, keeping its charge: it relights with what it had.
	slot.typeId.assign(ItemKindFor(slot.typeId).unlitAs);
	MemberMessage(c, loc::FormatLine("log.torch_put_out", c.name,
									 loc::View(ItemKindFor(slot.typeId).nameKey)));
	return true;
}

bool DungeonWorld::KindleTorch(size_t member, int hand) {
	if (!m_roster || member >= m_roster->size() || hand < 0 || hand > 1) return false;
	Character& c = (*m_roster)[member];
	if (!c.IsAlive()) return false;
	ItemSlot& slot = c.inventory.Hand(hand);
	if (TorchActFor(slot.typeId) != TorchAct::Light) return false;
	const ItemKind& unlit = ItemKindFor(slot.typeId);
	const ItemKind& lit = ItemKindFor(unlit.litAs);
	// The mana it asks is its power: torch_light_mana for each power level.
	const float cost = m_balance.torchLightMana * lit.powerLevel;
	if (c.mana < cost) {
		MemberMessage(c, loc::FormatLine("log.torch_no_mana", c.name, loc::View(unlit.nameKey),
										 static_cast<int>(std::ceil(cost))));
		return false;
	}
	c.mana -= cost;
	slot.typeId.assign(unlit.litAs);
	MemberMessage(c, loc::FormatLine("log.torch_kindled", c.name, loc::View(unlit.nameKey)));
	return true;
}

bool DungeonWorld::BurnTorch(ItemSlot& slot, float dt, const Character* holder) {
	const ItemKind& kind = ItemKindFor(slot.typeId);
	if (!kind.Lit()) return false;
	if (slot.charge < 0.0f) slot.charge = kind.burnTime; // a fresh one starts full
	slot.charge -= dt;
	if (slot.charge > 0.0f) return false;
	// Burnt out: the stub (or nothing, for a kind that leaves none). The line
	// names the torch by its UNLIT name - "Sera's torch burns out", not "Sera's
	// Lit torch".
	if (holder) {
		const ItemKind& named = kind.unlitAs.empty() ? kind : ItemKindFor(kind.unlitAs);
		MemberMessage(*holder, loc::FormatLine("log.torch_spent", holder->name,
											   loc::View(named.nameKey)));
	}
	if (kind.spentAs.empty()) {
		slot.Clear();
	} else {
		slot.typeId.assign(kind.spentAs);
		slot.charge = kNoCharge;
	}
	return true;
}

void DungeonWorld::TickCarriedLight(float dt) {
	if (!m_roster) return;
	for (Character& c : *m_roster) {
		// In a HAND it burns (a downed member's torch burns on in their grip).
		for (int h = 0; h < 2; ++h) {
			ItemSlot& slot = c.inventory.Hand(h);
			if (!slot.Empty()) BurnTorch(slot, dt, &c);
		}
		// In a PACK it goes out, keeping what is left of it.
		for (Pack& pack : c.inventory.packs)
			for (ItemSlot& s : pack.contents) {
				if (s.Empty()) continue;
				const ItemKind& kind = ItemKindFor(s.typeId);
				if (kind.Lit() && !kind.unlitAs.empty()) s.typeId.assign(kind.unlitAs);
			}
	}
	// On the CURSOR it is still in the leader's hand: it burns.
	// (Through a member scratch slot, swapped in and out: a local ItemSlot would
	// construct a string every frame, which the debug CRT allocates for.)
	if (m_cursorItem && m_cursorItem->has_value()) {
		ItemSlot& held = m_cursorScratch;
		held.typeId.swap(m_cursorItem->Id());
		held.charge = m_cursorItem->Charge();
		BurnTorch(held, dt, LeaderMember());
		m_cursorItem->Id().swap(held.typeId);
		m_cursorItem->SetCharge(held.charge);
		held.Clear();
	}
}

// --- lit torches on the floor (Phase 4) ----------------------------------------

void DungeonWorld::TickFloorTorches(float dt) {
	for (size_t i = 0; i < m_items.size(); ++i) {
		if (m_items[i].collected || !m_items[i].kind || !m_items[i].kind->Lit()) continue;
		if (m_items[i].id >= 0) {
			// An AUTHORED torch: once it burns it is state its .ent record cannot
			// describe, so it becomes a drop the save carries whole (kind and
			// charge) - the record is left behind as taken. Once per torch.
			Item drop = m_items[i];
			drop.id = m_nextDropId--;
			m_items[i].collected = true;
			PlaceDrop(drop); // may append: nothing below holds a reference
			continue;
		}
		Item& it = m_items[i];
		const ItemKind& kind = *it.kind;
		if (it.charge < 0.0f) it.charge = kind.burnTime; // a fresh one starts full
		it.charge -= dt;
		if (it.charge > 0.0f) continue;
		// Burnt out where it lies: its stub, or nothing for a kind that leaves none.
		// Either way what the cubes hold of it changes (code-review C178).
		NoteItemCaster(it);
		if (kind.spentAs.empty()) {
			it.collected = true;
		} else {
			it.kind = &ItemKindFor(kind.spentAs);
			it.charge = kNoCharge;
		}
	}
}

void DungeonWorld::AppendFloorTorchLights(float time) {
	for (size_t i = 0; i < m_items.size(); ++i) {
		const Item& it = m_items[i];
		if (it.collected || !it.kind || !it.kind->Lit()) continue;
		// In a shut niche it is hidden, and its light with it.
		if (it.niche >= 0 && !NicheOpenAt(it.x, it.z, static_cast<Direction>(it.niche))) continue;
		const ItemKind& kind = *it.kind;
		const light::Profile& profile =
			LightProfileFor(kind.light.empty() ? std::string_view("torch") : kind.light);
		const float brightness = TorchBrightness(kind, it.charge);
		const Vec3 head = FloorTorchHead(it);
		if (gfx::PointLight* l =
				PushLight(profile, "floor", LightKey(LightKind::FloorTorch, static_cast<u32>(i)),
						  {head.x, head.y + 0.12f, head.z}, time, static_cast<float>(i) * 1.3f,
						  {1, 1, 1}, brightness,
						  profile.radius * kCellSize * (0.6f + 0.4f * brightness));
			l && kind.flameTinted)
			l->color = kind.flameColor;
	}
}

void DungeonWorld::AppendCarriedLights(float time) {
	const Vec3 eye = PartyEye(); // a carried light falls with the camera
	// Its kind's profile (items.cat `light`), dimmed by its charge: the reach
	// shrinks to 60% and the brightness to the dim floor over the last tenth.
	// `slot` names the hand it is in (member x 2 + hand; the cursor is its own):
	// it keys the light and sets its flicker's phase.
	const auto add = [&](const ItemKind& kind, const Vec3& at, float brightness, u32 slot) {
		const light::Profile& profile =
			LightProfileFor(kind.light.empty() ? std::string_view("torch") : kind.light);
		gfx::PointLight* light =
			PushLight(profile, "torch", LightKey(LightKind::Torch, slot), at, time,
					  static_cast<float>(slot) * 1.7f, {1, 1, 1}, brightness,
					  profile.radius * kCellSize * (0.6f + 0.4f * brightness));
		// A magical torch (items.cat `flame_color`) lights in its flame's colour.
		if (light && kind.flameTinted) light->color = kind.flameColor;
	};
	if (m_roster) {
		const Direction faced = static_cast<Direction>(m_party.Facing());
		for (size_t m = 0; m < m_roster->size(); ++m) {
			const Character& c = (*m_roster)[m];
			if (!c.IsAlive()) continue;
			// At the member's own side of the eye, as their casts and throws leave
			// from (the quadrant lane), a little lower than the old eye light.
			const Direction lateral =
				static_cast<Direction>(facing::SlotSide(static_cast<int>(faced), m));
			const Vec3 at{eye.x + static_cast<float>(DirDX(lateral)) * kCellSize * 0.15f,
						  eye.y + 0.2f,
						  eye.z + static_cast<float>(DirDZ(lateral)) * kCellSize * 0.15f};
			for (int h = 0; h < 2; ++h) {
				const ItemSlot& slot = c.inventory.Hand(h);
				if (slot.Empty()) continue;
				const ItemKind& kind = ItemKindFor(slot.typeId);
				if (kind.Lit())
					add(kind, at, TorchBrightness(kind, slot.charge),
						static_cast<u32>(m * 2 + static_cast<size_t>(h)));
			}
			// WORN LIGHT (Phase 5): anything on the doll - or in a hand - whose
			// kind names a `light` and does not burn: a glowing amulet, a lit gem.
			// Steady at full strength (it has no charge to spend), from the
			// member's side like their torch: in a hand at the torch's height,
			// worn a little lower, at the chest.
			for (int s = 0; s < kEquipCount; ++s) {
				const ItemSlot& slot = c.inventory.equipment[static_cast<size_t>(s)];
				if (slot.Empty()) continue;
				const ItemKind& kind = ItemKindFor(slot.typeId);
				if (kind.Lit() || kind.light.empty()) continue; // a torch is the above
				const bool held = s == static_cast<int>(EquipSlot::LeftHand) ||
								  s == static_cast<int>(EquipSlot::RightHand);
				const u32 key = static_cast<u32>(m * static_cast<size_t>(kEquipCount)) +
								static_cast<u32>(s);
				PushLight(LightProfileFor(kind.light), "worn", LightKey(LightKind::Worn, key),
						  {at.x, held ? at.y : eye.y - 0.25f, at.z}, time,
						  static_cast<float>(key) * 1.3f);
			}
		}
	}
	if (m_cursorItem && m_cursorItem->has_value()) {
		const ItemKind& kind = ItemKindFor(**m_cursorItem);
		if (kind.Lit())
			add(kind, {eye.x, eye.y + 0.2f, eye.z}, TorchBrightness(kind, m_cursorItem->Charge()),
				kCursorTorchSlot);
	}
}

} // namespace dungeon::game
