add_library(Game STATIC
    DungeonMap.cpp
    DungeonMeshBuilder.cpp
    Game.cpp
    Party.cpp
)
target_link_libraries(Game PUBLIC Core Platform Assets Animation Graphics UI Audio)
