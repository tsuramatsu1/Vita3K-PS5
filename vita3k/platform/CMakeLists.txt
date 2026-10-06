add_library(platform STATIC src/platform.cpp)

if(VITA3K_PS5_PORT)
	target_sources(platform PRIVATE src/ps5_libc.cpp src/ps5_pad.cpp src/ps5_platform.cpp)
	# The payload SDK fork's platform layer: libc functions no system module exports, klog, direct memory
	target_link_libraries(platform PUBLIC ps5platform)
	target_link_libraries(platform PRIVATE util)
else()
	target_sources(platform PRIVATE src/desktop_pad.cpp src/desktop_platform.cpp)
endif()

target_include_directories(platform PUBLIC include)
