# Thread-system stress harness (not part of the game). Drives the real
# Core/ThreadManager + Game/MonsterAI AsyncDirector with synthetic per-bucket
# workloads. MonsterAI.cpp depends only on Core/ThreadManager.h, so it compiles
# straight in here without pulling in the rest of the Game lib.
#   ThreadStress.exe [--self-test]   - one verdict line (tools/Common/Verdict.h)
add_executable(ThreadStress
	Main.cpp
	${CMAKE_SOURCE_DIR}/src/Game/MonsterAI.cpp
)
# tools/ on the include path for the shared verdict line, "Common/Verdict.h".
target_include_directories(ThreadStress PRIVATE ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(ThreadStress PRIVATE Core)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(ThreadStress ${CMAKE_CURRENT_SOURCE_DIR})
