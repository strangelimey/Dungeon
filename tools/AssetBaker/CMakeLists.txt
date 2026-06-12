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
	SoundBaker.cpp
	TextureBaker.cpp
	TitleBaker.cpp
)
target_link_libraries(AssetBaker PRIVATE Core Assets ThirdPartyImpl)
