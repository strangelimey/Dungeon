add_library(Animation STATIC
	Animator.cpp
)
target_link_libraries(Animation PUBLIC Core Assets)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Animation ${CMAKE_CURRENT_SOURCE_DIR})
