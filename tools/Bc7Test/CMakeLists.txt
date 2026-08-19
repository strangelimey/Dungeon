# BC7 encoder regression harness (not part of the game). Compiles the real
# encoder straight in — the ThreadStress pattern — and pairs it with an
# independent decoder, so the thing under test is the shipping code and not a
# copy of it.
#   Bc7Test.exe [--assets <dir>] [--baseline <file>] [--audit] [--self-test]
add_executable(Bc7Test
	Bc7Decode.cpp
	Main.cpp
	${CMAKE_SOURCE_DIR}/tools/AssetBaker/Bc7Encoder.cpp
)
# tools/ on the include path so the harness can name AssetBaker/Bc7Encoder.h
# and AssetBaker/Bc7Tables.h without relative escapes.
target_include_directories(Bc7Test PRIVATE ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(Bc7Test PRIVATE Core Assets ThirdPartyImpl)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Bc7Test ${CMAKE_CURRENT_SOURCE_DIR})
