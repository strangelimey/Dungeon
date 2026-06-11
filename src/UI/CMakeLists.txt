add_library(UI STATIC
    Controls.cpp
    Font.cpp
    UIContext.cpp
)
target_link_libraries(UI PUBLIC Core Platform Graphics PRIVATE ThirdPartyImpl)
