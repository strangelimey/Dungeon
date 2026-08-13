# Regression test for the health record (not part of the game). Exercises
# Core/Diagnostics' ring directly — including concurrent writers against a live
# reader, which is the part that cannot be checked by reading the code.
#   DiagTest.exe   — one verdict line, exit 0 = PASS
add_executable(DiagTest
	Main.cpp
)
target_link_libraries(DiagTest PRIVATE Core)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(DiagTest ${CMAKE_CURRENT_SOURCE_DIR})
