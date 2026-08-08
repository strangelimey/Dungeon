add_library(UI STATIC
	ControlIcons.cpp
	Controls.cpp
	Font.cpp
	FontLibrary.cpp
	Layout.cpp
	Skin.cpp
	TreeInspector.cpp
	Units.cpp
	UIContext.cpp
	Widget.cpp
)
target_link_libraries(UI PUBLIC Core Platform Graphics PRIVATE ThirdPartyImpl)
