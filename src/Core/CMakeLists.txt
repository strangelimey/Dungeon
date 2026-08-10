add_library(Core STATIC
	AllocTrack.cpp
	CrashHandler.cpp
	Diagnostics.cpp
	Loc.cpp
	Log.cpp
	Paths.cpp
	Profile.cpp
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
# Hierarchical timing (Core/Profile): its own CONFIG rather than on-in-Debug like
# the allocation counters above, because a profiler perturbs what it measures —
# every instrumented scope costs a clock read — so the timings worth trusting come
# from a build carrying the profiler and nothing else. Hence the preset pair:
# debug-profile (readable, but debug-CRT slow) and release-profile (the one whose
# numbers mean anything). PUBLIC for the same reason as DN_ALLOC_TRACK: the macros
# expand in every lib, and a TU disagreeing with Core about prof::kEnabled would
# collect into storage the registry does not believe exists.
# NOTE the vs preset is a MULTI-config generator while this is a configure-time
# cache variable, so the solution is all-or-nothing: generate with -DDN_PROFILE=ON
# to get a profiling solution, and its Debug AND Release both carry it.
target_compile_definitions(Core PUBLIC
	$<$<BOOL:${DN_PROFILE}>:DN_PROFILE=1>)
