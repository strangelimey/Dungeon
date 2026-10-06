# Before/after check for animation storage and sampling (not part of the game).
# Samples every clip of every animated model through the real loader and the
# real anim::Animator and records or compares the skinning palettes.
#   AnimTest.exe --dump <file>      record this build
#   AnimTest.exe --compare <file>   compare against a recording; exit 0 = PASS
#   AnimTest.exe --contract [--self-test]   Play's promises on a built rig (CheckAll `anim`)
add_executable(AnimTest
	Main.cpp
)
target_link_libraries(AnimTest PRIVATE Animation)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here - source_group is directory-scoped.
dn_ide_layout(AnimTest ${CMAKE_CURRENT_SOURCE_DIR})
