cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(TenBox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put all executables in build root (e.g. x64-debug/) instead of deep src/... paths
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    add_compile_options(/utf-8)
endif()

include(FetchContent)

# Use a persistent cache directory for FetchContent downloads
# This avoids re-downloading dependencies when build directory is deleted
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "FetchContent cache directory")
endif()

# zlib — required for qcow2 compressed cluster support
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

# zstd — required for qcow2 zstd compressed cluster support
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

# lwIP — lightweight TCP/IP stack for NAT networking
FetchContent_Declare(
    lwip
    GIT_REPOSITORY https://github.com/lwip-tcpip/lwip.git
    GIT_TAG        STABLE-2_2_0_RELEASE
)
FetchContent_GetProperties(lwip)
if(NOT lwip_POPULATED)
    FetchContent_Populate(lwip)
endif()

set(LWIP_DIR ${lwip_SOURCE_DIR})
set(LWIP_INCLUDE_DIRS
    ${LWIP_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/net
)
set(LWIP_COMPILER_FLAGS "")
set(LWIP_DEFINITIONS "")
set(LWIP_MBEDTLS_DEFINITIONS "")
set(LWIP_MBEDTLS_INCLUDE_DIRS "")
set(LWIP_EXCLUDE_SLIPIF TRUE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
include(${LWIP_DIR}/src/Filelists.cmake)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

if(MSVC)
    target_compile_options(lwipcore PRIVATE /W0)
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

# nlohmann/json — JSON for Modern C++
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(json)

add_subdirectory(src/core)
add_subdirectory(src/ipc)
add_subdirectory(src/ui/win32)
add_subdirectory(src/platform/tray/windows)
add_subdirectory(src/runtime)
add_subdirectory(src/manager)
