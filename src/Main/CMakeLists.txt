add_executable(Dungeon WIN32
	Main.cpp
)
target_link_libraries(Dungeon PRIVATE Game)

# Visual Studio: F5 should launch the game from its output directory.
set_target_properties(Dungeon PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:Dungeon>
)

# Ship the assets directory next to the executable.
add_custom_command(TARGET Dungeon POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
			${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Dungeon>/assets
)
