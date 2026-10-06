add_library(
    audio
    STATIC
    src/audio.cpp
    src/impl/sdl_audio.cpp
    src/impl/cubeb_audio.cpp)

if(VITA3K_PS5_PORT)
	target_sources(audio PRIVATE src/impl/ps5_audio.cpp)
endif()

target_include_directories(audio PUBLIC include)
target_link_libraries(audio PRIVATE util cubeb SDL3::SDL3)
