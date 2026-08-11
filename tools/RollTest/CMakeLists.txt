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
add_executable(RollTest
	Main.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Roll.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Combat.cpp
	${CMAKE_SOURCE_DIR}/src/Game/Curve.cpp
)
# src/ on the include path so the harness and the engine both say "Game/Roll.h".
target_include_directories(RollTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RollTest PRIVATE Core)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(RollTest ${CMAKE_CURRENT_SOURCE_DIR})
