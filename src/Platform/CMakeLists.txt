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

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Platform ${CMAKE_CURRENT_SOURCE_DIR})
