# Offline asset generator: writes textures, sounds, and glTF models into the
# assets/ directory. Run manually after changing generation code:
#   AssetBaker.exe <path-to-assets-dir>
add_executable(AssetBaker
	Bc7Encoder.cpp
	GltfWriter.cpp
	ImportTextures.cpp
	Main.cpp
	MipBaker.cpp
	ModelBaker.cpp
	ModelImport.cpp
	RuneBaker.cpp
	SoundBaker.cpp
	TextureBaker.cpp
)
target_link_libraries(AssetBaker PRIVATE Core Assets ThirdPartyImpl)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(AssetBaker ${CMAKE_CURRENT_SOURCE_DIR})
