cmake_minimum_required(VERSION 3.20)
project(TenClaw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# zlib — required for qcow2 compressed cluster support
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
)
set(LWIP_COMPILER_FLAGS "")
set(LWIP_DEFINITIONS "")
set(LWIP_MBEDTLS_DEFINITIONS "")
set(LWIP_MBEDTLS_INCLUDE_DIRS "")
set(LWIP_EXCLUDE_SLIPIF TRUE)
include(${LWIP_DIR}/src/Filelists.cmake)

if(MSVC)
    target_compile_options(lwipcore PRIVATE /W0)
endif()

set(SOURCES
    src/main.cpp
    src/vmm/vm.cpp
    src/vmm/address_space.cpp
    src/hypervisor/whvp_platform.cpp
    src/hypervisor/whvp_vm.cpp
    src/hypervisor/whvp_vcpu.cpp
    src/arch/x86_64/boot.cpp
    src/device/serial/uart_16550.cpp
    src/device/timer/i8254_pit.cpp
    src/device/rtc/cmos_rtc.cpp
    src/device/irq/ioapic.cpp
    src/arch/x86_64/acpi.cpp
    src/device/acpi/acpi_pm.cpp
    src/device/virtio/virtqueue.cpp
    src/device/virtio/virtio_mmio.cpp
    src/device/virtio/virtio_blk.cpp
    src/device/virtio/disk_image.cpp
    src/device/virtio/raw_image.cpp
    src/device/virtio/qcow2.cpp
    src/device/virtio/virtio_net.cpp
    src/net/net_backend.cpp
)

add_executable(TenClaw ${SOURCES})
target_include_directories(TenClaw PRIVATE src/)

target_include_directories(TenClaw PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_include_directories(TenClaw PRIVATE ${LWIP_INCLUDE_DIRS})
target_link_libraries(TenClaw PRIVATE WinHvPlatform WinHvEmulation zlibstatic lwipcore ws2_32)
