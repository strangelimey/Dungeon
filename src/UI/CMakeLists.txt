add_library(UI STATIC
	Controls.cpp
	Font.cpp
	Skin.cpp
	UIContext.cpp
	Widget.cpp
)
target_link_libraries(UI PUBLIC Core Platform Graphics PRIVATE ThirdPartyImpl)
