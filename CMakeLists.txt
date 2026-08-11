cmake_minimum_required(VERSION 3.24)
project(Dungeon LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows-wide defines
add_compile_definitions(
	UNICODE
	_UNICODE
	NOMINMAX
	WIN32_LEAN_AND_MEAN
)

if(MSVC)
	add_compile_options(/W4 /EHsc /MP /utf-8)
	# Treat the most dangerous warnings as errors without being draconian.
	add_compile_options(/we4715) # not all control paths return a value
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ============================================================================
# IDE layout — every header visible, every file where its folder says it is.
#
# A generator shows only what a target LISTS, and the libs list .cpp ONLY. So
# in Solution Explorer every .h was missing outright and the sources landed in
# one flat node — the 21 Spell/*.cpp indistinguishable from everything else,
# and Spell/Flame.h not there at all. That matters here more than in most
# projects: the spell and effect CLASSES are the hand-edited surface of those
# systems (docs/magic system.md), so "can't find the file" is a design problem,
# not a nicety.
#
# Headers are added HEADER_FILE_ONLY — nothing compiles them, they are there to
# be found and opened — and globbed with CONFIGURE_DEPENDS, the same pattern
# src/Main already uses for the .lang files: a new header shows up without a
# CMake edit. The hand-listed .cpp convention is deliberately UNTOUCHED. The
# two cases are not alike: a missing source must fail loudly at link time,
# whereas a missed header only fails to appear in a tree, so globbing is safe
# for exactly one of them.
#
# CALL THIS FROM THE TARGET'S OWN CMakeLists, NEVER FROM HERE. source_group is
# DIRECTORY-SCOPED: the generator builds a target's folders from the groups
# visible in the directory that DEFINED the target, so a call made in this file
# reaches a target in src/<Lib> not at all. It fails in the worst way — CMake
# configures clean, target_sources still adds the headers (that part IS
# cross-directory), and the .vcxproj comes out with all 175 files in two flat
# nodes, which looks enough like success to be missed. Hence the function is
# defined BEFORE add_subdirectory and invoked at the bottom of each lib.
# ============================================================================
function(dn_ide_layout target dir)
	file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${dir}/*.h")
	if(headers)
		target_sources(${target} PRIVATE ${headers})
		set_source_files_properties(${headers} PROPERTIES HEADER_FILE_ONLY ON)
	endif()

	# Group every file this target owns by its real folder. source_group(TREE)
	# needs absolute paths that all live UNDER the tree root, and the hand-listed
	# sources are relative to the target's own dir — so resolve them, and drop
	# anything outside it (Dungeon also carries assets/lang, which keeps the
	# "lang" group src/Main gave it).
	get_target_property(sources ${target} SOURCES)
	set(grouped "")
	foreach(src IN LISTS sources)
		if(IS_ABSOLUTE "${src}")
			set(path "${src}")
		else()
			set(path "${dir}/${src}")
		endif()
		file(RELATIVE_PATH rel "${dir}" "${path}")
		if(NOT rel MATCHES "^\\.\\.")
			list(APPEND grouped "${path}")
		endif()
	endforeach()
	source_group(TREE "${dir}" FILES ${grouped})
endfunction()

# Third-party single-header libraries (interface target + one impl TU)
add_subdirectory(external)

# Engine modules — order matches the dependency graph (see docs/ARCHITECTURE.md)
add_subdirectory(src/Core)
add_subdirectory(src/Platform)
add_subdirectory(src/Assets)
add_subdirectory(src/Animation)
add_subdirectory(src/Graphics)
add_subdirectory(src/UI)
add_subdirectory(src/Audio)
add_subdirectory(src/Game)
add_subdirectory(src/Main)

# Offline tooling (not part of the game).
add_subdirectory(tools/AssetBaker)
add_subdirectory(tools/Bc7Test)
add_subdirectory(tools/DiagTest)
add_subdirectory(tools/RollTest)
add_subdirectory(tools/ThreadStress)

# Visual Studio solution: open with the game as the startup project.
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Dungeon)
