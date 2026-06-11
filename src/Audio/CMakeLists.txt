add_library(Audio STATIC
	AudioEngine.cpp
)
target_link_libraries(Audio PUBLIC Core Assets PRIVATE xaudio2.lib)
