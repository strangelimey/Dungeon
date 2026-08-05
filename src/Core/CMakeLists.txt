add_library(Core STATIC
	AllocTrack.cpp
	Loc.cpp
	Log.cpp
	Paths.cpp
	Time.cpp
	StringUtil.cpp
	ThreadManager.cpp
)
target_include_directories(Core PUBLIC ${CMAKE_SOURCE_DIR}/src)
# AllocTrack: SymFromAddr/SymGetLineFromAddr64 name the call site that broke a
# steady-state frame (DbgHelp is only touched when one actually does).
target_link_libraries(Core PRIVATE dbghelp)
# The repo's source assets dir, so a dev build's editor can write edits back to
# the git tree (paths::RepoAssetsDir); empty/undefined in a shipped build.
target_compile_definitions(Core PRIVATE DN_REPO_ASSETS="${CMAKE_SOURCE_DIR}/assets")
# The ONE assets directory every build config reads and writes (paths::AssetsDir).
# Pointing it at the source tree is what lets debug/release/vs/any future profile
# config share a SINGLE copy instead of each linking its own duplicate — see the
# comment in Paths.cpp. A packaged build falls back to assets\ beside the exe when
# this path is absent, so baking in a dev path costs a shipped build nothing.
target_compile_definitions(Core PRIVATE DN_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
# Heap allocation counting (Core/AllocTrack): on in Debug, and available to a
# Release build via -DDN_TRACK_ALLOCS=ON for a measurement run at real speed.
# PUBLIC because the header's alloc::kEnabled must agree with the TU that
# replaces ::operator new — a mismatch would report zero and look clean.
target_compile_definitions(Core PUBLIC
	$<$<OR:$<CONFIG:Debug>,$<BOOL:${DN_TRACK_ALLOCS}>>:DN_ALLOC_TRACK=1>)
