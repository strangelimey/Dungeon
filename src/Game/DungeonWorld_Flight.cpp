// ============================================================================
// Game/DungeonWorld_Flight.cpp - what a thing in flight lights and sheds
// (lighting-updates Phase 4, docs/lighting-updates-plan.md).
//
// Every launch - a party bolt, each bolt of a volley as it leaves the queue, a
// monster's shot, a thrown item - goes through Launch, which DRESSES the spec
// first: its light profile and its trail are resolved from what made it, most
// particular first:
//   1. the names it carries - a spell's own spells.cat `light` / `trail`;
//   2. its CARGO kind's - items.cat `light` (a lit torch's `torch`) / `trail`;
//   3. its SCHOOL'S default - `bolt_fire` / `trail_fire` and so on;
//   4. a plain monster shot, which has none of those: `bolt_shot` / `trail_shot`.
// Then each flight is its own light, keyed by its projectile id (Phase 3's
// stable key), on from the moment it launches until it lands; a lit bolt's
// end leaves a flash a moment longer. Nothing assumes a volley flies together:
// each bolt is launched, lit, shed and ended on its own.
//
// Nothing here allocates: a dress looks profiles up by id (views), copies a
// trail spec of plain numbers, and the lights go through PushLight.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Log.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {
// The light handle of a flight whose named profile the project lacks: the
// built-in fallback (ReloadLightProfiles warns about the name once).
constexpr int kFallbackLight = 0x7FFFFFFF;

std::string_view SchoolLight(SpellSymbol s) {
	switch (s) {
	case SpellSymbol::Fire: return "bolt_fire";
	case SpellSymbol::Earth: return "bolt_earth";
	case SpellSymbol::Air: return "bolt_air";
	case SpellSymbol::Water: return "bolt_water";
	default: return {};
	}
}

std::string_view SchoolTrail(SpellSymbol s) {
	switch (s) {
	case SpellSymbol::Fire: return "trail_fire";
	case SpellSymbol::Earth: return "trail_earth";
	case SpellSymbol::Air: return "trail_air";
	case SpellSymbol::Water: return "trail_water";
	default: return {};
	}
}

// A glow colour (often past 1, it is additive) as a light colour: its own hue,
// brightest channel 1.
Vec3 Hue(const Vec4& c) {
	const float m = std::max({c.x, c.y, c.z, 1e-4f});
	return {c.x / m, c.y / m, c.z / m};
}
} // namespace

const trail::Spec& DungeonWorld::TrailSpecFor(std::string_view id) const {
	for (const trail::Profile& p : m_trailProfiles)
		if (p.id == id) return p.spec;
	static const trail::Spec kNone{};
	return kNone;
}

void DungeonWorld::DressFlight(ProjectileSpec& spec) const {
	if (spec.dressed) return;
	spec.dressed = true;
	const ItemKind* cargo = static_cast<const ItemKind*>(spec.cargo);
	const bool school = spec.payload.flavour.has_value();

	// The light: the spec's own name, else the cargo's, else the school's.
	std::string_view lightId = spec.lightId;
	if (lightId.empty() && cargo) lightId = cargo->light;
	if (lightId.empty() && !cargo)
		lightId = school ? SchoolLight(*spec.payload.flavour) : std::string_view("bolt_shot");
	spec.light = -1;
	if (!lightId.empty()) {
		spec.light = kFallbackLight;
		for (size_t i = 0; i < m_lightProfiles.size(); ++i)
			if (m_lightProfiles[i].id == lightId) {
				spec.light = static_cast<int>(i);
				break;
			}
	}

	// Its colour: what its source looks like (a school's element, a magical
	// torch's flame, the shot's own glow) - unless the profile has a colour of
	// its own, which then IS the light's colour, and the trail's.
	Vec3 source = school ? Hue(ElementColor(*spec.payload.flavour)) : Hue(spec.color);
	if (cargo) source = cargo->flameTinted ? cargo->flameColor : Vec3{1.0f, 0.62f, 0.28f};
	spec.lightColor = source;
	if (spec.light >= 0) {
		const light::Profile& p = spec.light < static_cast<int>(m_lightProfiles.size())
									  ? m_lightProfiles[static_cast<size_t>(spec.light)]
									  : light::Fallback();
		if (!p.sourceColor && !(cargo && cargo->flameTinted)) spec.lightColor = p.color;
	}

	// The trail, likewise - a lit torch needs none: it trails its own flame.
	std::string_view trailId = spec.trailId;
	if (trailId.empty() && cargo) trailId = cargo->trail;
	if (trailId.empty() && !cargo)
		trailId = school ? SchoolTrail(*spec.payload.flavour) : std::string_view("trail_shot");
	spec.trail = trailId.empty() ? trail::Spec{} : TrailSpecFor(trailId);

	// The names were borrowed from the spell or kind that made the spec; a
	// dressed spec may wait in a queue past a catalog reload.
	spec.lightId = {};
	spec.trailId = {};
}

void DungeonWorld::Launch(ProjectileSpec spec) {
	DressFlight(spec);
	m_projectiles.Spawn(spec);
}

void DungeonWorld::AppendFlightLights(float time) {
	const auto profileOf = [this](int handle) -> const light::Profile& {
		return handle >= 0 && handle < static_cast<int>(m_lightProfiles.size())
				   ? m_lightProfiles[static_cast<size_t>(handle)]
				   : light::Fallback();
	};
	// Each flight its own light. A thrown torch burns as it does in a hand:
	// dimmed by what is left of it, its reach shrinking with it.
	m_projectiles.ForEachLit([&](u32 id, const Vec3& pos, int handle, const Vec3& color,
								 const void* cargo, float charge) {
		const light::Profile& p = profileOf(handle);
		float brightness = 1.0f;
		float radius = 0.0f;
		if (cargo) {
			const ItemKind& kind = *static_cast<const ItemKind*>(cargo);
			if (kind.Lit()) {
				brightness = TorchBrightness(kind, charge);
				radius = p.radius * kCellSize * (0.6f + 0.4f * brightness);
			}
		}
		if (gfx::PointLight* l = PushLight(p, "bolt", LightKey(LightKind::Bolt, id), pos, time,
										   static_cast<float>(id) * 0.37f, color, brightness,
										   radius))
			l->color = color; // the dressed colour is already the profile's or the source's
	});
	// And where a lit bolt ended, its light a moment longer, falling away fast.
	m_projectiles.ForEachFlash([&](size_t slot, const Vec3& pos, int handle, const Vec3& color,
								   float left) {
		if (gfx::PointLight* l =
				PushLight(profileOf(handle), "flash",
						  LightKey(LightKind::Flash, static_cast<u32>(slot)), pos, time, 0.0f,
						  color, 1.4f * left * left))
			l->color = color;
	});
}

std::vector<std::string> DungeonWorld::DescribeTrails() const {
	std::vector<std::string> out;
	const ProjectileSystem::PoolStats s = m_projectiles.Stats();
	out.push_back(std::format(
		"trails: {} flights ({} shedding); spark pool {} / {} live, {} of them trail; last "
		"second {} recycled, {} refused",
		s.flights, s.shedding, s.live, s.capacity, s.trail, s.recycled, s.refused));
	for (const trail::Profile& p : m_trailProfiles) {
		const trail::Spec& t = p.spec;
		out.push_back(std::format(
			"  [{}] {} rate {:.1f}/sq life {:.2f} size {:.3f} spread {:.2f} fall {:.1f}{}{}{}  {}",
			p.id, trail::ShapeName(t.shape), t.rate, t.life, t.size, t.spread, t.fall,
			t.swell ? " swell" : "", t.flicker > 0.0f ? std::format(" flicker {:.2f}", t.flicker) : "",
			t.swirl > 0.0f ? std::format(" swirl {:.1f}", t.swirl) : "",
			t.hasColor ? std::format("rgb {:.2f} {:.2f} {:.2f}", t.color.x, t.color.y, t.color.z)
					   : std::string("colour of its light")));
	}
	return out;
}

} // namespace dungeon::game
