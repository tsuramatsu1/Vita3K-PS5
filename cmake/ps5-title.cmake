# Links an executable as a PS5 title with the RADV Vulkan driver, and packages it.
#
# The recipe is PS5_Vulkan's (tools/build-radv-title.sh and tools/radv-link.sh), so titles stay in step with the
# driver: its own crt, a PIE layout that exposes the unwind tables to libunwind, RADV linked whole, the SDK's libc++,
# and the platform layer bound in place of the libc functions the console does not export. It is read from
# PS5_Vulkan at configure time rather than copied here.
#
# Prerequisites in PS5_Vulkan: tools/setup-native-dependencies.sh, tools/build-radv.sh release, and
# tools/rebuild-libc.sh (the title's libc.prx and the ps5-native-tool that converts and signs the executable).

get_filename_component(PS5_VULKAN_ROOT_DEFAULT "${PS5_PAYLOAD_SDK}/../../.." ABSOLUTE)
set(PS5_VULKAN_ROOT "${PS5_VULKAN_ROOT_DEFAULT}" CACHE PATH "PS5_Vulkan checkout, which builds RADV and the title tools")
set(PS5_RADV_ARCHIVE "${PS5_VULKAN_ROOT}/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" CACHE FILEPATH "RADV for the PS5")
set(PS5_NATIVE_TOOL "${PS5_VULKAN_ROOT}/build/runtime-shim/ps5-native-tool" CACHE FILEPATH "PS5_Vulkan's ps5-native-tool")
set(PS5_TITLE_LIBC "${PS5_VULKAN_ROOT}/runtime/libc.prx" CACHE FILEPATH "The title's libc module")

foreach(required "${PS5_VULKAN_ROOT}/tools/radv-link.sh" "${PS5_RADV_ARCHIVE}" "${PS5_NATIVE_TOOL}" "${PS5_TITLE_LIBC}")
	if(NOT EXISTS "${required}")
		message(FATAL_ERROR "${required} is missing. Build RADV and the title runtime in PS5_Vulkan first (docs/ps5-porting-plan.md)")
	endif()
endforeach()

function(vita3k_ps5_title target)
	cmake_parse_arguments(PARSE_ARGV 1 TITLE "" "SCE_SYS_DIRECTORY;OUTPUT_DIRECTORY" "ASSET_DIRECTORIES;LIBC_BINDINGS")

	set(work "${CMAKE_CURRENT_BINARY_DIR}/ps5-title")
	file(MAKE_DIRECTORY "${work}")

	# radv_link_recipe fills three arrays: the linker script, the flags, and the inputs that follow the objects.
	# They become response files for the compiler driver, which hands each argument to the linker in order
	execute_process(
		COMMAND bash -c [[
			source "$0/tools/radv-link.sh" && radv_link_recipe "$0" "$1" "$2" || exit 2
			printf -- '-Wl,%s\n' "${radv_linker_script[@]}" "${radv_link_flags[@]}" > "$3/link-head.rsp"
			printf -- '-Wl,%s\n' "${radv_link_inputs[@]}" > "$3/link-tail.rsp"
		]] "${PS5_VULKAN_ROOT}" "${PS5_PAYLOAD_SDK}" "${PS5_RADV_ARCHIVE}" "${work}"
		RESULT_VARIABLE recipe_result)
	if(NOT recipe_result EQUAL 0)
		message(FATAL_ERROR "PS5_Vulkan's RADV link recipe failed (${recipe_result})")
	endif()

	# The recipe links the SDK's platform layer; this build links its copy with dlmalloc renamed (external/CMakeLists.txt)
	get_target_property(platform_library ps5platform IMPORTED_LOCATION)
	file(READ "${work}/link-tail.rsp" link_tail)
	string(REPLACE "${PS5_PAYLOAD_SDK}/target/lib/libps5platform.a" "${platform_library}" link_tail "${link_tail}")
	file(WRITE "${work}/link-tail.rsp" "${link_tail}")

	set(native "${PS5_VULKAN_ROOT}/tooling/native")
	file(APPEND "${work}/link-head.rsp" "-Wl,--eh-frame-hdr\n-Wl,--version-script\n-Wl,${native}/app-symbols.map\n-Wl,--exclude-libs=ALL\n-Wl,-e\n-Wl,_start\n")
	# Mesa's dispatch tables reference every entry point weakly, and the ones RADV leaves out stay undefined. In a
	# PIE they would become imports, which ps5-native-tool requires a system module to provide. With no dynamic linker
	# named, LLD 18 resolves them to NULL instead (PS5_Vulkan's docs/M5_PHASE_B.md measures it)
	file(APPEND "${work}/link-head.rsp" "-Wl,--no-dynamic-linker\n")

	# The title's own libc bindings: each libc name becomes its ps5_ function, which is linked in even though only
	# the binding names it. A bound name stays local, as the recipe's are, because the title converter refuses exports
	if(TITLE_LIBC_BINDINGS)
		set(local_map "{\n    local:\n")
		foreach(name IN LISTS TITLE_LIBC_BINDINGS)
			file(APPEND "${work}/link-head.rsp" "-Wl,--defsym=${name}=ps5_${name}\n-Wl,--undefined=ps5_${name}\n")
			string(APPEND local_map "        ${name};\n")
		endforeach()
		string(APPEND local_map "};\n")
		file(WRITE "${work}/libc-bindings.map" "${local_map}")
		file(APPEND "${work}/link-head.rsp" "-Wl,--version-script\n-Wl,${work}/libc-bindings.map\n")
	endif()

	# AGC comes from system modules; these host-link stubs only name its imports
	foreach(stub libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c)
		string(REPLACE ":" ";" stub "${stub}")
		list(GET stub 0 library)
		list(GET stub 1 source)
		add_custom_command(
			OUTPUT "${work}/${library}.so"
			COMMAND "${CMAKE_C_COMPILER}" -std=c11 -O2 -fPIC -c "${PS5_VULKAN_ROOT}/vendor/ps5/sdk/stubs/${source}" -o "${work}/${library}_stub.o"
			COMMAND "${PS5_PAYLOAD_SDK}/bin/prospero-lld" --shared -soname "${library}.prx" -o "${work}/${library}.so" "${work}/${library}_stub.o"
			DEPENDS "${PS5_VULKAN_ROOT}/vendor/ps5/sdk/stubs/${source}"
			VERBATIM)
		list(APPEND stub_libraries "${work}/${library}.so")
	endforeach()
	add_custom_target(${target}-agc-stubs DEPENDS ${stub_libraries})
	add_dependencies(${target} ${target}-agc-stubs)

	file(GLOB sdk_stub_libraries "${PS5_PAYLOAD_SDK}/target/lib/*.so")
	foreach(library IN LISTS stub_libraries)
		file(APPEND "${work}/link-tail.rsp" "-Wl,${library}\n")
	endforeach()
	file(APPEND "${work}/link-tail.rsp" "-Wl,--as-needed\n")
	foreach(library IN LISTS sdk_stub_libraries)
		file(APPEND "${work}/link-tail.rsp" "-Wl,${library}\n")
	endforeach()

	# The title's entry point and C++ allocation operators, built as PS5_Vulkan builds them
	add_library(${target}-crt OBJECT "${native}/app_crt.cpp" "${native}/app_cpp_runtime.cpp")
	target_compile_options(${target}-crt PRIVATE -std=c++20 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
	target_sources(${target} PRIVATE $<TARGET_OBJECTS:${target}-crt>)

	# The head goes before the objects; the tail after every library, which only the standard libraries are
	target_link_options(${target} PRIVATE -nostdlib -nostartfiles "@${work}/link-head.rsp")
	set(CMAKE_CXX_STANDARD_LIBRARIES "@${work}/link-tail.rsp" PARENT_SCOPE)
	set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${work}/link-head.rsp" "${work}/link-tail.rsp" "${PS5_RADV_ARCHIVE}")

	# The linked PIE becomes the console's ELF, then a signed SELF, in a title folder with its assets
	# sce_sys holds param.json and the icon0.png the shell and installers (ShadowMountPlus) require
	file(READ "${TITLE_SCE_SYS_DIRECTORY}/param.json" param_json)
	string(JSON title_id GET "${param_json}" titleId)
	set(title "${TITLE_OUTPUT_DIRECTORY}/${title_id}")
	# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's, so an import only their stubs define
	# links and is null at run time (PS5_RetroArch tools/build.sh). Refuse such a title; bind the import instead
	# (LIBC_BINDINGS)
	find_program(PS5_NM NAMES llvm-nm llvm-nm-18 REQUIRED)
	set(package_commands
		COMMAND bash "${CMAKE_SOURCE_DIR}/cmake/ps5-check-imports.sh" "${PS5_NM}" "$<TARGET_FILE:${target}>"
			"${PS5_PAYLOAD_SDK}/target/lib" "${work}/libSceAgc.so" "${work}/libSceAgcDriver.so"
		COMMAND "${PS5_NATIVE_TOOL}" link --in "$<TARGET_FILE:${target}>" --out "${work}/eboot.elf"
			--stub-dir "${PS5_PAYLOAD_SDK}/target/lib" --stub "${work}/libSceAgc.so" --stub "${work}/libSceAgcDriver.so"
			--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
		COMMAND ${CMAKE_COMMAND} -E remove_directory "${title}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${title}/sce_sys" "${title}/sce_module"
		COMMAND "${PS5_NATIVE_TOOL}" self --sign --in "${work}/eboot.elf" --out "${title}/eboot.bin" --magic 0x1D3D154F
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${TITLE_SCE_SYS_DIRECTORY}" "${title}/sce_sys"
		COMMAND ${CMAKE_COMMAND} -E copy "${PS5_TITLE_LIBC}" "${title}/sce_module/libc.prx")
	foreach(directory IN LISTS TITLE_ASSET_DIRECTORIES)
		get_filename_component(name "${directory}" NAME)
		list(APPEND package_commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${directory}" "${title}/${name}")
	endforeach()
	add_custom_command(TARGET ${target} POST_BUILD ${package_commands}
		COMMAND "${PS5_NATIVE_TOOL}" self --inspect --file "${title}/eboot.bin"
		COMMENT "Packaging the PS5 title ${title}"
		VERBATIM)
endfunction()
