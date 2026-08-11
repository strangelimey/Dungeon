add_library(Audio STATIC
	AudioEngine.cpp
)
target_link_libraries(Audio PUBLIC Core Assets PRIVATE xaudio2.lib)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Audio ${CMAKE_CURRENT_SOURCE_DIR})
