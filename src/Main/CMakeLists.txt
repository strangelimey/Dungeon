add_executable(Dungeon WIN32
    Main.cpp
)
target_link_libraries(Dungeon PRIVATE Game)

# Ship the assets directory next to the executable.
add_custom_command(TARGET Dungeon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Dungeon>/assets
)
