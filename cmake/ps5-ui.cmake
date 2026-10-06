# The PS5 front end's UI kit: BlackBearReloaded's ps5-homebrew-ui through mihawk-99's PS5_VKHomebrewUI fork, which adds
# its Vulkan renderer, at the revision PS5_VulkanTemplate pins (ps5/ui/setup-kit.sh). The kit, and so the PS5 build,
# is GPL-3.0-or-later.
#
# Only the kit's drawing, widgets and input are built (src/gfx, src/ui, src/core, src/audio): not its own app, designs,
# demo content, OpenGL backend or console layer; vita3k/ps5/frontend stands in for the last. Its Vulkan calls go
# through volk, filled from the RADV the title links (there is no Vulkan loader on the console).

set(PS5_UI_KIT_REVISION 4451c261ac8631c698291ee765a37456f25bc22c)
set(PS5_UI_KIT "${CMAKE_BINARY_DIR}/external/hui")
set(PS5_UI_KIT_GIT "${CMAKE_BINARY_DIR}/external/PS5_VKHomebrewUI.git")
set(PS5_VOLK_TAG vulkan-sdk-1.4.328.0) # matches external/VulkanMemoryAllocator-Hpp/Vulkan-Headers
set(PS5_VOLK "${CMAKE_BINARY_DIR}/external/volk")

if(NOT EXISTS "${PS5_UI_KIT}/.revision" OR NOT EXISTS "${PS5_UI_KIT}/src/gfx/vk/vk_renderer.cpp")
	message(STATUS "Fetching PS5_VKHomebrewUI ${PS5_UI_KIT_REVISION}...")
	if(NOT EXISTS "${PS5_UI_KIT_GIT}")
		execute_process(COMMAND git clone --quiet --bare https://github.com/mihawk-99/PS5_VKHomebrewUI.git "${PS5_UI_KIT_GIT}"
			RESULT_VARIABLE PS5_UI_RESULT)
	endif()
	file(REMOVE_RECURSE "${PS5_UI_KIT}")
	file(MAKE_DIRECTORY "${PS5_UI_KIT}")
	execute_process(
		COMMAND bash -c "git -C \"$0\" archive \"$1\" src assets/fonts third_party/stb LICENSE THIRD_PARTY_NOTICES.md | tar -x -C \"$2\""
			"${PS5_UI_KIT_GIT}" "${PS5_UI_KIT_REVISION}" "${PS5_UI_KIT}"
		RESULT_VARIABLE PS5_UI_RESULT)
	if(NOT PS5_UI_RESULT EQUAL 0 OR NOT EXISTS "${PS5_UI_KIT}/src/gfx/vk/vk_renderer.cpp")
		message(FATAL_ERROR "Could not fetch PS5_VKHomebrewUI ${PS5_UI_KIT_REVISION}")
	endif()
	file(WRITE "${PS5_UI_KIT}/.revision" "${PS5_UI_KIT_REVISION}\n")
endif()

if(NOT EXISTS "${PS5_VOLK}/volk.c")
	message(STATUS "Fetching volk ${PS5_VOLK_TAG}...")
	execute_process(
		COMMAND git clone --quiet --depth 1 --branch ${PS5_VOLK_TAG} https://github.com/zeux/volk.git "${PS5_VOLK}"
		RESULT_VARIABLE PS5_VOLK_RESULT)
	if(NOT PS5_VOLK_RESULT EQUAL 0)
		message(FATAL_ERROR "Could not fetch volk ${PS5_VOLK_TAG}")
	endif()
endif()

# The Vulkan commands are volk's function pointers in every unit that draws: none is linked, as no loader exports them
add_library(ps5-volk STATIC "${PS5_VOLK}/volk.c")
target_include_directories(ps5-volk PUBLIC "${PS5_VOLK}")
target_link_libraries(ps5-volk PUBLIC vulkan)
target_compile_definitions(ps5-volk PUBLIC VK_NO_PROTOTYPES)

file(GLOB_RECURSE PS5_UI_KIT_SOURCES
	"${PS5_UI_KIT}/src/gfx/*.cpp" "${PS5_UI_KIT}/src/ui/*.cpp" "${PS5_UI_KIT}/src/core/*.cpp" "${PS5_UI_KIT}/src/audio/*.cpp")
list(FILTER PS5_UI_KIT_SOURCES EXCLUDE REGEX "/src/gfx/(gl_[a-z_]+|backdrop|canvas)\\.cpp$")
add_library(ps5-ui STATIC ${PS5_UI_KIT_SOURCES})
target_include_directories(ps5-ui PUBLIC "${PS5_UI_KIT}/src" PRIVATE "${PS5_UI_KIT}/third_party/stb")
target_compile_features(ps5-ui PUBLIC cxx_std_20)
target_compile_options(ps5-ui PRIVATE -w "SHELL:-include volk.h")
target_link_libraries(ps5-ui PUBLIC ps5-volk)

set(PS5_UI_FONTS "${PS5_UI_KIT}/assets/fonts")
