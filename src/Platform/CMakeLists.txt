add_library(Platform STATIC
	FileDialog.cpp
	Process.cpp
	Window.cpp
	Input.cpp
	PerfMonitor.cpp
)
target_link_libraries(Platform PUBLIC Core)
# PerfMonitor: GetProcessMemoryInfo (Psapi) and GPU-engine counters (PDH);
# FileDialog: IFileOpenDialog (Ole32).
target_link_libraries(Platform PRIVATE pdh Psapi Ole32)
