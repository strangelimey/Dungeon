add_library(Graphics STATIC
	DisplayEnum.cpp
	GpuProfiler.cpp
	GraphicsDevice.cpp
	Mesh.cpp
	ModelPreview.cpp
	ParticleBatch.cpp
	PostProcess.cpp
	Renderer.cpp
	ShaderCompiler.cpp
	SpriteBatch.cpp
	Texture.cpp
	UploadAllocator.cpp
)
target_link_libraries(Graphics
	PUBLIC Core Platform Assets
	PRIVATE d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
)

# Solution Explorer: headers visible, folders mirrored (dn_ide_layout, root
# CMakeLists). MUST be called here — source_group is directory-scoped.
dn_ide_layout(Graphics ${CMAKE_CURRENT_SOURCE_DIR})
