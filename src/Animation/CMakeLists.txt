add_library(Animation STATIC
	Animator.cpp
)
target_link_libraries(Animation PUBLIC Core Assets)
