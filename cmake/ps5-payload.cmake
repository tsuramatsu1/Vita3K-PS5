if(NOT DEFINED PS5_PAYLOAD_SDK)
    if(DEFINED ENV{PS5_PAYLOAD_SDK})
        set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}" CACHE PATH "PS5 payload SDK root")
    else()
        set(PS5_PAYLOAD_SDK "/opt/ps5-payload-sdk" CACHE PATH "PS5 payload SDK root")
    endif()
endif()

if(NOT EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
    message(FATAL_ERROR
        "PS5 payload SDK not found. Set PS5_PAYLOAD_SDK to the SDK root."
    )
endif()

set(VITA3K_PS5_PORT ON CACHE BOOL "Build Vita3K for the PS5" FORCE)
# Vita3K uses no C++ modules, and the SDK ships no clang-scan-deps
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(BOOST_CXX_FLAGS
    "--target=x86_64-sie-ps5 --sysroot=${PS5_PAYLOAD_SDK}/target -DBOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF"
    CACHE STRING "Boost compiler flags for the PS5 payload SDK"
)
include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")