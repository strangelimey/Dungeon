add_executable(Dungeon WIN32
	Main.cpp
)
target_link_libraries(Dungeon PRIVATE Game)

# Show the language files in the IDE for editing — nothing compiles them (the
# game loads them from paths::AssetsDir at runtime). CONFIGURE_DEPENDS re-globs
# when a new .lang file appears, so adding a language needs no CMake edit.
file(GLOB LANG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/lang/*.lang)
target_sources(Dungeon PRIVATE ${LANG_FILES})
set_source_files_properties(${LANG_FILES} PROPERTIES HEADER_FILE_ONLY ON)
source_group("lang" FILES ${LANG_FILES})

# Visual Studio: F5 should launch the game from its output directory.
set_target_properties(Dungeon PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:Dungeon>
)

# No post-build asset copy: the game runs directly out of ${CMAKE_SOURCE_DIR}/assets
# (baked in as DN_ASSETS_DIR, see src/Core/CMakeLists.txt), so every config shares
# one copy. PACKAGING a build is what copies assets\ beside the exe — paths::AssetsDir
# falls back to that when the baked-in source path isn't there.
