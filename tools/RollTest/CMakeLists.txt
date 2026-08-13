# Dice regression harness (not part of the game). Compiles the real roll engine
# straight in — the Bc7Test pattern — so what is measured is the shipping code
# and not a copy of it. Links Core only: Game/Roll.cpp is deliberately pure and
# must STAY that way, so a link error here is the wall doing its job.
#   RollTest.exe [--self-test]   — one verdict line, exit 0 = PASS
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
# Mishap.cpp is that same bargain once more: the fumble PARSER is what content
# authors actually hit, and the severity rule decides how often a fight goes
# sideways. What it deliberately does NOT hold — dropping a weapon, finding the
# ally a wild swing catches — is the half that needs the world, and that half
# stayed in DungeonWorld beside the effects adapters.
add_executable(RollTest
	Main.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Roll.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Combat.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Curve.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Defense.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Mishap.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Blast.cpp
)
# NOT linked, and the attempt is worth recording: pulling Balance.cpp in for the
# attacker's type axis dragged Catalog -> assets::ReadBinaryFile, i.e. the file
# layer. That is the wall doing exactly what it was built for. The arithmetic moved
# into Defense.cpp instead (defense::Potent) with Balance::Potent as a thin adapter
# passing its clamp — the same split the armor floor already uses.
# src/ on the include path so the harness and the engine both say "Game/Roll.h".
target_include_directories(RollTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RollTest PRIVATE Core)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(RollTest ${CMAKE_CURRENT_SOURCE_DIR})
