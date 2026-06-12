add_library(Core STATIC
	Loc.cpp
	Log.cpp
	Paths.cpp
	Time.cpp
	StringUtil.cpp
)
target_include_directories(Core PUBLIC ${CMAKE_SOURCE_DIR}/src)
