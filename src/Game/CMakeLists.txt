add_library(Game STATIC
	AssetUtil.cpp
	Character.cpp
	DevConsole.cpp
	DungeonEntities.cpp
	DungeonMap.cpp
	DungeonMeshBuilder.cpp
	DungeonWorld.cpp
	Entity.cpp
	FireEffect.cpp
	Game.cpp
	GameSettings.cpp
	GameUI.cpp
	Party.cpp
	PartyHud.cpp
	SoundBank.cpp
)
target_link_libraries(Game PUBLIC Core Platform Assets Animation Graphics UI Audio)
