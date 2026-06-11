add_library(Platform STATIC
	Window.cpp
	Input.cpp
)
target_link_libraries(Platform PUBLIC Core)
