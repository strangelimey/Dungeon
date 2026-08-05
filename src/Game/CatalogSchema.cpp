// ============================================================================
// Game/CatalogSchema.cpp — see CatalogSchema.h. One table per catalog category.
//
// Rows use DESIGNATED initializers: a row names the members it sets, so the
// table reads as documentation and inserting a member into FieldSpec doesn't
// silently reinterpret every row.
// ============================================================================
#include "Game/CatalogSchema.h"

#include <cctype>

namespace dungeon::game {

namespace {
// --- rows shared by several categories --------------------------------------
// Every prop-like category (a model + a texture set + draw flags) repeats these,
// so they are written once and spliced into the tables below — a table stays a
// flat literal you can read top to bottom, which is the point.
#define IDENTITY_DISPLAY                                                       \
	{.key = "display", .kind = FieldKind::Text, .sectionKey = kSectionIdentity, \
	 .help = "Name shown in the editor palette and inspectors."}
#define IDENTITY_CATEGORY                                                       \
	{.key = "category", .kind = FieldKind::Text, .sectionKey = kSectionIdentity, \
	 .help = "Groups this type under a collapsible sub-accordion in the palette."}
#define PROP_MODEL                                                        \
	{.key = "model", .kind = FieldKind::Model, .sectionKey = kSectionLook, \
	 .help = "The mesh in assets/models (without the extension)."}
#define PROP_TEXTURE                                                             \
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook, \
	 .help = "PBR set bound by name; empty keeps the model's own glTF material."}
#define PROP_SCALE                                                        \
	{.key = "scale", .kind = FieldKind::Float, .sectionKey = kSectionLook, \
	 .help = "Trims the authored size (1 = as authored, in dungeon squares).",  \
	 .lo = 0.1f, .hi = 4.0f, .step = 0.05f, .def = "1"}
#define PROP_AUTHORED                                                        \
	{.key = "authored", .kind = FieldKind::Bool, .sectionKey = kSectionLook,  \
	 .help = "Back-face cull (on for bought/authored meshes, off for hand-built ones).", \
	 .def = "1"}
#define MATERIAL_ROWS                                                              \
	{.key = "metallic", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,   \
	 .help = "Scales the ORM map's metalness; absent leaves the map authoritative.", \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .neutral = "1"},                        \
	{.key = "roughness", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,  \
	 .help = "Scales the ORM map's roughness; absent leaves the map authoritative.", \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .neutral = "1"},                        \
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionMaterial, \
	 .help = "Parallax depth of the height map, in units. 0 = flat.",               \
	 .lo = 0.0f, .hi = 0.2f, .step = 0.005f, .neutral = "0.05"}

// A surface type's material factors. No default, because ABSENT is meaningful:
// it leaves the set's ORM map authoritative (a value replaces the draw's factor,
// which the shader then multiplies over the map). Neither is baked, so saving
// one takes effect immediately — DungeonWorld::RefreshSurfaceMaterials.
#define SURFACE_MATERIAL_ROWS                                                        \
	{.key = "metallic", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,     \
	 .help = "Scales the set's metalness; absent leaves its ORM map authoritative.",  \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .neutral = "1"},                          \
	{.key = "roughness", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,    \
	 .help = "Scales the set's roughness; absent leaves its ORM map authoritative.",  \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .neutral = "1"}

// --- walls ------------------------------------------------------------------
// The worn block mesh is baked per texture set, so `texture`, `relief` and
// `wear` all invalidate it (rebakes = the owner re-runs AssetBaker). The old
// `columns` knob (edge pillars baked into every wall) was retired 2026-08-05:
// walls are plain now and a pillar is a DECORATION, so one pillar model serves
// all 54 surface types instead of being baked into each of them.
constexpr FieldSpec kWallFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "The PBR set this surface paints with; its height map drives the worn relief.",
	 .rebakes = true},
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Parallax depth of the painted relief, in units.",
	 .lo = 0.0f, .hi = 0.12f, .step = 0.005f, .def = "0.055"},
	{.key = "relief", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "How far the stones stand proud of the panel, in metres (real "
			 "geometry, so it shows at any angle). `wear` scales this.",
	 .lo = 0.0f, .hi = 0.25f, .step = 0.005f, .def = "0.055", .rebakes = true},
	{.key = "wear", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Scales the relief: 0 = a flat panel, 1 = the full authored depth.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "1", .rebakes = true},
	SURFACE_MATERIAL_ROWS,
};

// --- floors -----------------------------------------------------------------
// Same as walls minus the pillars (a wall-block feature). Ceilings get their own
// table below purely because they are baked at a DEEPER default relief, and a
// `def` that doesn't match what the baker would do makes the slider lie.
constexpr FieldSpec kFloorFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "The PBR set this surface paints with; its height map drives the worn relief.",
	 .rebakes = true},
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Parallax depth of the painted relief, in units.",
	 .lo = 0.0f, .hi = 0.12f, .step = 0.005f, .def = "0.045"},
	{.key = "relief", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "How far the surface stands proud of the panel, in metres (real "
			 "geometry, so it shows at any angle). `wear` scales this.",
	 .lo = 0.0f, .hi = 0.25f, .step = 0.005f, .def = "0.045", .rebakes = true},
	{.key = "wear", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Scales the relief: 0 = a flat panel, 1 = the full authored depth.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "1", .rebakes = true},
	SURFACE_MATERIAL_ROWS,
};

// --- ceilings ---------------------------------------------------------------
// The floor table with a deeper default relief (a rough ceiling hangs further
// down than a floor stands proud — BakeWornBlocks' per-kind default).
constexpr FieldSpec kCeilingFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "The PBR set this surface paints with; its height map drives the worn relief.",
	 .rebakes = true},
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Parallax depth of the painted relief, in units.",
	 .lo = 0.0f, .hi = 0.12f, .step = 0.005f, .def = "0.045"},
	{.key = "relief", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "How far the surface hangs below the panel, in metres (real "
			 "geometry, so it shows at any angle). `wear` scales this.",
	 .lo = 0.0f, .hi = 0.25f, .step = 0.005f, .def = "0.08", .rebakes = true},
	{.key = "wear", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Scales the relief: 0 = a flat panel, 1 = the full authored depth.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "1", .rebakes = true},
	SURFACE_MATERIAL_ROWS,
};

// --- decorations ------------------------------------------------------------
constexpr FieldSpec kDecorationFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	PROP_AUTHORED,
	{.key = "solid", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Blocks the square (the party and monsters can't enter).", .def = "0"},
	{.key = "mount", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Where the brush hangs it: on a wall face, or standing on the floor.",
	 .options = "floor wall", .def = "floor"},
	{.key = "facing_arrow", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Draw the editor's green facing arrow for placements of this type.",
	 .def = "1"},
	{.key = "alpha_test", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Cutout threshold for a masked texture (0 = opaque, no clip).",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0"},
	MATERIAL_ROWS,
};

// --- fixtures ---------------------------------------------------------------
// flame_height / flame_out are UNITS (points on the mesh); flame_scale is a
// dimensionless particle size.
constexpr FieldSpec kFixtureFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	{.key = "part2_model", .kind = FieldKind::Model, .sectionKey = kSectionLook,
	 .help = "Optional second mesh drawn with it (the brazier's coals)."},
	{.key = "part2_texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "PBR set for that second mesh."},
	{.key = "mount", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Wall-mounted (a sconce resolves its wall) or standing on the floor.",
	 .options = "floor wall", .def = "floor"},
	{.key = "flame", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "This kind burns; off places it unlit, with no light or particles.",
	 .def = "1"},
	{.key = "flame_height", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Height of the flame origin on the mesh, in units.",
	 .lo = 0.0f, .hi = 1.5f, .step = 0.002f},
	{.key = "flame_out", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "How far the flame origin stands out from the mesh, in units.",
	 .lo = 0.0f, .hi = 0.3f, .step = 0.002f},
	{.key = "flame_scale", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Particle-effect size (brazier 1.0, sconce ~0.55).",
	 .lo = 0.0f, .hi = 3.0f, .step = 0.05f, .def = "1"},
};

// --- monsters ---------------------------------------------------------------
// Behaviour + animation rows are MonsterConfigDialog's (see the header note).
constexpr FieldSpec kMonsterFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	{.key = "modelscale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Trims the authored size (1 = as authored, in dungeon squares).",
	 .lo = 0.1f, .hi = 4.0f, .step = 0.05f, .def = "1"},
	{.key = "modelyaw", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Turns the mesh so it faces +Z like the engine expects (degrees).",
	 .lo = -180.0f, .hi = 180.0f, .step = 5.0f, .def = "0"},
	{.key = "hp", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Hit points.", .lo = 1.0f, .hi = 200.0f, .step = 1.0f, .def = "10"},
	{.key = "damage", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Damage per landed blow.", .lo = 0.0f, .hi = 50.0f, .step = 1.0f, .def = "3"},
	{.key = "dmgtype", .kind = FieldKind::Enum, .sectionKey = kSectionStats,
	 .help = "Damage type its melee deals (resists key off this).",
	 .options = "slash pierce bash fire earth air water", .def = "bash"},
	{.key = "accuracy", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Chance to land a blow before the defender's evasion.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0.6"},
	{.key = "defense", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Evasion against incoming attacks.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0"},
	{.key = "armor", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Flat damage soak.", .lo = 0.0f, .hi = 20.0f, .step = 1.0f, .def = "0"},
	{.key = "resists", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Per-type resistance, e.g. 'pierce 0.5, slash 0.25' (1 = immune, negative = weakness)."},
	{.key = "reach", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Melee range in squares; 2 lets it strike from one square back.",
	 .lo = 1.0f, .hi = 3.0f, .step = 1.0f, .def = "1"},
	{.key = "attackcd", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Seconds between its attacks.", .lo = 0.2f, .hi = 5.0f, .step = 0.1f, .def = "1.5"},
	{.key = "movecd", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Seconds between its steps.", .lo = 0.1f, .hi = 3.0f, .step = 0.05f, .def = "0.5"},
	{.key = "aggro", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "How many squares away it notices the party.",
	 .lo = 0.0f, .hi = 20.0f, .step = 1.0f, .def = "6"},
	{.key = "iq", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Thinking rate: picks its AI bucket, so a dim monster changes its mind slower.",
	 .lo = 0.0f, .hi = 200.0f, .step = 5.0f, .def = "100"},
	{.key = "faces", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Has a facing (off hides the editor's facing arrow for it).", .def = "1"},
};

// --- buttons ----------------------------------------------------------------
constexpr FieldSpec kButtonFields[] = {
	IDENTITY_DISPLAY, IDENTITY_CATEGORY, PROP_MODEL, PROP_TEXTURE, PROP_SCALE, PROP_AUTHORED,
};

// --- doors ------------------------------------------------------------------
constexpr FieldSpec kDoorFields[] = {
	IDENTITY_DISPLAY, IDENTITY_CATEGORY, PROP_MODEL, PROP_TEXTURE, PROP_SCALE, PROP_AUTHORED,
	{.key = "hidden", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Internal type the palette never offers (the shared door frame).",
	 .def = "0"},
};

// --- stairs / pits ----------------------------------------------------------
constexpr FieldSpec kStairFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	PROP_AUTHORED,
	{.key = "solid", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Blocks the square.", .def = "0"},
	{.key = "up", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Leads UP (also picks which way the map icon's arrow points).",
	 .def = "0"},
	{.key = "pair", .kind = FieldKind::CatalogRef, .sectionKey = kSectionRules,
	 .help = "The type auto-authored on the destination level (a pit pairs with pit_ceiling).",
	 .options = "stairs"},
	{.key = "hole", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Which cell block the mesh builder skips so the shaft shows.",
	 .options = "none floor ceiling", .def = "none"},
	{.key = "traverse", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Stepping on it transitions (off = scenery, e.g. a pit's ceiling hole).",
	 .def = "1"},
	{.key = "fall", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "The transition is a PLUNGE: the camera drops through the shaft first.",
	 .def = "0"},
};

// Items name themselves with a LOC KEY (`name = item.<id>`), not a display
// string — the one category family whose label is translated.
#define ITEM_NAME                                                              \
	{.key = "name", .kind = FieldKind::Text, .sectionKey = kSectionIdentity,     \
	 .help = "Loc key of the item's name, by convention item.<id> (add it to every .lang)."}
#define ITEM_WEIGHT                                                            \
	{.key = "weight", .kind = FieldKind::Float, .sectionKey = kSectionStats,     \
	 .help = "Kilograms, against the carry load.",                              \
	 .lo = 0.0f, .hi = 50.0f, .step = 0.1f, .def = "1"}

// --- items (the catch-all: runes, keys, food, containers, ingredients) ------
// Weapons and armor moved to their own catalogs/schemas, so their attack/defense
// fields no longer clutter a rune or an apple.
constexpr FieldSpec kItemFields[] = {
	ITEM_NAME,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	ITEM_WEIGHT,
	{.key = "holdable", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Can be held in a hand slot.", .def = "1"},
	{.key = "command", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Hand-menu verbs this item offers, e.g. 'eat'."},
	{.key = "symbol", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Rune symbol this item teaches (runes only).",
	 .options = "fire earth air water project protect sight"},
	{.key = "capacity", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Container capacity in kilograms.", .lo = 0.0f, .hi = 50.0f, .step = 0.5f},
	{.key = "accepts", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Item categories a container takes, e.g. 'rune'."},
};

// --- weapons ----------------------------------------------------------------
// The attack side of the combat formula (docs/combat.md). category defaults to
// "weapon" so a new one behaves right at runtime.
constexpr FieldSpec kWeaponFields[] = {
	ITEM_NAME,
	{.key = "category", .kind = FieldKind::Text, .sectionKey = kSectionIdentity,
	 .help = "Groups it in the palette; keep 'weapon' so the runtime treats it as one.",
	 .def = "weapon"},
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	ITEM_WEIGHT,
	{.key = "damage", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Base weapon damage, before stats and skill.",
	 .lo = 0.0f, .hi = 50.0f, .step = 1.0f, .def = "5"},
	{.key = "speed", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Swing pace multiplier (higher = slower).", .lo = 0.1f, .hi = 3.0f, .step = 0.05f, .def = "1"},
	{.key = "skill", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Weapon class trained + scaled by (e.g. blade, blunt); empty trains nothing."},
	{.key = "stats", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Attributes whose average boosts its damage, e.g. 'str dex'."},
	// Enchantment (docs/effects.md): an element riding every landed blow — bonus
	// damage of that type, and the flavour its on-hit effects arrive with.
	{.key = "element", .kind = FieldKind::Enum, .sectionKey = kSectionStats,
	 .help = "Element carried into every landed blow; none = a plain weapon.",
	 .options = "none fire earth air water", .def = "none"},
	{.key = "element_bonus", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Elemental damage added, as a fraction of the blow (resisted by element).",
	 .lo = 0.0f, .hi = 2.0f, .step = 0.05f, .def = "0"},
	{.key = "on_hit", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Effects a landed blow leaves, by id: 'burn 3 6 0.5, bleed 2 10'."},
	{.key = "reach", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "melee = adjacent only; polearm strikes from the rear rank; ranged flies.",
	 .options = "melee polearm ranged", .def = "melee"},
	{.key = "command", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Attack verbs the hand menu offers, e.g. 'stab, slash'."},
	{.key = "holdable", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Can be held in a hand slot (weapons should be on).", .def = "1"},
};

// --- armor ------------------------------------------------------------------
// The worn defender side of the formula: a flat soak + per-type resists.
constexpr FieldSpec kArmorFields[] = {
	ITEM_NAME,
	{.key = "category", .kind = FieldKind::Text, .sectionKey = kSectionIdentity,
	 .help = "Groups it in the palette (armor / clothing); keep it a worn kind.",
	 .def = "armor"},
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	ITEM_WEIGHT,
	{.key = "armor", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Flat damage soak when worn.", .lo = 0.0f, .hi = 20.0f, .step = 0.25f, .def = "1"},
	{.key = "resists", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Per-type resistance granted when worn, e.g. 'slash 0.2, fire 0.1'."},
};

// --- status effects ---------------------------------------------------------
// What an effect IS and how it reads (docs/effects.md). Deliberately no dps or
// duration: those are authored at the SOURCE that inflicts it — a weapon's or a
// monster's `on_hit` — because the same burn is a different fire on every blade.
// An effect's BEHAVIOUR is its class, so this catalog cannot create one.
constexpr FieldSpec kEffectFields[] = {
	{.key = "name", .kind = FieldKind::Text, .sectionKey = kSectionIdentity,
	 .help = "Display name loc key; the sheet appends '.desc' for the long form."},
	{.key = "icon", .kind = FieldKind::Text, .sectionKey = kSectionLook,
	 .help = "Item id whose baked icon it borrows in the HUD strip; empty = a tinted square."},
	{.key = "school", .kind = FieldKind::Enum, .sectionKey = kSectionLook,
	 .help = "Tint (and flavour) when the source lends none of its own.",
	 .options = "fire earth air water", .def = "fire"},
	{.key = "plume", .kind = FieldKind::Bool, .sectionKey = kSectionLook,
	 .help = "Its bearer visibly burns: a flame plume and its own coloured light.",
	 .def = "0"},
	// NOTE the shown value is the schema default when the entry omits the field,
	// and `burn` omits it deliberately — its class resolves the type per
	// instance. Hence the second sentence: without it the row reads as a claim
	// that a burn deals bash damage.
	{.key = "damage_type", .kind = FieldKind::Enum, .sectionKey = kSectionStats,
	 .help = "What a DoT is RESISTED as — not its tint (bleeding is pierce). "
			 "A burn ignores this: it answers to whatever element lit it.",
	 .options = "slash pierce bash fire earth air water", .def = "bash"},
	{.key = "stacking", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "refresh = replace it; school = replace only the same school's; stack = pile up.",
	 .options = "refresh school stack", .def = "refresh"},
	{.key = "apply_party", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Loc key announcing it took hold on a MEMBER; takes their name."},
	{.key = "apply_monster", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Loc key announcing it took hold on a MONSTER; takes its name."},
};

// --- wall features ----------------------------------------------------------
constexpr FieldSpec kWallFeatureFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "model", .kind = FieldKind::Model, .sectionKey = kSectionLook,
	 .help = "The panel mesh stamped in place of the plain wall panel."},
	{.key = "bore", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "A see-through window bored THROUGH the block, not a pocket in its face.",
	 .def = "0"},
};
} // namespace

std::span<const FieldSpec> SchemaFor(std::string_view catalogKey) {
	if (catalogKey == "walls") return kWallFields;
	if (catalogKey == "floors") return kFloorFields;
	if (catalogKey == "ceilings") return kCeilingFields;
	if (catalogKey == "decorations") return kDecorationFields;
	if (catalogKey == "fixtures") return kFixtureFields;
	if (catalogKey == "monsters") return kMonsterFields;
	if (catalogKey == "buttons") return kButtonFields;
	if (catalogKey == "doors") return kDoorFields;
	if (catalogKey == "stairs") return kStairFields;
	if (catalogKey == "items") return kItemFields;
	if (catalogKey == "weapons") return kWeaponFields;
	if (catalogKey == "armor") return kArmorFields;
	if (catalogKey == "wallfeatures") return kWallFeatureFields;
	if (catalogKey == "effects") return kEffectFields;
	return {};
}

std::string PrettyFieldName(std::string_view key) {
	std::string out(key);
	for (char& c : out)
		if (c == '_') c = ' ';
	if (!out.empty())
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	return out;
}

} // namespace dungeon::game
