# Dice regression harness (not part of the game). Compiles the real roll engine
# straight in — the Bc7Test pattern — so what is measured is the shipping code
# and not a copy of it. Links Core only: Game/Roll.cpp is deliberately pure and
# must STAY that way, so a link error here is the wall doing its job.
#   RollTest.exe [--self-test]   - one verdict line (tools/Common/Verdict.h), exit 0 = PASS
# Combat.cpp comes in too so the hit-rate section measures the SHIPPING
# ResolveAttack — the margin multiplier and the accuracy->d100 mapping are the
# numbers that decide how the game feels, and a copy of them here would be
# worth nothing. It links because the damage-type BOOK lives in its own TU
# (DamageTypes.cpp): the resolver itself touches no catalog, no file layer and
# no Assets, which is exactly the purity Combat.h claims.
# Defense.cpp comes in for the same reason: the armor floor, the two-hand MAX
# rule and the training-loop split are DECISIONS, and a copy of them here would
# measure the copy. It was lifted out of DungeonWorld precisely so it could be
# linked without the map, the monsters and the catalogs coming with it — so if
# this ever stops linking, something world-shaped has been added to it.
# Resource.cpp joins them for the edge it shares with the armor floor: a curve
# whose cap is zero falls back to an UNBOUNDED straight line, which is right for
# a curve and ruinous for a pool, so "a zero cap switches the practice off" is a
# decision and not a sum. It also holds the inverse the save loader depends on —
# a pre-v17 save recovers its authored base by subtracting Contribution — and a
# formula and its inverse living in two files is how they drift.
# Mishap.cpp is that same bargain once more: the fumble PARSER is what content
# authors actually hit, and the severity rule decides how often a fight goes
# sideways. What it deliberately does NOT hold — dropping a weapon, finding the
# ally a wild swing catches — is the half that needs the world, and that half
# stayed in DungeonWorld beside the effects adapters.
# DamageLedger.cpp is here for the same bargain a third time: it is the machinery
# that turns docs/effects.md's one-pipeline invariant into a checked rule, so the
# checker itself has to be checked. It links because it is deliberately pure —
# Core/Types.h and the standard library — and knows nothing of members, monsters
# or the world it watches. The moment a Character or a DungeonWorld appears in
# it, this line stops compiling, which is the wall doing its job.
# Generate.cpp (tool-refinement Phase 7) for generate::Populate, the populate-
# only pass: the level generator was always pure (Params in, a grid and records
# out), so linking it here costs nothing and measures the shipping rules.
# LightProfile.cpp (lighting-updates Phase 2): a light's look parsed from its
# lights.cat fields and its pulse maths - pure, and what every light goes
# through, so the parse and the pulses are measured where they ship.
# Graphics/LightTiles.cpp (Phase 3): the screen-tile light binning the scene
# shader trusts. Its one promise - a light is never MISSING from a tile it
# reaches - is a geometric claim, so it is checked here by sampling.
# Trail.cpp (Phase 4): what a thing in flight sheds, parsed from trails.cat,
# and each shape's defaults - pure, beside the light profile it reads with.
# Assets/NodeTransform.cpp (code-review C253): the one bake of a glTF node's
# transform into its mesh - normals by the inverse-transpose, a mirror's
# winding put back, a skinned mesh left alone. Its own TU so it links here
# without the loader (cgltf, the image decoders) coming with it.
add_executable(RollTest
	Main.cpp
	${CMAKE_SOURCE_DIR}/src/Game/DamageLedger.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Roll.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Combat.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Curve.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Defense.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Mishap.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Blast.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Resource.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Power.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Style.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Carve.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Generate.cpp
	${CMAKE_SOURCE_DIR}/src/Game/LightProfile.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Trail.cpp
	${CMAKE_SOURCE_DIR}/src/Graphics/LightTiles.cpp
	${CMAKE_SOURCE_DIR}/src/Assets/NodeTransform.cpp
)
# NOT linked, and the attempt is worth recording: pulling Balance.cpp in for the
# attacker's type axis dragged Catalog -> assets::ReadBinaryFile, i.e. the file
# layer. That is the wall doing exactly what it was built for. The arithmetic moved
# into Defense.cpp instead (defense::Potent) with Balance::Potent as a thin adapter
# passing its clamp - the same split the armor floor already uses. The knob
# DEFAULTS need no link at all: they are Game/BalanceKnobs.h, a header that
# includes nothing (code-review C423 - the harness used to retype them).
# src/ on the include path so the harness and the engine both say "Game/Roll.h";
# tools/ for the shared verdict line, "Common/Verdict.h".
target_include_directories(RollTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(RollTest PRIVATE Core)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(RollTest ${CMAKE_CURRENT_SOURCE_DIR})
