add_library(Game STATIC
	AssetUtil.cpp
	Catalog.cpp
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
	MapView.cpp
	MessageLog.cpp
	Party.cpp
	PartyHud.cpp
	Project.cpp
	SaveGame.cpp
	Serialize.cpp
	SoundBank.cpp
)
target_link_libraries(Game PUBLIC Core Platform Assets Animation Graphics UI Audio)
