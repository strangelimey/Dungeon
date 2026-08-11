add_library(Assets STATIC
	Dds.cpp
	File.cpp
	Image.cpp
	Model.cpp
	ObjLoader.cpp
	PbrMaps.cpp
	Wav.cpp
)
target_link_libraries(Assets PUBLIC Core PRIVATE ThirdPartyImpl)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Assets ${CMAKE_CURRENT_SOURCE_DIR})
