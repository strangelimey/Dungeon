# Vendored single-header libraries. ThirdParty exposes the headers; the
# implementations are compiled once into ThirdPartyImpl.
add_library(ThirdParty INTERFACE)
target_include_directories(ThirdParty INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(ThirdPartyImpl STATIC impl.c)
target_link_libraries(ThirdPartyImpl PUBLIC ThirdParty)
if(MSVC)
	# Third-party code is not held to our warning level.
	target_compile_options(ThirdPartyImpl PRIVATE /W0)
endif()
