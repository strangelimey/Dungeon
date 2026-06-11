add_library(Graphics STATIC
    GraphicsDevice.cpp
    Mesh.cpp
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
