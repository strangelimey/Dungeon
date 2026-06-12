add_library(Game STATIC
	DungeonEntities.cpp
	DungeonMap.cpp
	DungeonMeshBuilder.cpp
	Entity.cpp
	FireEffect.cpp
	Game.cpp
	Party.cpp
)
target_link_libraries(Game PUBLIC Core Platform Assets Animation Graphics UI Audio)
