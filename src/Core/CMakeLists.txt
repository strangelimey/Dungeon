add_library(Core STATIC
	Loc.cpp
	Log.cpp
	Paths.cpp
	Time.cpp
	StringUtil.cpp
)
target_include_directories(Core PUBLIC ${CMAKE_SOURCE_DIR}/src)
# The repo's source assets dir, so a dev build's editor can write edits back to
# the git tree (paths::RepoAssetsDir); empty/undefined in a shipped build.
target_compile_definitions(Core PRIVATE DN_REPO_ASSETS="${CMAKE_SOURCE_DIR}/assets")
