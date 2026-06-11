add_library(Assets STATIC
    File.cpp
    Image.cpp
    Model.cpp
    ObjLoader.cpp
    Wav.cpp
)
target_link_libraries(Assets PUBLIC Core PRIVATE ThirdPartyImpl)
