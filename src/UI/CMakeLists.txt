add_library(UI STATIC
	ControlIcons.cpp
	Controls.cpp
	Controls_Containers.cpp
	Controls_Fields.cpp
	Controls_Lists.cpp
	Controls_Popups.cpp
	FloatingPanel.cpp
	Font.cpp
	FontLibrary.cpp
	Layout.cpp
	ScrollBar.cpp
	Skin.cpp
	TreeInspector.cpp
	Units.cpp
	UIContext.cpp
	Widget.cpp
)
target_link_libraries(UI PUBLIC Core Platform Graphics PRIVATE ThirdPartyImpl)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(UI ${CMAKE_CURRENT_SOURCE_DIR})
