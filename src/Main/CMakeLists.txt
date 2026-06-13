add_executable(Dungeon WIN32
	Main.cpp
)
target_link_libraries(Dungeon PRIVATE Game)

# Show the language files in the IDE for editing — nothing compiles them
# (they ship via the asset copy below). CONFIGURE_DEPENDS re-globs when a
# new .lang file appears, so adding a language needs no CMake edit.
file(GLOB LANG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/lang/*.lang)
target_sources(Dungeon PRIVATE ${LANG_FILES})
set_source_files_properties(${LANG_FILES} PROPERTIES HEADER_FILE_ONLY ON)
source_group("lang" FILES ${LANG_FILES})

# Visual Studio: F5 should launch the game from its output directory.
set_target_properties(Dungeon PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:Dungeon>
)

# Ship the assets directory next to the executable.
add_custom_command(TARGET Dungeon POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
			${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Dungeon>/assets
)
