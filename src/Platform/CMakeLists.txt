add_library(Platform STATIC
	Window.cpp
	Input.cpp
)
target_link_libraries(Platform PUBLIC Core PRIVATE xinput9_1_0.lib)
