add_library(Platform STATIC
	Window.cpp
	Input.cpp
	PerfMonitor.cpp
)
target_link_libraries(Platform PUBLIC Core)
# PerfMonitor: GetProcessMemoryInfo (Psapi) and GPU-engine counters (PDH).
target_link_libraries(Platform PRIVATE pdh Psapi)
