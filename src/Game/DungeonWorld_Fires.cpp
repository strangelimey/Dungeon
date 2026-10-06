// ============================================================================
// Game/DungeonWorld_Fires.cpp - fires as LIVE state: a wall torch or brazier lit
// or put out in play, a gust fanning one, and the smoke a fire leaves when it
// goes out. The authored `lit` on the map record never changes here (DungeonMap
// WallSconce::Burning); the save keeps the difference.
//
// THE SMOKE GOES THROUGH THE EFFECTS SYSTEM (Michael): a fire put out takes its
// kind's on_douse effects (fixtures.cat, `smoke <power> <seconds>`) onto its own
// effect list, and the haze over its square is READ OFF that list each frame -
// power x the share of its time left, so it thins to nothing - the way a
// burning body's plume is read off `plume`. Nothing here stores a puff.
//
// Everything here can run inside a cast, a frame the steady-state allocation
// guard watches: the flame is re-lit in its reserved buffer (FireEffect::
// Ignite), the effect list was reserved at build, the haze grid is refilled in
// place and copied into its texture by the next scene pass
// (RefreshTurbidity / Renderer::UpdateTexture), and the puffs ride the frame
// constants.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// How long a flare takes to die away.
constexpr float kFlareSeconds = 1.2f;
// How far a doused fire's smoke spreads from its square's centre, in squares.
constexpr float kSmokeRadiusCells = 1.5f;
} // namespace

DungeonWorld::Fire* DungeonWorld::FindFire(int x, int z, int wall) {
	for (Fire& f : m_fires)
		if (f.x == x && f.z == z && f.wall == wall && f.brazier == (wall < 0)) return &f;
	return nullptr;
}

bool DungeonWorld::SetFireBurning(int x, int z, int wall, bool burning, bool smoke) {
	Fire* fire = FindFire(x, z, wall);
	// A bowl with no fuel never catches (fixtures.cat `flame = 0`).
	if (burning && fire && fire->kind && fire->kind->flameless) return false;
	if (!m_map.SetFixtureBurning(x, z, wall, burning)) return false;
	if (fire) {
		const bool wasLit = fire->lit;
		fire->lit = burning;
		fire->flare = 0.0f;
		fire->effect.SetFlare(0.0f);
		if (burning) {
			// Re-lit in its own reserved buffer, pre-warmed so it is not cold.
			fire->effect.Ignite(fire->flamePos, static_cast<u32>(fire->phase * 977.0f));
		} else {
			fire->effect.Clear();
			// What going out leaves behind, through the effects system: the
			// kind's on_douse effects land on the fire itself (its smoke).
			if (smoke && wasLit && fire->kind)
				for (const fx::Proc& p : fire->kind->onDouse)
					if (const fx::EffectKind* k = m_effects.Find(p.id.View()))
						fx::Apply(fire->effects, *k, k->DefaultSchool(), p.magnitude,
								  p.duration);
		}
	}
	// Its smoke ring came or went with it (DungeonMap::RebuildTurbidity ran).
	RefreshTurbidity();
	return true;
}

bool DungeonWorld::SetSconceEmpty(int x, int z, int wall, bool empty, bool burning) {
	Fire* fire = FindFire(x, z, wall);
	if (!fire || !fire->kind || !fire->kind->meshEmpty) return false; // not takeable
	if (!m_map.SetSconceEmpty(x, z, wall, empty, burning)) return false;
	fire->empty = empty;
	// The bracket swaps meshes (torch in / bare), which no map revision says
	// (code-review C178): the draw's own sphere.
	m_shadows.NoteCasterChanged(fire->flamePos, 0.5f * kUnit);
	const bool lit = !empty && burning && !fire->kind->flameless;
	fire->flare = 0.0f;
	fire->effect.SetFlare(0.0f);
	if (lit != fire->lit) {
		fire->lit = lit;
		if (lit) fire->effect.Ignite(fire->flamePos, static_cast<u32>(fire->phase * 977.0f));
		else fire->effect.Clear();
	}
	RefreshTurbidity();
	return true;
}

bool DungeonWorld::SconceUnderCursor(float mx, float my, float w, float h, int& x, int& z,
									 int& wall) const {
	// The wall torch the party FACES, from its own square - the lever's rule -
	// and the click has to be ON it: a ball round the torch, as a door's panel
	// is hit-tested, so a click elsewhere in the view is not a grab.
	if (!FireAheadCell(x, z, wall) || wall < 0) return false;
	for (const Fire& f : m_fires) {
		if (f.x != x || f.z != z || f.wall != wall) continue;
		const gfx::Camera::Ray ray = m_camera.ScreenRay(mx, my, w, h);
		const Vec3 c{f.flamePos.x, f.flamePos.y - 0.15f * kUnit, f.flamePos.z};
		const Vec3 oc{ray.origin.x - c.x, ray.origin.y - c.y, ray.origin.z - c.z};
		const float r = 0.22f * kUnit;
		const float b = oc.x * ray.dir.x + oc.y * ray.dir.y + oc.z * ray.dir.z;
		const float cc = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - r * r;
		return b * b - cc >= 0.0f && -b > 0.0f;
	}
	return false;
}

bool DungeonWorld::TakeTorchAhead(float mx, float my, float w, float h, HeldItem& cursor) {
	int x = 0, z = 0, wall = -1;
	return !cursor.has_value() && SconceUnderCursor(mx, my, w, h, x, z, wall) &&
		   TakeTorchAt(x, z, wall, cursor);
}

bool DungeonWorld::TakeTorchAt(int x, int z, int wall, HeldItem& cursor) {
	const Fire* fire = FindFire(x, z, wall);
	const WallSconce* sconce = m_map.SconceAt(x, z, wall);
	if (!fire || fire->empty || !fire->kind || !sconce) return false;
	// What comes off the wall: the torch that went in (the kind's own when none
	// was recorded), lit when the sconce was, with the charge it went in with.
	const std::string& base = sconce->torch.empty() ? fire->kind->torchItem : sconce->torch;
	if (base.empty()) return false;
	const ItemKind& torch = ItemKindFor(base);
	const float charge = sconce->torchCharge;
	const bool lit = fire->lit;
	const std::string& id = lit && !torch.litAs.empty() ? torch.litAs : torch.id;
	if (!SetSconceEmpty(x, z, wall, true)) return false;
	m_map.SetSconceTorch(x, z, wall, {}, kNoCharge); // the bracket is bare now
	// Into the LEADER's free hand (right, then left); with both full, onto the
	// cursor as anything lifted is.
	Character* leader = m_roster && m_leader >= 0 && static_cast<size_t>(m_leader) < m_roster->size()
							? &(*m_roster)[static_cast<size_t>(m_leader)]
							: nullptr;
	bool inHand = false;
	if (leader)
		for (int h = 1; h >= 0 && !inHand; --h)
			if (ItemSlot& slot = leader->inventory.Hand(h); slot.Empty()) {
				slot.typeId.assign(id);
				slot.charge = charge;
				inHand = true;
			}
	if (!inHand) cursor.Set(id, charge);
	m_audio.Play(m_sounds.click, 0.6f);
	if (onMessage)
		onMessage(loc::FormatLine("log.take_item", LeaderName(),
								  loc::View(ItemKindFor(id).nameKey)));
	return true;
}

bool DungeonWorld::MountTorchAhead(const std::string& itemId, float mx, float my, float w,
								   float h, float charge) {
	int x = 0, z = 0, wall = -1;
	return !itemId.empty() && SconceUnderCursor(mx, my, w, h, x, z, wall) &&
		   MountTorchAt(x, z, wall, itemId, charge);
}

bool DungeonWorld::MountTorchAt(int x, int z, int wall, const std::string& itemId,
								float charge) {
	const Fire* fire = FindFire(x, z, wall);
	if (!fire || !fire->empty || !fire->kind) return false;
	// Any torch that can burn - lit or not - goes in; a stub does not.
	const ItemKind& kind = ItemKindFor(itemId);
	if (!kind.Lit() && kind.litAs.empty()) return false;
	if (!SetSconceEmpty(x, z, wall, false, kind.Lit())) return false;
	// Remember WHICH torch: its unlit id (whether it burns is the sconce's
	// state) and its charge, which a bracket does not burn down. The fixture's
	// own torch, fresh, is the default and records nothing, so a bracket that
	// got its own torch back saves no line.
	const std::string& base = kind.Lit() && !kind.unlitAs.empty() ? kind.unlitAs : kind.id;
	if (base == fire->kind->torchItem && charge < 0.0f)
		m_map.SetSconceTorch(x, z, wall, {}, kNoCharge);
	else
		m_map.SetSconceTorch(x, z, wall, base, charge);
	m_audio.Play(m_sounds.click, 0.5f);
	return true;
}

std::string_view DungeonWorld::SconceTorch(int x, int z, int wall) {
	const Fire* fire = FindFire(x, z, wall);
	const WallSconce* sconce = m_map.SconceAt(x, z, wall);
	if (!fire || fire->empty || !fire->kind || !sconce) return {};
	return sconce->torch.empty() ? std::string_view(fire->kind->torchItem)
								 : std::string_view(sconce->torch);
}

bool DungeonWorld::FlareFire(int x, int z, int wall) {
	Fire* fire = FindFire(x, z, wall);
	if (!fire || !fire->lit) return false;
	fire->flare = 1.0f;
	return true;
}

void DungeonWorld::SyncFiresFromMap() {
	// After the map's burning states were set wholesale (a new game, a reset):
	// each live fire takes its record's state, relit or put out to match, with
	// nothing left hanging from before.
	const auto sync = [&](int x, int z, int wall, bool burning, bool empty) {
		Fire* fire = FindFire(x, z, wall);
		if (!fire) return;
		fire->empty = empty;
		const bool lit = burning && !(fire->kind && fire->kind->flameless);
		fire->flare = 0.0f;
		fire->effect.SetFlare(0.0f);
		fire->effects.clear();
		if (lit == fire->lit) return;
		fire->lit = lit;
		if (lit) fire->effect.Ignite(fire->flamePos, static_cast<u32>(fire->phase * 977.0f));
		else fire->effect.Clear();
	};
	for (const WallSconce& s : m_map.Sconces())
		sync(s.x, s.z, static_cast<int>(s.wall), s.Burning(), s.empty);
	for (const FloorBrazier& b : m_map.Braziers()) sync(b.x, b.z, -1, b.Burning(), false);
	RefreshTurbidity();
}

void DungeonWorld::UpdateFireTransients(float dt) {
	for (Fire& f : m_fires) {
		if (f.flare > 0.0f) {
			f.flare = std::max(0.0f, f.flare - dt / kFlareSeconds);
			f.effect.SetFlare(f.flare);
		}
		// A fire's effects AGE like any bearer's. There is no TickEffects here
		// because a fire is not an fx::ITarget - nothing can wound it as a fire
		// (smashing one is its FixtureBreak's business) - so nothing it carries
		// can bite; it only runs out.
		if (f.effects.empty()) continue;
		for (fx::Inst& e : f.effects) e.timeLeft -= dt;
		std::erase_if(f.effects, [](const fx::Inst& e) { return e.timeLeft <= 0.0f; });
	}
}

void DungeonWorld::GatherDustPuffs(gfx::Atmosphere& atmo) const {
	// The strongest haze effects any fire carries, at most kMaxDustPuffs: power x
	// the share of its time left, centred on the fire's square.
	std::array<float, gfx::kMaxDustPuffs> held{};
	for (const Fire& f : m_fires)
		for (const fx::Inst& e : f.effects) {
			if (!e.kind || !e.kind->Haze() || e.duration <= 0.0f) continue;
			const float strength = e.magnitude * std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f);
			if (strength <= 0.0f) continue;
			// Into the weakest slot, if it beats it.
			size_t weakest = 0;
			for (size_t i = 1; i < held.size(); ++i)
				if (held[i] < held[weakest]) weakest = i;
			if (strength <= held[weakest]) continue;
			held[weakest] = strength;
			const Vec3 c = m_map.CellCenter(f.x, f.z);
			atmo.dustPuffs[weakest] = {c.x, c.z, kSmokeRadiusCells * kCellSize, strength};
		}
}

} // namespace dungeon::game
