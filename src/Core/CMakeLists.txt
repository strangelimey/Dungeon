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
# Heap allocation counting (Core/AllocTrack): on in Debug, and available to a
# Release build via -DDN_TRACK_ALLOCS=ON for a measurement run at real speed.
# PUBLIC because the header's alloc::kEnabled must agree with the TU that
# replaces ::operator new — a mismatch would report zero and look clean.
target_compile_definitions(Core PUBLIC
	$<$<OR:$<CONFIG:Debug>,$<BOOL:${DN_TRACK_ALLOCS}>>:DN_ALLOC_TRACK=1>)
